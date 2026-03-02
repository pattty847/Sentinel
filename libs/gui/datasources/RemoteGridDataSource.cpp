#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include "../config/GuiConfigStore.hpp"

namespace {
std::pair<double, double> computeBandRange(const std::vector<OrderBookLevel>& bids,
                                           const std::vector<OrderBookLevel>& asks,
                                           double bandPct) {
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    bool hasPrice = false;

    for (const auto& level : bids) {
        if (level.price > bestBid) {
            bestBid = level.price;
        }
        if (!hasPrice) {
            minPrice = level.price;
            maxPrice = level.price;
            hasPrice = true;
        } else {
            minPrice = std::min(minPrice, level.price);
            maxPrice = std::max(maxPrice, level.price);
        }
    }

    for (const auto& level : asks) {
        if (bestAsk <= 0.0 || level.price < bestAsk) {
            bestAsk = level.price;
        }
        if (!hasPrice) {
            minPrice = level.price;
            maxPrice = level.price;
            hasPrice = true;
        } else {
            minPrice = std::min(minPrice, level.price);
            maxPrice = std::max(maxPrice, level.price);
        }
    }

    if (bestBid > 0.0 && bestAsk > 0.0) {
        const double mid = (bestBid + bestAsk) * 0.5;
        const double halfRange = mid * bandPct;
        return {mid - halfRange, mid + halfRange};
    }

    if (hasPrice && maxPrice > minPrice) {
        const double pad = std::max(1e-6, (maxPrice - minPrice) * 0.10);
        return {std::max(0.0, minPrice - pad), maxPrice + pad};
    }

    return {0.0, 1.0};
}
}

RemoteGridDataSource::RemoteGridDataSource(const QString& host, const QString& port,
                                           const QString& caFile, QObject* parent)
    : IGridDataSource(parent)
    , m_client(host.toStdString(), port.toStdString(), caFile.toStdString())
{
    qRegisterMetaType<HeatmapHistoryColumn>("HeatmapHistoryColumn");
    qRegisterMetaType<QVector<HeatmapHistoryColumn>>("QVector<HeatmapHistoryColumn>");
    qRegisterMetaType<HeatmapSlice>("HeatmapSlice");
    qRegisterMetaType<FootprintSlice>("FootprintSlice");
    qRegisterMetaType<TpoSlice>("TpoSlice");
    qRegisterMetaType<trading::OrderUpdate>("trading::OrderUpdate");
    qRegisterMetaType<trading::PositionUpdate>("trading::PositionUpdate");
    m_candleBuffer = std::make_unique<CandleSeriesBuffer>(this);
    connect(&m_client, &SentinelStreamClient::tradeReceived,
            this, &IGridDataSource::tradeReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::snapshotReceived,
            this, &RemoteGridDataSource::onSnapshotReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::l2UpdateReceived,
            this, &RemoteGridDataSource::onL2UpdateReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::heatmapSliceReceived,
            this, &RemoteGridDataSource::onHeatmapSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::footprintSliceReceived,
            this, &RemoteGridDataSource::onFootprintSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::tpoSliceReceived,
            this, &RemoteGridDataSource::onTpoSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::heatmapHistoryReceived,
            this, &RemoteGridDataSource::onHeatmapHistoryReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleBarUpdateReceived,
            this, &RemoteGridDataSource::onCandleBarUpdateReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleBarClosedReceived,
            this, &RemoteGridDataSource::onCandleBarClosedReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleHistoryReceived,
            this, &RemoteGridDataSource::onCandleHistoryReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::serverConfigReceived,
            this, &RemoteGridDataSource::onServerConfigReceived, Qt::QueuedConnection);

    connect(&m_client, &SentinelStreamClient::connected,
            this,
            [this]{ emit connectionStatusChanged(true); },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::disconnected,
            this,
            [this]{ emit connectionStatusChanged(false); },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::errorOccurred,
            this, &IGridDataSource::errorOccurred, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::orderUpdated,
            this, &IGridDataSource::orderUpdated, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::positionUpdated,
            this, &IGridDataSource::positionUpdated, Qt::QueuedConnection);
}

