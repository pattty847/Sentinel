#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include "RecordingDir.hpp"
#include "metrics/MetricsRegistry.hpp"
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
    for (const auto& symbol : normalizedDefaultSymbols(m_serverConfig.defaultSymbols))
        m_pinnedUp.emplace(symbol, false);
    m_mdConnected.store(pinnedAllUp(), std::memory_order_relaxed);
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
    const auto choice = recording::resolveRecordingDir(rc.dir, rc.fallbackDir); // shared with the lab
    if (choice.dir.empty()) {
        sLog_Warning("Recording v2 not started: volume for " << rc.dir
                     << " is not mounted and recording.fallback_dir is empty");
        return;
    }
    if (choice.fallback)
        sLog_Warning("Recording v2: volume for " << rc.dir << " not mounted, using fallback " << rc.fallbackDir);
    const std::filesystem::path dir = choice.dir;
    recording::RecorderConfig cfg;
    cfg.root = dir;
    cfg.priceScale = rc.priceScale;
    cfg.sizeScale = {rc.sizeFloor, rc.codesPerOctave};
    cfg.latenessMs = rc.latenessMs;
    cfg.livePublishMs = rc.livePublishMs;
    const auto units = [&](double dollars) { return static_cast<int64_t>(std::llround(dollars * rc.priceScale)); };
    cfg.layers = {
        {"near", units(rc.nearTick), 1.0 - rc.nearPct, 1.0 + rc.nearPct, false},
        {"deep", units(rc.deepTick), rc.deepLowFrac, rc.deepHighMult, true},
    };
    // Symbols as the app subscribes them (upper-cased), one series per layer.
    m_stallSeries.clear();
    for (const auto& symbol : normalizedDefaultSymbols(m_serverConfig.defaultSymbols))
        for (const auto& layer : cfg.layers)
            m_stallSeries.push_back({symbol, layer.name, 0});
    m_stallMonitor.emplace(rc.latenessMs);
    try {
        m_recordingLive = std::make_shared<recording::LiveService>(dir, recording::liveCadenceMs(rc.livePublishMs));
        cfg.publisher = [live = m_recordingLive, drops = &m_livePublishDrops](recording::RecordPtr record) {
            if (!live->publish(std::move(record))) {
                drops->fetch_add(1, std::memory_order_relaxed);
                sLog_Probe("recording.live.drop", "publication exceeds series limit or is stale");
            }
        };
        cfg.onSelfInvalidated = [this](const std::string& symbol, const std::string& reason) {
            // Recorder worker thread: hand off only.
            QMetaObject::invokeMethod(this, [this, s = QString::fromStdString(symbol),
                                             r = QString::fromStdString(reason)] {
                emit recordingResnapshotRequested(s, r);
            }, Qt::QueuedConnection);
        };
        m_recorder = std::make_unique<recording::BookRecorder>(std::move(cfg));
        m_recordingDir = dir;
    } catch (const std::exception& e) {
        m_recordingLive.reset();
        m_stallMonitor.reset();
        sLog_Error("Recording v2 failed to start: dir=" << dir.string() << " error=" << e.what());
        return;
    }
    sLog_App("Recording v2 started: dir=" << dir.string()
             << " near=" << rc.nearTick << "@+/-" << rc.nearPct * 100 << "%"
             << " deep=" << rc.deepTick << "@[" << rc.deepLowFrac << "x.." << rc.deepHighMult << "x]"
             << " livePublishMs=" << rc.livePublishMs << " liveCadenceMs=" << recording::liveCadenceMs(rc.livePublishMs));

    m_recorderTimer.setInterval(250);
    connect(&m_recorderTimer, &QTimer::timeout, this, [this]() {
        if (!m_recorder) return;
        m_recorder->onTick(localNowMs());
        if (++m_recorderTicks % 4 == 0)  // once a second
            checkRecorderProgress(localNowMs());
        if (m_recorderTicks % 240 == 0) {  // once a minute
            const auto s = m_recorder->stats();
            sLog_Data("Recording v2 stats: columns=" << s.columnsWritten << " late=" << s.lateEvents
                      << " backward=" << s.backwardSteps << " queueDrops=" << s.queueDrops
                      << " invalidations=" << s.invalidations << " diskErrors=" << s.diskErrors);
        }
    });
    m_recorderTimer.start();
}

// Warns (throttled per series by the monitor) when a recorded layer stops
// committing columns while the market-data connection is up.
void ServerDataModel::checkRecorderProgress(int64_t nowMs) {
    if (!m_stallMonitor) return;
    for (auto& series : m_stallSeries)
        series.lastColumnMs = m_recorder->watermarks(series.symbol, series.layer).lastColumnMs;
    for (const auto& stall : m_stallMonitor->check(nowMs, m_stallSeries))
        sLog_Warning("Recording v2 stalled: symbol=" << stall.symbol << " layer=" << stall.layer
                     << " lastColumnMs=" << stall.lastColumnMs << " overdueMs=" << stall.overdueMs
                     << " invalidations=" << m_recorder->stats().invalidations);
}

// One call per product connection transition (pinned recorder symbols and GUI
// symbols alike). The stall monitor tracks every symbol by its own state. The
// health series (sentinel_mdc_connected and the up/down counters) follow only the
// pinned symbols: a GUI chart closing its product must not page, and a pinned
// product down must not hide behind a GUI product that is still up.
void ServerDataModel::onMarketDataConnectionChanged(const std::string& symbol, bool connected) {
    if (m_stallMonitor) m_stallMonitor->setConnected(symbol, connected, localNowMs());
    const auto pinned = m_pinnedUp.find(symbol);
    if (pinned == m_pinnedUp.end() || pinned->second == connected) return;
    pinned->second = connected;
    (connected ? m_mdTransportUps : m_mdTransportDowns).fetch_add(1, std::memory_order_relaxed);
    m_mdConnected.store(pinnedAllUp(), std::memory_order_relaxed);
}

