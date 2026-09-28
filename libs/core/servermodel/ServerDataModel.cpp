#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
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
        // Local clock or exchange timestamp is off by >10 s; exchangeNowMs()
        // keeps the previous offset. Can repeat per message; rate limited.
        sLog_DataN(10000, "Exchange clock offset out of range, ignored: offsetMs=" << rawOffset
                   << " exchangeMs=" << exchangeMs << " localMs=" << nowMs);
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
    int64_t maxTfMs = std::max<int64_t>(1000, m_serverConfig.heatmap.activeTimeframeMs);
    for (const int64_t tf : m_serverConfig.heatmap.timeframesMs) {
        if (tf > maxTfMs) {
            maxTfMs = tf;
        }
    }
    const int64_t gridSpanMs = maxTfMs * std::max<int64_t>(1024, m_serverConfig.heatmap.gridWidth);
    m_footprintTradeRetentionMs = std::clamp<int64_t>(gridSpanMs * 2, 300'000, 86'400'000);

    connect(m_aggregator.get(), &TimeframeAggregator::barClosed, this, &ServerDataModel::barClosed);
    connect(m_aggregator.get(), &TimeframeAggregator::barUpdated, this, &ServerDataModel::barUpdated);

    connect(m_heatmapStreamer.get(), &HeatmapTwapStreamer::heatmapSliceReady,
            this, &ServerDataModel::heatmapSliceReady);

    // F1 phase 2: prime the in-RAM heatmap rings from any persisted columns
    // before the streamer starts sampling. No-op when persistence is disabled.
    m_heatmapStreamer->bootstrapFromDisk(m_serverConfig.defaultSymbols);

    m_heatmapStreamer->start();

    m_candleTimer.setTimerType(Qt::PreciseTimer);
    m_candleTimer.setInterval(250);
    connect(&m_candleTimer, &QTimer::timeout, this, [this]() {
        if (m_aggregator) {
            m_aggregator->tick(exchangeNowMs());
        }
    });
    m_candleTimer.start();

    startRecorder();
}

ServerDataModel::~ServerDataModel() {
}

namespace {
// "/Volumes/T7/..." is only usable while that volume is mounted; writing there
// otherwise would silently fill the boot disk under /Volumes.
bool volumeMounted(const std::filesystem::path& dir) {
    auto it = dir.begin();
    if (dir.is_absolute() && it != dir.end() && ++it != dir.end() && *it == "Volumes" && ++it != dir.end()) {
        return std::filesystem::is_directory(std::filesystem::path("/Volumes") / *it);
    }
    return true;
}

std::vector<recording::Level> toRecorderLevels(const std::vector<OrderBookLevel>& bids,
                                               const std::vector<OrderBookLevel>& asks) {
    std::vector<recording::Level> levels;
    levels.reserve(bids.size() + asks.size());
    for (const auto& l : bids) levels.push_back({true, l.price, l.size});
    for (const auto& l : asks) levels.push_back({false, l.price, l.size});
    return levels;
}
} // namespace