void RemoteGridDataSource::connectToServer() {
    m_client.connectToServer();
}

void RemoteGridDataSource::subscribe(const QString& symbol) {
    m_client.subscribe(symbol.toStdString());

    // Initialize replica on snapshot for authoritative range.
    std::string s = symbol.toStdString();
    if (m_replicaBooks.find(s) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(s, std::make_unique<LiveOrderBook>(s));
    }
}


void RemoteGridDataSource::unsubscribe(const QString& symbol) {
    m_client.unsubscribe(symbol.toStdString());
}

void RemoteGridDataSource::requestHeatmapHistory(const QString& symbol,
                                                 int64_t timeframeMs,
                                                 int64_t endTimeMs,
                                                 int count) {
    m_client.requestHeatmapHistory(symbol.toStdString(), timeframeMs, endTimeMs, count);
}

void RemoteGridDataSource::requestFootprintHistory(const QString& symbol,
                                                   int64_t timeframeMs,
                                                   int64_t endTimeMs,
                                                   int count) {
    m_client.requestFootprintHistory(symbol.toStdString(), timeframeMs, endTimeMs, count);
}

void RemoteGridDataSource::requestCandleHistory(const QString& symbol,
                                                int64_t timeframeSec,
                                                int64_t endTimeSec,
                                                int limit) {
    m_client.requestCandleHistory(symbol.toStdString(), timeframeSec, endTimeSec, limit);
}

void RemoteGridDataSource::requestTpoHistory(const QString& symbol,
                                             int64_t timeframeMs,
                                             int64_t endTimeMs,
                                             int count) {
    m_client.requestTpoHistory(symbol.toStdString(), timeframeMs, endTimeMs, count);
}


void RemoteGridDataSource::sendTradeCommand(const trading::TradeCommand& command) {
    m_client.sendTradeCommand(command);
}
const LiveOrderBook& RemoteGridDataSource::getDirectLiveOrderBook(const std::string& productId) const {
    auto it = m_replicaBooks.find(productId);
    if (it != m_replicaBooks.end() && it->second) {
        return *it->second;
    }
    static LiveOrderBook empty;
    return empty;
}

void RemoteGridDataSource::onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks) {
    std::string symbol = productId.toStdString();

    // Create or reset replica
    if (m_replicaBooks.find(symbol) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(symbol, std::make_unique<LiveOrderBook>(symbol));
    }

    auto& book = *m_replicaBooks[symbol];

    // Re-initialize using banded range around best bid/ask.
    const double tickSize = m_serverConfig.orderbook.tickSize;
    const double bandPct = m_serverConfig.orderbook.bandPct;
    const auto [minPrice, maxPrice] = computeBandRange(bids, asks, bandPct);
    book.initialize(minPrice, maxPrice, tickSize);

    std::vector<BookLevelUpdate> updates;
    updates.reserve(bids.size() + asks.size());

    for (const auto& level : bids) {
        updates.push_back({true, level.price, level.size});
    }
    for (const auto& level : asks) {
        updates.push_back({false, level.price, level.size});
    }

    auto now = std::chrono::system_clock::now();
    std::vector<BookDelta> deltas;
    book.applyUpdates(updates, now, &deltas);
    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }

    sLog_Data(QString("RemoteGridDataSource: Snapshot applied for %1 (%2 bids, %3 asks)")
              .arg(productId).arg(bids.size()).arg(asks.size()));
}

void RemoteGridDataSource::onServerConfigReceived(const ServerConfig& config) {
    m_serverConfig = config;
    GuiConfigStore::instance().setServerConfig(config);
}

void RemoteGridDataSource::onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates) {
    std::string symbol = productId.toStdString();
    auto it = m_replicaBooks.find(symbol);
    if (it == m_replicaBooks.end()) return;

    auto& book = *it->second;

    thread_local std::vector<BookDelta> deltas;
    deltas.clear();

    auto now = std::chrono::system_clock::now();
    book.applyUpdates(updates, now, &deltas);

    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }
}

