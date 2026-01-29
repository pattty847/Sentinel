#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <QProcessEnvironment>

namespace {
double getOrderBookTickSize() {
    const QByteArray tickEnv = qgetenv("SENTINEL_ORDERBOOK_TICK_SIZE");
    bool ok = false;
    const double envTick = tickEnv.toDouble(&ok);
    if (ok && envTick > 0.0) {
        return envTick;
    }
    return 0.10;
}

double getOrderBookBandPct() {
    const QByteArray bandEnv = qgetenv("SENTINEL_ORDERBOOK_BAND_PCT");
    bool ok = false;
    const double envBand = bandEnv.toDouble(&ok);
    if (ok && envBand > 0.0) {
        return envBand;
    }
    return 0.30;
}

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

RemoteGridDataSource::RemoteGridDataSource(const QString& host, const QString& port, QObject* parent)
    : IGridDataSource(parent)
    , m_client(host.toStdString(), port.toStdString())
{
    qRegisterMetaType<HeatmapHistoryColumn>("HeatmapHistoryColumn");
    qRegisterMetaType<QVector<HeatmapHistoryColumn>>("QVector<HeatmapHistoryColumn>");
    connect(&m_client, &SentinelStreamClient::tradeReceived, this, &IGridDataSource::tradeReceived);
    // Connect to new specific signals
    connect(&m_client, &SentinelStreamClient::snapshotReceived, this, &RemoteGridDataSource::onSnapshotReceived);
    connect(&m_client, &SentinelStreamClient::l2UpdateReceived, this, &RemoteGridDataSource::onL2UpdateReceived);
    connect(&m_client, &SentinelStreamClient::heatmapSliceReceived, this, &RemoteGridDataSource::onHeatmapSliceReceived);
    connect(&m_client, &SentinelStreamClient::heatmapHistoryReceived, this, &RemoteGridDataSource::onHeatmapHistoryReceived);
    
    connect(&m_client, &SentinelStreamClient::connected,
            this,
            [this]{ emit connectionStatusChanged(true); },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::disconnected,
            this,
            [this]{ emit connectionStatusChanged(false); },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::errorOccurred, this, &IGridDataSource::errorOccurred);
}

void RemoteGridDataSource::connectToServer() {
    m_client.connectToServer();
}

void RemoteGridDataSource::subscribe(const QString& symbol) {
    m_client.subscribe(symbol.toStdString());
    
    // Ensure we have a replica ready - initialize on snapshot for authoritative range.
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
    
    // Re-initialize (clears data) using banded range around best bid/ask.
    const double tickSize = getOrderBookTickSize();
    const double bandPct = getOrderBookBandPct();
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

void RemoteGridDataSource::onHeatmapSliceReceived(const QString& symbol,
                                                  int64_t bucketStartMs,
                                                  int64_t bucketEndMs,
                                                  int64_t timeframeMs,
                                                  int gridWidth,
                                                  int gridHeight,
                                                  double minPrice,
                                                  double maxPrice,
                                                  double tickSize,
                                                  double midPrice,
                                                  double lastTrade,
                                                  const QString& format,
                                                  const QByteArray& column,
                                                  const QByteArray& liquidityColumn,
                                                  double liquidityScale,
                                                  bool reset) {
    emit heatmapSliceReceived(symbol,
                              bucketStartMs,
                              bucketEndMs,
                              timeframeMs,
                              gridWidth,
                              gridHeight,
                              minPrice,
                              maxPrice,
                              tickSize,
                              midPrice,
                              lastTrade,
                              format,
                              column,
                              liquidityColumn,
                              liquidityScale,
                              reset);
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
