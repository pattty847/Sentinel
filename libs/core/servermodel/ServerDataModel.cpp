#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {
int64_t localNowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
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

int64_t ServerDataModel::exchangeNowMs() const {
    const int64_t offset = m_exchangeOffsetMs.load(std::memory_order_relaxed);
    if (offset == 0) {
        return localNowMs();
    }
    return localNowMs() - offset;
}

void ServerDataModel::updateExchangeOffsetMs(int64_t exchangeMs) {
    const int64_t nowMs = localNowMs();
    const int64_t rawOffset = nowMs - exchangeMs;
    if (std::llabs(rawOffset) > 10000) {
        return;
    }
    const int64_t prev = m_exchangeOffsetMs.load(std::memory_order_relaxed);
    if (prev == 0) {
        m_exchangeOffsetMs.store(rawOffset, std::memory_order_relaxed);
        return;
    }
    const int64_t smoothed = static_cast<int64_t>(prev * 0.9 + rawOffset * 0.1);
    m_exchangeOffsetMs.store(smoothed, std::memory_order_relaxed);
}

ServerDataModel::ServerDataModel(const ServerConfig& config, QObject* parent)
    : QObject(parent)
    , m_serverConfig(config)
    , m_logger(std::make_unique<TickBinaryLogger>())
    , m_aggregator(std::make_unique<TimeframeAggregator>(m_serverConfig.heatmap.timeframesMs))
    , m_heatmapStreamer(std::make_unique<HeatmapTwapStreamer>(*this, m_serverConfig.heatmap))
{
    connect(m_aggregator.get(), &TimeframeAggregator::barClosed, this, &ServerDataModel::barClosed);
    connect(m_aggregator.get(), &TimeframeAggregator::barUpdated, this, &ServerDataModel::barUpdated);

    connect(m_heatmapStreamer.get(), &HeatmapTwapStreamer::heatmapSliceReady,
            this, &ServerDataModel::heatmapSliceReady);

    m_heatmapStreamer->start();

    m_candleTimer.setTimerType(Qt::PreciseTimer);
    m_candleTimer.setInterval(250);
    connect(&m_candleTimer, &QTimer::timeout, this, [this]() {
        if (m_aggregator) {
            m_aggregator->tick(exchangeNowMs());
        }
    });
    m_candleTimer.start();
}

ServerDataModel::~ServerDataModel() {
}

SymbolHotData& ServerDataModel::ensureSymbol(const std::string& symbol) {
    {
        std::shared_lock lock(m_mutex);
        auto it = m_symbols.find(symbol);
        if (it != m_symbols.end()) {
            return *it->second;
        }
    }
    
    std::unique_lock lock(m_mutex);
    auto [it, inserted] = m_symbols.try_emplace(symbol, std::make_unique<SymbolHotData>(symbol));
    if (inserted) {
        sLog_Data("ServerDataModel: New symbol tracked: " << symbol);
    }
    return *it->second;
}

std::vector<std::string> ServerDataModel::getSymbolsSnapshot() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> symbols;
    symbols.reserve(m_symbols.size());
    for (const auto& [key, _] : m_symbols) {
        symbols.push_back(key);
    }
    return symbols;
}

const LiveOrderBook& ServerDataModel::getLiveOrderBook(const std::string& symbol) {
    return ensureSymbol(symbol).liveBook;
}

std::vector<OHLCVBar> ServerDataModel::getHistory(const std::string& symbol, Timeframe tf, size_t limit) const {
    if (m_aggregator) {
        return m_aggregator->getHistory(symbol, tf, limit);
    }
    return {};
}

bool ServerDataModel::getHeatmapHistory(const std::string& symbol,
                                        int64_t timeframeMs,
                                        int64_t endTimeMs,
                                        int count,
                                        int& outGridWidth,
                                        int& outGridHeight,
                                        std::vector<HeatmapTwapStreamer::HistoryColumn>& out) const {
    if (!m_heatmapStreamer) {
        return false;
    }
    return m_heatmapStreamer->fetchHistory(symbol, timeframeMs, endTimeMs, count,
                                           outGridWidth, outGridHeight, out);
}

void ServerDataModel::onTrade(const Trade& trade) {
    if (m_logger) {
        m_logger->logTrade(trade);
    }

    const int64_t exchangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    updateExchangeOffsetMs(exchangeMs);

    if (m_aggregator) {
        m_aggregator->onTrade(trade);
    }
    
    auto& data = ensureSymbol(trade.product_id);
    data.lastTradePrice = trade.price;
    
    emit tradeBroadcast(trade);
}

void ServerDataModel::onLiveOrderBookLevelUpdates(const QString& productId,
                                                  const std::vector<BookLevelUpdate>& updates,
                                                  qint64 exchangeMs) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);

    updateExchangeOffsetMs(static_cast<int64_t>(exchangeMs));

    if (data.liveBook.getTickSize() <= 0.0) {
        sLog_Warning("Order book update ignored - book not initialized for " << symbol);
        return;
    }

    if (updates.empty()) {
        return;
    }

    const auto timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(exchangeMs));
    std::vector<BookDelta> deltas;
    data.liveBook.applyUpdates(updates, timestamp, &deltas);
    if (!deltas.empty()) {
        emit bookUpdateBroadcast(productId, deltas);
    }
}

void ServerDataModel::onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);
    
    const double tickSize = m_serverConfig.orderbook.tickSize;
    const double bandPct = m_serverConfig.orderbook.bandPct;
    const auto [minPrice, maxPrice] = computeBandRange(bids, asks, bandPct);
    data.liveBook.initialize(minPrice, maxPrice, tickSize);
    
    std::vector<BookLevelUpdate> updates;
    updates.reserve(bids.size() + asks.size());
    
    for (const auto& level : bids) {
        updates.push_back({true, level.price, level.size});
    }
    for (const auto& level : asks) {
        updates.push_back({false, level.price, level.size});
    }
    
    auto now = std::chrono::system_clock::now();
    data.liveBook.applyUpdates(updates, now, nullptr);
    
    sLog_Data(QString("ServerDataModel: Initialized book for %1 with %2 bids, %3 asks")
              .arg(productId).arg(bids.size()).arg(asks.size()));
}