void ServerDataModel::startRecorder() {
    const auto& rc = m_serverConfig.recording;
    if (!rc.enabled) {
        sLog_App("Recording v2 disabled (recording.enabled=false)");
        return;
    }
    std::filesystem::path dir = rc.dir;
    if (!volumeMounted(dir)) {
        if (rc.fallbackDir.empty()) {
            sLog_Warning("Recording v2 not started: volume for " << dir.string()
                         << " is not mounted and recording.fallback_dir is empty");
            return;
        }
        sLog_Warning("Recording v2: volume for " << dir.string() << " not mounted, using fallback "
                     << rc.fallbackDir);
        dir = rc.fallbackDir;
    }
    recording::RecorderConfig cfg;
    cfg.root = dir;
    cfg.priceScale = rc.priceScale;
    cfg.sizeScale = {rc.sizeFloor, rc.codesPerOctave};
    cfg.latenessMs = rc.latenessMs;
    const auto units = [&](double dollars) { return static_cast<int64_t>(std::llround(dollars * rc.priceScale)); };
    cfg.layers = {
        {"near", units(rc.nearTick), 1.0 - rc.nearPct, 1.0 + rc.nearPct, false},
        {"deep", units(rc.deepTick), rc.deepLowFrac, rc.deepHighMult, true},
    };
    try {
        m_recorder = std::make_unique<recording::BookRecorder>(std::move(cfg));
        m_recordingDir = dir;
    } catch (const std::exception& e) {
        sLog_Error("Recording v2 failed to start: dir=" << dir.string() << " error=" << e.what());
        return;
    }
    sLog_App("Recording v2 started: dir=" << dir.string()
             << " near=" << rc.nearTick << "@+/-" << rc.nearPct * 100 << "%"
             << " deep=" << rc.deepTick << "@[" << rc.deepLowFrac << "x.." << rc.deepHighMult << "x]");

    m_recorderTimer.setInterval(250);
    connect(&m_recorderTimer, &QTimer::timeout, this, [this]() {
        if (!m_recorder) return;
        m_recorder->onTick(localNowMs());
        if (++m_recorderTicks % 240 == 0) {  // once a minute
            const auto s = m_recorder->stats();
            sLog_Data("Recording v2 stats: columns=" << s.columnsWritten << " late=" << s.lateEvents
                      << " backward=" << s.backwardSteps << " queueDrops=" << s.queueDrops
                      << " invalidations=" << s.invalidations << " diskErrors=" << s.diskErrors);
        }
    });
    m_recorderTimer.start();
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

std::vector<OHLCVBar> ServerDataModel::getHistory(const std::string& symbol, int64_t timeframeMs, size_t limit) const {
    if (m_aggregator) {
        return m_aggregator->getHistory(symbol, timeframeMs, limit);
    }
    return {};
}

bool ServerDataModel::getHeatmapHistory(const std::string& symbol,
                                        int64_t timeframeMs,
                                        int64_t endTimeMs,
                                        int count,
                                        int& outGridWidth,
                                        int& outGridHeight,
                                        std::vector<HeatmapTwapStreamer::HistoryColumn>& out,
                                        int64_t startTimeMs) const {
    if (!m_heatmapStreamer) {
        return false;
    }
    return m_heatmapStreamer->fetchHistory(symbol, timeframeMs, endTimeMs, count,
                                           outGridWidth, outGridHeight, out, startTimeMs);
}

int64_t ServerDataModel::oldestHeatmapPersistedMs(const std::string& symbol,
                                                  int64_t timeframeMs) const {
    if (!m_heatmapStreamer) return 0;
    return m_heatmapStreamer->oldestPersistedMs(symbol, timeframeMs);
}

bool ServerDataModel::collectFootprintTrades(const std::string& symbol,
                                             int64_t startTimeMs,
                                             int64_t endTimeMs,
                                             std::vector<FootprintTradeSample>& out) const {
    out.clear();
    if (symbol.empty() || endTimeMs <= startTimeMs) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_footprintTradeMutex);
    auto it = m_recentFootprintTrades.find(symbol);
    if (it == m_recentFootprintTrades.end()) {
        return false;
    }
    const auto& trades = it->second;
    for (const auto& sample : trades) {
        if (sample.timestampMs < startTimeMs) {
            continue;
        }
        if (sample.timestampMs >= endTimeMs) {
            break;
        }
        out.push_back(sample);
    }
    return !out.empty();
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

    if (exchangeMs > 0 && trade.price > 0.0 && trade.size > 0.0) {
        std::lock_guard<std::mutex> lock(m_footprintTradeMutex);
        auto& trades = m_recentFootprintTrades[trade.product_id];
        const int64_t cutoff = exchangeMs - m_footprintTradeRetentionMs;
        while (!trades.empty() && trades.front().timestampMs < cutoff) {
            trades.pop_front();
        }
        trades.push_back(FootprintTradeSample{exchangeMs, trade.price, trade.size, trade.side});
    }
    
    emit tradeBroadcast(trade);
}

void ServerDataModel::onLiveOrderBookLevelUpdates(const QString& productId,
                                                  const std::vector<BookLevelUpdate>& updates,
                                                  qint64 exchangeMs) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);

    updateExchangeOffsetMs(static_cast<int64_t>(exchangeMs));

    if (m_recorder && !updates.empty()) {
        std::vector<recording::Level> levels;
        levels.reserve(updates.size());
        for (const auto& u : updates) levels.push_back({u.isBid, u.price, u.quantity});
        m_recorder->onUpdates(symbol, static_cast<int64_t>(exchangeMs), std::move(levels));
    }

    if (data.liveBook.getTickSize() <= 0.0) {
        // Can repeat per message until the snapshot arrives; rate limited.
        sLog_DataN(1000, "Order book update ignored, book not initialized: symbol=" << symbol
                   << " updates=" << updates.size());
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

void ServerDataModel::onLiveOrderBookInvalidated(const QString& productId, const QString& reason) {
    const std::string symbol = productId.toStdString();
    int count = 0;
    std::shared_lock lock(m_mutex);
    for (auto& [key, data] : m_symbols) {
        if (symbol.empty() || key == symbol) {
            if (data) {
                data->bookValid = false;
                ++count;
            }
        }
    }
    if (m_recorder) {
        m_recorder->onInvalid(symbol, localNowMs(), reason.toStdString());
    }
    sLog_Data("ServerDataModel: book invalid until next snapshot: symbol="
              << (symbol.empty() ? std::string("*") : symbol) << " symbols=" << count
              << " reason=" << reason);
}

void ServerDataModel::onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, qint64 envelopeMs) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);
    
    if (m_recorder) {
        // The whole book, before the live book's band clip.
        m_recorder->onSnapshot(symbol, envelopeMs > 0 ? static_cast<int64_t>(envelopeMs) : exchangeNowMs(),
                               toRecorderLevels(bids, asks));
    }

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
    data.bookValid = true;

    sLog_Data("ServerDataModel: Initialized book: symbol=" << symbol << " envelopeMs=" << envelopeMs
              << " bids=" << bids.size() << " asks=" << asks.size()
              << " range=[" << minPrice << ".." << maxPrice << "]"
              << " tick=" << tickSize << " bandPct=" << bandPct);
}