// AND over the pinned symbols; vacuously true when nothing is pinned (nothing to record).
bool ServerDataModel::pinnedAllUp() const {
    return std::all_of(m_pinnedUp.begin(), m_pinnedUp.end(), [](const auto& entry) { return entry.second; });
}

void ServerDataModel::registerMetrics(sentinel::metrics::MetricsRegistry& r) {
    using Value = std::optional<double>;
    const auto load = [](const std::atomic<uint64_t>& a) { return [&a]() -> Value { return double(a.load(std::memory_order_relaxed)); }; };
    r.gaugeFn("sentinel_mdc_connected", "1 while every pinned (recorder) product's upstream connection is up.", {},
              [this]() -> Value { return m_mdConnected.load(std::memory_order_relaxed) ? 1.0 : 0.0; });
    r.counterFn("sentinel_mdc_transport_up_total", "Pinned products' upstream connection up transitions (reconnects = this - pinned products).", {},
                load(m_mdTransportUps));
    r.counterFn("sentinel_mdc_transport_down_total", "Pinned products' upstream connection down transitions.", {},
                load(m_mdTransportDowns));
    r.gaugeFn("sentinel_exchange_clock_offset_ms", "Smoothed local minus exchange clock in ms (0 = not yet measured).", {},
              [this]() -> Value { return double(m_exchangeOffsetMs.load(std::memory_order_relaxed)); });
    r.gaugeFn("sentinel_recorder_running", "1 when recording v2 started in this process.", {},
              [this]() -> Value { return m_recorder ? 1.0 : 0.0; });
    if (!m_recorder) return;

    // BookRecorder::stats() is relaxed atomics; one call per series per scrape.
    using Stats = recording::BookRecorder::Stats;
    const auto stat = [this](uint64_t Stats::*field) { return [this, field]() -> Value { return double(m_recorder->stats().*field); }; };
    r.counterFn("sentinel_recorder_columns_written_total", "Minute columns the recorder committed.", {}, stat(&Stats::columnsWritten));
    r.counterFn("sentinel_recorder_late_events_total", "Book messages timestamped before the already-closed minutes.", {}, stat(&Stats::lateEvents));
    r.counterFn("sentinel_recorder_backward_steps_total", "Book messages whose timestamp stepped backwards.", {}, stat(&Stats::backwardSteps));
    r.counterFn("sentinel_recorder_queue_drops_total", "Book messages dropped (or turned into an invalidation) on recorder queue overflow.", {}, stat(&Stats::queueDrops));
    r.counterFn("sentinel_recorder_invalidations_total", "Recorder book invalidations (upstream and self).", {}, stat(&Stats::invalidations));
    r.counterFn("sentinel_recorder_disk_errors_total", "Recorder disk write failures.", {}, stat(&Stats::diskErrors));
    r.counterFn("sentinel_recorder_live_publish_drops_total", "Live publications refused (series limit or stale).", {},
                load(m_livePublishDrops));

    // Pinned symbol x layer, the series the stall monitor watches (main thread).
    for (const auto& series : m_stallSeries) {
        const sentinel::metrics::Labels labels{{"product", series.symbol}, {"layer", series.layer}};
        r.gaugeFn("sentinel_recorder_last_column_timestamp_seconds",
                  "Start of the newest committed minute column (absent until the first column).", labels,
                  [this, symbol = series.symbol, layer = series.layer]() -> Value {
                      const int64_t ms = m_recorder->watermarks(symbol, layer).lastColumnMs;
                      return ms > 0 ? Value(ms / 1000.0) : std::nullopt;
                  });
        r.gaugeFn("sentinel_recorder_column_overdue_seconds",
                  "Seconds the next column is past due (stall monitor; absent while this product is disconnected). "
                  "The log warns 'Recording v2 stalled' at 60.", labels,
                  [this, symbol = series.symbol, layer = series.layer]() -> Value {
                      const auto overdue = m_stallMonitor->overdueMs(
                          symbol, localNowMs(), m_recorder->watermarks(symbol, layer).lastColumnMs);
                      return overdue ? Value(*overdue / 1000.0) : std::nullopt;
                  });
    }
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

// Worker-only bounded snapshot. False means over budget, never a partial tape.
bool ServerDataModel::collectOverlayTrades(const std::string& symbol, int64_t startMs,
                                          int64_t endMs, size_t limit,
                                          std::vector<FootprintTradeSample>& out,
                                          int64_t* retainedFromMs) const {
    out.clear();
    if (retainedFromMs) *retainedFromMs = 0;
    if (endMs <= startMs) return true;
    {
        std::lock_guard<std::mutex> lock(m_footprintTradeMutex);
        const auto it = m_recentFootprintTrades.find(symbol);
        if (it == m_recentFootprintTrades.end()) return true;
        // Arrival order need not match exchange time. Inspect every timestamp,
        // but only copy and budget the requested half-open window.
        for (const auto& trade : it->second) {
            if (retainedFromMs && (*retainedFromMs == 0 || trade.timestampMs < *retainedFromMs))
                *retainedFromMs = trade.timestampMs;
            if (trade.timestampMs < startMs || trade.timestampMs >= endMs) continue;
            if (out.size() == limit) {
                out.clear(); // Never expose an over-budget partial snapshot.
                return false;
            }
            out.push_back(trade);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.timestampMs < b.timestampMs; });
    return true;
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