void RemoteGridDataSource::onHeatmapSliceReceived(const HeatmapSlice& slice) {
    emit heatmapSliceReceived(slice);
}

void RemoteGridDataSource::onFootprintSliceReceived(const FootprintSlice& slice) {
    emit footprintSliceReceived(slice);
}

void RemoteGridDataSource::onTpoSliceReceived(const TpoSlice& slice) {
    emit tpoSliceReceived(slice);
}

void RemoteGridDataSource::onHeatmapHistoryReceived(const QString& symbol,
                                                    int64_t timeframeMs,
                                                    int gridWidth,
                                                    int gridHeight,
                                                    const QVector<SentinelStreamClient::HeatmapHistoryColumn>& columns) {
    QVector<HeatmapHistoryColumn> converted;
    converted.reserve(columns.size());
    for (const auto& col : columns) {
        HeatmapHistoryColumn out;
        out.bucketStartMs = col.bucketStartMs;
        out.bucketEndMs = col.bucketEndMs;
        out.minPrice = col.minPrice;
        out.maxPrice = col.maxPrice;
        out.tickSize = col.tickSize;
        out.intensity = col.intensity;
        out.liquidity = col.liquidity;
        out.liquidityScale = col.liquidityScale;
        converted.push_back(std::move(out));
    }
    emit heatmapHistoryReceived(symbol, timeframeMs, gridWidth, gridHeight, converted);
}

void RemoteGridDataSource::onCandleBarUpdateReceived(const QString& symbol,
                                                     int64_t timeframeSec,
                                                     int64_t,
                                                     int64_t seq,
                                                     const SentinelStreamClient::CandleBar& bar) {
    if (!m_candleBuffer) {
        return;
    }
    CandleSeriesBuffer::CandleBar out;
    out.timeStartMs = bar.timeStartMs;
    out.timeEndMs = bar.timeEndMs;
    out.open = bar.open;
    out.high = bar.high;
    out.low = bar.low;
    out.close = bar.close;
    out.volume = bar.volume;
    out.isClosed = bar.isClosed;
    out.seq = seq;
    m_candleBuffer->applyUpdate(symbol, timeframeSec, out, seq, false);
}

void RemoteGridDataSource::onCandleBarClosedReceived(const QString& symbol,
                                                     int64_t timeframeSec,
                                                     int64_t,
                                                     int64_t seq,
                                                     const SentinelStreamClient::CandleBar& bar) {
    if (!m_candleBuffer) {
        return;
    }
    CandleSeriesBuffer::CandleBar out;
    out.timeStartMs = bar.timeStartMs;
    out.timeEndMs = bar.timeEndMs;
    out.open = bar.open;
    out.high = bar.high;
    out.low = bar.low;
    out.close = bar.close;
    out.volume = bar.volume;
    out.isClosed = true;
    out.seq = seq;
    m_candleBuffer->applyUpdate(symbol, timeframeSec, out, seq, true);
}

void RemoteGridDataSource::onCandleHistoryReceived(const QString& symbol,
                                                   int64_t timeframeSec,
                                                   int64_t startTimeSec,
                                                   int64_t endTimeSec,
                                                   const QVector<SentinelStreamClient::CandleBar>& candles) {
    if (!m_candleBuffer) {
        return;
    }
    int64_t seq = 0;
    for (const auto& bar : candles) {
        CandleSeriesBuffer::CandleBar out;
        out.timeStartMs = bar.timeStartMs;
        out.timeEndMs = bar.timeEndMs;
        out.open = bar.open;
        out.high = bar.high;
        out.low = bar.low;
        out.close = bar.close;
        out.volume = bar.volume;
        out.isClosed = bar.isClosed;
        out.seq = ++seq;
        m_candleBuffer->applyUpdate(symbol, timeframeSec, out, out.seq, out.isClosed);
    }
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Candle history applied: symbol=%1 tfSec=%2 startSec=%3 endSec=%4 count=%5")
                   .arg(symbol)
                   .arg(timeframeSec)
                   .arg(startTimeSec)
                   .arg(endTimeSec)
                   .arg(candles.size()));
    }
}
