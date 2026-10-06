#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include "RecordingDir.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "roller/Grid.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
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
    for (const auto& symbol : normalizedDefaultSymbols(m_serverConfig.defaultSymbols)) {
        auto& feed = m_feeds[symbol];
        feed.pinned = true;
        feed.lifetime = ++m_nextFeedLifetime;
    }
    m_metadataTimer.setInterval(5000);
    connect(&m_metadataTimer, &QTimer::timeout, this, [this] {
        for (auto& [symbol, feed] : m_feeds) {
            if (symbol == "BTC-USD") continue; // Keep the established configured BTC book tick.
            requestProductMetadataIfNeeded(symbol, feed);
        }
    });
    m_metadataTimer.start();
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
    if (rc.source == "roller") {
        startRollerServing();
        return;
    }
    if (rc.source != "primary") {
        sLog_Error("Recording v2 not started: unknown recording.source=" << rc.source << " (primary | roller)");
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
        cfg.onReleased = [live = m_recordingLive](const std::string& symbol) { live->releaseSymbol(symbol); };
        cfg.onSelfInvalidated = [this](const std::string& symbol, const std::string& reason) {
            // Recorder worker thread: hand off only.
            QMetaObject::invokeMethod(this, [this, s = QString::fromStdString(symbol),
                                             r = QString::fromStdString(reason)] {
                if (m_feeds.contains(s.toStdString())) emit recordingResnapshotRequested(s, r);
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

// recording.source: roller. No primary BookRecorder: the journal roller writes
// roller_shadow.dir and publishes into this LiveService (SentinelServerApp
// installs rollerPublisher() and attachRoller() before the stream server starts).
void ServerDataModel::startRollerServing() {
    const auto& rc = m_serverConfig.recording;
    const auto& shadow = m_serverConfig.rollerShadow;
    if (!shadow.enabled) {
        sLog_Error("Recording not served: recording.source=roller requires roller_shadow.enabled=true");
        return;
    }
    if (shadow.outputRoot.empty() || !recording::volumeMounted(shadow.outputRoot)) {
        sLog_Warning("Recording not served: volume for roller_shadow.dir " << shadow.outputRoot << " is not mounted");
        return;
    }
    const std::filesystem::path dir = shadow.outputRoot;
    m_stallSeries.clear();
    for (const auto& symbol : rollerProducts(m_serverConfig))
        for (const auto* layer : {"near", "deep"})
            m_stallSeries.push_back({symbol, layer, 0});
    m_stallMonitor.emplace(recording::RecorderConfig{}.latenessMs);
    try {
        m_recordingLive = std::make_shared<recording::LiveService>(dir, recording::liveCadenceMs(rc.livePublishMs));
    } catch (const std::exception& e) {
        m_stallMonitor.reset();
        sLog_Error("Recording not served: dir=" << dir.string() << " error=" << e.what());
        return;
    }
    m_recordingDir = dir;
    m_servesRoller = true;
    sLog_App("Recording served by the journal roller: dir=" << dir.string() << " products="
             << rollerProducts(m_serverConfig).size() << " livePublishMs=" << rc.livePublishMs
             << " liveCadenceMs=" << recording::liveCadenceMs(rc.livePublishMs));
    m_recorderTimer.setInterval(1000);
    connect(&m_recorderTimer, &QTimer::timeout, this, [this]() { checkRecorderProgress(localNowMs()); });
    m_recorderTimer.start();
}

std::function<void(recording::RecordPtr)> ServerDataModel::rollerPublisher() {
    if (!m_servesRoller || !m_recordingLive) return {};
    // Roller history and lead workers: bounded hand-off only.
    return [live = m_recordingLive, drops = &m_livePublishDrops](recording::RecordPtr record) {
        if (!live->publish(std::move(record))) {
            drops->fetch_add(1, std::memory_order_relaxed);
            sLog_Probe("recording.live.drop", "publication exceeds series limit or is stale");
        }
    };
}

void ServerDataModel::attachRoller(RollerWatermarks watermarks, std::function<bool(const std::string&)> running) {
    if (!m_servesRoller || !watermarks || !running) return;
    m_rollerWatermarks = std::move(watermarks);
    m_rollerRunning = std::move(running);
    m_rollerAttached.store(true);
}

// Warns (throttled per series by the monitor) when a recorded layer stops
// committing columns while the market-data connection (roller: the product's
// worker) is up.
void ServerDataModel::checkRecorderProgress(int64_t nowMs) {
    if (!m_stallMonitor || !recordingAvailable()) return;
    if (m_servesRoller)
        for (const auto& series : m_stallSeries)
            m_stallMonitor->setConnected(series.symbol, m_rollerRunning(series.symbol), nowMs);
    for (auto& series : m_stallSeries)
        series.lastColumnMs = recordingWatermarks(series.symbol, series.layer).lastColumnMs;
    for (const auto& stall : m_stallMonitor->check(nowMs, m_stallSeries))
        sLog_Warning("Recording v2 stalled: symbol=" << stall.symbol << " layer=" << stall.layer
                     << " lastColumnMs=" << stall.lastColumnMs << " overdueMs=" << stall.overdueMs
                     << " invalidations=" << (m_recorder ? m_recorder->stats().invalidations : 0));
}

// Active feed membership is managed before queued transport statuses are delivered.
void ServerDataModel::acquireGuiFeed(const std::string& symbol) {
    auto [it, inserted] = m_feeds.try_emplace(symbol);
    if (inserted) {
        it->second.lifetime = ++m_nextFeedLifetime;
        requestProductMetadataIfNeeded(symbol, it->second);
    }
}
void ServerDataModel::requestProductMetadataIfNeeded(const std::string& symbol, FeedState& feed) {
    if (symbol == "BTC-USD" || !feed.metadata.is_null() || feed.metadataPending ||
        localNowMs() < feed.nextMetadataAttemptMs) return;
    feed.metadataPending = true;
    feed.nextMetadataAttemptMs = localNowMs() + 30'000;
    emit productMetadataRequested(QString::fromStdString(symbol), feed.lifetime);
}
void ServerDataModel::onProductMetadata(const std::string& symbol, uint64_t lifetime,
                                        const nlohmann::json& metadata, const std::string& error) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end() || it->second.lifetime != lifetime || symbol == "BTC-USD") return;
    auto& feed = it->second;
    feed.metadataPending = false;
    try {
        if (!error.empty()) throw std::runtime_error(error);
        if (metadata.at("product_id").get<std::string>() != symbol)
            throw std::runtime_error("product_id mismatch");
        // Validate exact decimal strings now; derive from the retained current BBO below.
        sentinel::roller::deriveGrid(metadata, 1.0);
        feed.metadata = metadata;
        sLog_Data("Live book metadata ready: symbol=" << symbol
                  << " quoteIncrement=" << metadata.at("quote_increment").get<std::string>());
        auto& data = ensureSymbol(symbol);
        if (data.rawValid)
            publishAggregatedBook(QString::fromStdString(symbol), data, feed, 0);
    } catch (const std::exception& e) {
        feed.metadata = nullptr;
        feed.nextMetadataAttemptMs = localNowMs() + 30'000;
        auto& data = ensureSymbol(symbol);
        data.clearAggregatedBook();
        emit bookSnapshotBroadcast(QString::fromStdString(symbol), {}, {}, 0.0,
                                   QStringLiteral("metadata_unavailable"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book metadata unavailable: symbol=" << symbol << " error=" << e.what());
    }
}
void ServerDataModel::releaseGuiFeed(const std::string& symbol, int64_t releaseLocalMs) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end() || it->second.pinned) return;
    m_feeds.erase(it);
    {
        std::shared_lock lock(m_mutex);
        if (const auto data = m_symbols.find(symbol); data != m_symbols.end()) {
            data->second->invalidateLiveBook();
            data->second->lastTradePrice = 0;
        }
    }
    if (m_heatmapStreamer) m_heatmapStreamer->releaseSymbol(symbol);
    if (m_recorder) m_recorder->releaseSymbol(symbol, releaseLocalMs ? releaseLocalMs : localNowMs());
    else if (m_recordingLive && !m_servesRoller) m_recordingLive->releaseSymbol(symbol); // the roller owns its series
    sLog_Data("ServerDataModel: feed released, recording stopped: symbol=" << symbol);
}
void ServerDataModel::onMarketDataConnectionChanged(const std::string& symbol, bool connected) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end()) return; // retired GUI socket: never recreate a series
    if (connected) requestProductMetadataIfNeeded(symbol, it->second);
    // Only recorded products need deadlines; do not retain closed GUI products here.
    if (m_stallMonitor && it->second.pinned && !m_servesRoller) m_stallMonitor->setConnected(symbol, connected, localNowMs());
    auto& feed = it->second;
    if (feed.connected == connected) return;
    feed.connected = connected;
    ++(connected ? feed.ups : feed.downs);
}

void ServerDataModel::registerMetrics(sentinel::metrics::MetricsRegistry& r) {
    using Value = std::optional<double>;
    const auto load = [](const std::atomic<uint64_t>& a) { return [&a]() -> Value { return double(a.load(std::memory_order_relaxed)); }; };
    using Registry = sentinel::metrics::MetricsRegistry;
    const auto sampleFeeds = [this](auto value) {
        return [this, value] {
            std::vector<Registry::Sample> samples;
            for (const auto& [symbol, feed] : m_feeds)
                samples.push_back({{{"product", symbol}, {"pinned", feed.pinned ? "1" : "0"}}, double(value(feed))});
            return samples;
        };
    };
    r.familyFn("sentinel_mdc_connected", "1 while this product's upstream connection is up.", Registry::Type::Gauge,
               sampleFeeds([](const FeedState& f) { return f.connected; }));
    r.familyFn("sentinel_mdc_transport_up_total", "Product upstream up transitions in this feed lifetime.", Registry::Type::Counter,
               sampleFeeds([](const FeedState& f) { return f.ups; }));
    r.familyFn("sentinel_mdc_transport_down_total", "Product upstream down transitions in this feed lifetime.", Registry::Type::Counter,
               sampleFeeds([](const FeedState& f) { return f.downs; }));
    r.gaugeFn("sentinel_exchange_clock_offset_ms", "Smoothed local minus exchange clock in ms (0 = not yet measured).", {},
              [this]() -> Value { return double(m_exchangeOffsetMs.load(std::memory_order_relaxed)); });
    r.gaugeFn("sentinel_recorder_running", "1 when recording is served (primary recorder started, or the roller attached).", {},
              [this]() -> Value { return recordingAvailable() ? 1.0 : 0.0; });
    if (!m_recorder && !m_servesRoller) return;

    if (m_recorder) {
        // BookRecorder::stats() is relaxed atomics; one call per series per scrape.
        using Stats = recording::BookRecorder::Stats;
        const auto stat = [this](uint64_t Stats::*field) { return [this, field]() -> Value { return double(m_recorder->stats().*field); }; };
        r.counterFn("sentinel_recorder_columns_written_total", "Minute columns the recorder committed.", {}, stat(&Stats::columnsWritten));
        r.counterFn("sentinel_recorder_late_events_total", "Book messages timestamped before the already-closed minutes.", {}, stat(&Stats::lateEvents));
        r.counterFn("sentinel_recorder_backward_steps_total", "Book messages whose timestamp stepped backwards.", {}, stat(&Stats::backwardSteps));
        r.counterFn("sentinel_recorder_queue_drops_total", "Book messages dropped (or turned into an invalidation) on recorder queue overflow.", {}, stat(&Stats::queueDrops));
        r.counterFn("sentinel_recorder_invalidations_total", "Recorder book invalidations (upstream and self).", {}, stat(&Stats::invalidations));
        r.counterFn("sentinel_recorder_disk_errors_total", "Recorder disk write failures.", {}, stat(&Stats::diskErrors));
    }
    r.counterFn("sentinel_recorder_live_publish_drops_total", "Live publications refused (series limit or stale).", {},
                load(m_livePublishDrops));

    // Pinned symbol x layer, the series the stall monitor watches (main thread).
    for (const auto& series : m_stallSeries) {
        const sentinel::metrics::Labels labels{{"product", series.symbol}, {"layer", series.layer}};
        r.gaugeFn("sentinel_recorder_last_column_timestamp_seconds",
                  "Start of the newest committed minute column (absent until the first column).", labels,
                  [this, symbol = series.symbol, layer = series.layer]() -> Value {
                      const int64_t ms = recordingWatermarks(symbol, layer).lastColumnMs;
                      return ms > 0 ? Value(ms / 1000.0) : std::nullopt;
                  });
        r.gaugeFn("sentinel_recorder_column_overdue_seconds",
                  "Seconds the next column is past due (stall monitor; absent while this product is disconnected). "
                  "The log warns 'Recording v2 stalled' at 60.", labels,
                  [this, symbol = series.symbol, layer = series.layer]() -> Value {
                      if (!m_stallMonitor) return std::nullopt;
                      const auto overdue = m_stallMonitor->overdueMs(
                          symbol, localNowMs(), recordingWatermarks(symbol, layer).lastColumnMs);
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
    if (!m_feeds.contains(trade.product_id)) return;
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
    if (!m_feeds.contains(symbol)) return;
    SymbolHotData& data = ensureSymbol(symbol);

    updateExchangeOffsetMs(static_cast<int64_t>(exchangeMs));

    if (m_recorder && !updates.empty()) {
        std::vector<recording::Level> levels;
        levels.reserve(updates.size());
        for (const auto& u : updates) levels.push_back({u.isBid, u.price, u.quantity});
        m_recorder->onUpdates(symbol, static_cast<int64_t>(exchangeMs), std::move(levels));
    }

    if (updates.empty()) return;
    if (!data.rawValid) {
        sLog_DataN(1000, "Order book update ignored until upstream snapshot: symbol=" << symbol
                   << " updates=" << updates.size());
        return;
    }

    const auto timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(exchangeMs));
    const bool ready = data.bookValid && data.liveBook.getTickSize() > 0.0;
    const double minPrice = data.rawMinPrice;
    const double maxPrice = data.rawMaxPrice;
    const double tickSize = ready ? data.liveBook.getTickSize() : 0.0;
    thread_local std::vector<BookLevelUpdate> bucketUpdates;
    bucketUpdates.clear();
    if (ready) bucketUpdates.reserve(updates.size());
    for (const auto& update : updates) {
        if (!std::isfinite(update.price) || update.price <= 0.0 ||
            !std::isfinite(update.quantity) || update.quantity < 0.0) continue;
        if (update.price < minPrice || update.price > maxPrice) {
            // An out-of-band new best cannot be reconstructed from the bounded
            // raw map. Keep recording, but await a real upstream snapshot.
            if (update.quantity > 0.0 &&
                ((update.isBid && update.price > maxPrice) || (!update.isBid && update.price < minPrice))) {
                data.invalidateLiveBook();
                emit bookSnapshotBroadcast(productId, {}, {}, 0.0,
                                           QStringLiteral("aggregation_unavailable"),
                                           data.liveBook.stateVersion());
                sLog_Warning("Live book raw band exceeded by potential best: symbol=" << symbol
                             << " price=" << update.price);
                return;
            }
            continue;
        }
        auto& raw = update.isBid ? data.rawBids : data.rawAsks;
        const auto it = raw.find(update.price);
        const double previous = it == raw.end() ? 0.0 : it->second;
        if (previous == update.quantity) continue;
        if (update.quantity == 0.0) {
            if (it != raw.end()) raw.erase(it);
        } else if (it == raw.end()) {
            raw.emplace(update.price, update.quantity);
        } else {
            it->second = update.quantity;
        }
        if (!ready) continue; // Metadata gate affects only live aggregation.
        const auto index = LiveOrderBook::bucketIndex(update.price,
                                                       data.liveBook.getMinPrice(), tickSize);
        auto& totals = update.isBid ? data.bucketBids : data.bucketAsks;
        auto& counts = update.isBid ? data.bucketBidCounts : data.bucketAskCounts;
        if (index >= totals.size()) continue;
        if (update.quantity == 0.0) {
            if (previous > 0.0) --counts[index];
        } else {
            if (previous == 0.0) ++counts[index];
        }
        const double before = totals[index];
        double after = before + update.quantity - previous;
        if (counts[index] == 0) {
            after = 0.0;
        } else if (after <= 64.0 * std::numeric_limits<double>::epsilon() *
                            std::max({std::abs(before), std::abs(previous), std::abs(update.quantity)})) {
            // Cancellation can hide or distort a tiny remaining peer. Rebuild only
            // this rare exceptional bucket, never scan the side in ordinary updates.
            after = 0.0;
            for (const auto& [price, quantity] : raw)
                if (price >= minPrice && price <= maxPrice &&
                    LiveOrderBook::bucketIndex(price, data.liveBook.getMinPrice(), tickSize) == index)
                    after += quantity;
            if (after <= 0.0)
                sLog_Warning("Live book aggregate underflow: symbol=" << symbol << " price=" << update.price);
        }
        totals[index] = after;
        bucketUpdates.push_back({update.isBid, data.liveBook.index_to_price(index), after});
    }
    if (!ready) {
        if (data.awaitingRawBbo && !data.rawBids.empty() && !data.rawAsks.empty()) {
            const auto feed = m_feeds.find(symbol);
            if (feed != m_feeds.end() && (symbol == "BTC-USD" || !feed->second.metadata.is_null())) {
                double bestBid = 0.0, bestAsk = 0.0;
                for (const auto& [price, quantity] : data.rawBids)
                    if (quantity > 0.0) bestBid = std::max(bestBid, price);
                for (const auto& [price, quantity] : data.rawAsks)
                    if (quantity > 0.0 && (bestAsk == 0.0 || price < bestAsk)) bestAsk = price;
                if (bestBid > 0.0 && bestAsk > bestBid)
                    publishAggregatedBook(productId, data, feed->second, exchangeMs);
            }
        }
        return;
    }
    if (bucketUpdates.empty()) return;
    std::vector<BookDelta> deltas;
    data.liveBook.applyUpdates(bucketUpdates, timestamp, &deltas);
    if (!deltas.empty())
        emit bookUpdateBroadcast(productId, deltas, data.liveBook.getMinPrice(),
                                 data.liveBook.getTickSize(), data.liveBook.stateVersion());
}

void ServerDataModel::onLiveOrderBookInvalidated(const QString& productId, const QString& reason) {
    const std::string symbol = productId.toStdString();
    int count = 0;
    std::vector<std::pair<QString, uint64_t>> notifications;
    {
        std::shared_lock lock(m_mutex);
        for (auto& [key, data] : m_symbols) {
            if ((symbol.empty() || key == symbol) && data) {
                data->invalidateLiveBook();
                notifications.emplace_back(QString::fromStdString(key), data->liveBook.stateVersion());
                ++count;
            }
        }
    }
    for (const auto& [product, version] : notifications)
        emit bookSnapshotBroadcast(product, {}, {}, 0.0, QStringLiteral("invalidated"), version);
    if (m_recorder) {
        m_recorder->onInvalid(symbol, localNowMs(), reason.toStdString());
    }
    sLog_Data("ServerDataModel: book invalid until next snapshot: symbol="
              << (symbol.empty() ? std::string("*") : symbol) << " symbols=" << count
              << " reason=" << reason);
}

void ServerDataModel::onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, qint64 envelopeMs) {
    std::string symbol = productId.toStdString();
    if (!m_feeds.contains(symbol)) return;
    SymbolHotData& data = ensureSymbol(symbol);
    
    if (m_recorder) {
        // The whole book, before the live book's band clip.
        m_recorder->onSnapshot(symbol, envelopeMs > 0 ? static_cast<int64_t>(envelopeMs) : exchangeNowMs(),
                               toRecorderLevels(bids, asks));
    }

    const auto validSide = [](const std::vector<OrderBookLevel>& side) {
        return !side.empty() && std::all_of(side.begin(), side.end(), [](const OrderBookLevel& level) {
            return std::isfinite(level.price) && level.price > 0.0 &&
                   std::isfinite(level.size) && level.size > 0.0;
        });
    };
    const bool validLevels = validSide(bids) && validSide(asks);
    const double bestBid = validLevels ? std::max_element(bids.begin(), bids.end(),
        [](const auto& a, const auto& b) { return a.price < b.price; })->price : 0.0;
    const double bestAsk = validLevels ? std::min_element(asks.begin(), asks.end(),
        [](const auto& a, const auto& b) { return a.price < b.price; })->price : 0.0;
    if (!validLevels || bestBid >= bestAsk) {
        data.invalidateLiveBook();
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("invalid_snapshot"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book snapshot rejected: symbol=" << symbol << " bids=" << bids.size()
                     << " asks=" << asks.size());
        return;
    }

    const auto [minPrice, maxPrice] = computeBandRange(bids, asks, m_serverConfig.orderbook.bandPct);
    if (!std::isfinite(minPrice) || !std::isfinite(maxPrice) || maxPrice <= minPrice) {
        data.invalidateLiveBook();
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("invalid_snapshot"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book snapshot band invalid: symbol=" << symbol);
        return;
    }
    // Track native levels independently of the product tick. This remains current
    // while REST metadata is pending; it never asks the upstream to reconnect.
    data.invalidateLiveBook();
    data.rawMinPrice = std::max(0.0, minPrice);
    data.rawMaxPrice = maxPrice;
    for (const auto& level : bids)
        if (level.price >= data.rawMinPrice && level.price <= data.rawMaxPrice)
            data.rawBids[level.price] = level.size;
    for (const auto& level : asks)
        if (level.price >= data.rawMinPrice && level.price <= data.rawMaxPrice)
            data.rawAsks[level.price] = level.size;
    data.rawValid = true;

    const auto& feed = m_feeds.at(symbol);
    if (symbol != "BTC-USD" && feed.metadata.is_null()) {
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("metadata_unavailable"),
                                   data.liveBook.stateVersion());
        sLog_Data("Live book raw snapshot retained while metadata pending: symbol=" << symbol
                  << " bids=" << data.rawBids.size() << " asks=" << data.rawAsks.size());
        return;
    }
    publishAggregatedBook(productId, data, feed, envelopeMs);
}

void ServerDataModel::publishAggregatedBook(const QString& productId, SymbolHotData& data,
                                            const FeedState& feed, qint64 envelopeMs) {
    const std::string symbol = productId.toStdString();
    if (!data.rawValid) return; // A real upstream invalidation requires a new snapshot.
    double bestBid = 0.0, bestAsk = 0.0;
    for (const auto& [price, quantity] : data.rawBids)
        if (quantity > 0.0) bestBid = std::max(bestBid, price);
    for (const auto& [price, quantity] : data.rawAsks)
        if (quantity > 0.0 && (bestAsk == 0.0 || price < bestAsk)) bestAsk = price;
    if (bestBid <= 0.0 || bestAsk <= bestBid) {
        data.clearAggregatedBook();
        data.awaitingRawBbo = true;
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("aggregation_unavailable"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book current raw BBO unavailable: symbol=" << symbol
                     << " bid=" << bestBid << " ask=" << bestAsk);
        return;
    }
    data.awaitingRawBbo = false;
    double tickSize = m_serverConfig.orderbook.tickSize;
    if (symbol != "BTC-USD") {
        try {
            tickSize = sentinel::roller::deriveGrid(feed.metadata, (bestBid + bestAsk) * 0.5).nearTick;
        } catch (const std::exception& e) {
            data.clearAggregatedBook();
            emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("invalid_tick"),
                                       data.liveBook.stateVersion());
            sLog_Warning("Live book tick unavailable: symbol=" << symbol << " error=" << e.what());
            return;
        }
    }
    if (!std::isfinite(tickSize) || tickSize <= 0.0) {
        data.clearAggregatedBook();
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("invalid_tick"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book configured tick invalid: symbol=" << symbol << " tick=" << tickSize);
        return;
    }
    data.bookValid = false;
    data.liveBook.initialize(data.rawMinPrice, data.rawMaxPrice, tickSize);
    const auto bucketCount = data.liveBook.getBids().size();
    data.bucketBids.assign(bucketCount, 0.0);
    data.bucketAsks.assign(bucketCount, 0.0);
    data.bucketBidCounts.assign(bucketCount, 0);
    data.bucketAskCounts.assign(bucketCount, 0);
    const auto ingestSide = [&](const auto& raw, bool isBid) {
        auto& totals = isBid ? data.bucketBids : data.bucketAsks;
        auto& counts = isBid ? data.bucketBidCounts : data.bucketAskCounts;
        for (const auto& [price, quantity] : raw) {
            if (price < data.liveBook.getMinPrice() || price > data.liveBook.getMaxPrice()) continue;
            const auto index = LiveOrderBook::bucketIndex(price, data.liveBook.getMinPrice(), tickSize);
            if (index >= totals.size()) continue;
            totals[index] += quantity;
            ++counts[index];
        }
    };
    ingestSide(data.rawBids, true);
    ingestSide(data.rawAsks, false);
    std::vector<BookLevelUpdate> updates;
    updates.reserve(std::min(bucketCount * 2, data.rawBids.size() + data.rawAsks.size()));
    for (size_t i = 0; i < bucketCount; ++i) {
        if (data.bucketBids[i] > 0.0)
            updates.push_back({true, data.liveBook.index_to_price(i), data.bucketBids[i]});
        if (data.bucketAsks[i] > 0.0)
            updates.push_back({false, data.liveBook.index_to_price(i), data.bucketAsks[i]});
    }
    data.liveBook.applyUpdates(updates, std::chrono::system_clock::now(), nullptr);
    if (data.liveBook.getBidCount() == 0 || data.liveBook.getAskCount() == 0) {
        data.clearAggregatedBook();
        emit bookSnapshotBroadcast(productId, {}, {}, 0.0, QStringLiteral("aggregation_unavailable"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book aggregation has no two-sided levels: symbol=" << symbol
                     << " tick=" << tickSize);
        return;
    }
    data.bookValid = true;
    std::vector<OrderBookLevel> publishedBids, publishedAsks;
    const auto& bookBids = data.liveBook.getBids();
    const auto& bookAsks = data.liveBook.getAsks();
    publishedBids.reserve(data.liveBook.getBidCount());
    publishedAsks.reserve(data.liveBook.getAskCount());
    for (size_t i = 0; i < bookBids.size(); ++i)
        if (bookBids[i] > 0.0) publishedBids.push_back({data.liveBook.index_to_price(i), bookBids[i]});
    for (size_t i = 0; i < bookAsks.size(); ++i)
        if (bookAsks[i] > 0.0) publishedAsks.push_back({data.liveBook.index_to_price(i), bookAsks[i]});
    emit bookSnapshotBroadcast(productId, publishedBids, publishedAsks, tickSize,
                               QStringLiteral("ready"), data.liveBook.stateVersion());
    sLog_Data("ServerDataModel: Initialized current book: symbol=" << symbol << " envelopeMs=" << envelopeMs
              << " rawBids=" << data.rawBids.size() << " rawAsks=" << data.rawAsks.size()
              << " range=[" << data.rawMinPrice << ".." << data.rawMaxPrice << "]"
              << " tick=" << tickSize);

}
