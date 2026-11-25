// Live flow holds trades across the provisional-tap/durable-grid ordering gap.
// Grid holds are bounded on the model thread; the ordinary trade path does no
// grid I/O, clock read, or hold allocation. Admission lateness is saved at arrival.
#include "TradeOverlayPublisher.hpp"
#include "ServerDataModel.hpp"
#include "FlowStoreLag.hpp"
#include "SentinelLogging.hpp"
#include "RecordingDir.hpp"
#include "flow/FlowCodec.hpp"
#include <QThread>
#include "flow/FlowCompose.hpp"
#include "protocol/SentinelStreamProtocol.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "roller/Grid.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <cstdlib>

namespace {
bool flowLossIn(const std::set<int64_t>& losses, int64_t start, int64_t end) {
    const auto it = losses.lower_bound(start);
    return it != losses.end() && *it < end;
}

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
    , m_aggregator(std::make_unique<TimeframeAggregator>(m_serverConfig.chart.timeframesMs,
                                                         m_serverConfig.chart.historyBars))
{
    for (const auto& symbol : rollerProducts(m_serverConfig)) {
        m_feeds.try_emplace(symbol);
        m_flowReceiveStates.try_emplace(symbol);
        m_flowLateTrades.try_emplace(symbol, 0);
        m_flowSealMismatches.try_emplace(symbol, 0);
    }
    m_reseedTimer.setSingleShot(true);
    connect(&m_reseedTimer, &QTimer::timeout, this, [this] { deliverReseeds(); });
    int64_t maxTfMs = std::max<int64_t>(1000, m_serverConfig.chart.defaultTimeframeMs);
    for (const int64_t tf : m_serverConfig.chart.timeframesMs) {
        if (tf > maxTfMs) {
            maxTfMs = tf;
        }
    }
    const int64_t gridSpanMs = maxTfMs * std::max<int64_t>(1024, m_serverConfig.tradeOverlays.retentionColumns);
    m_tradeRetentionMs = std::clamp<int64_t>(gridSpanMs * 2, 300'000, 86'400'000);

    // Candles close on each product's journal
    // time (onFeedWatermark and its trades), never this process's wall clock.
    connect(m_aggregator.get(), &TimeframeAggregator::barClosed, this, &ServerDataModel::barClosed);
    connect(m_aggregator.get(), &TimeframeAggregator::barUpdated, this, &ServerDataModel::barUpdated);
    connect(m_aggregator.get(), &TimeframeAggregator::liveRollupsUpdated, this,
            &ServerDataModel::liveRollupsUpdated);

    m_flowTimer.setTimerType(Qt::PreciseTimer);
    m_flowTimer.setInterval(std::clamp(config.recording.livePublishMs,
        ServerRecordingConfig::kLivePublishMinMs, ServerRecordingConfig::kLivePublishMaxMs));
    connect(&m_flowTimer, &QTimer::timeout, this, &ServerDataModel::publishFlow);
    m_flowTimer.start();
    startRollerServing();
}

ServerDataModel::~ServerDataModel() {
}

// The roller publishes into this LiveService; the app attaches its watermark
// and health queries before the stream server starts.
void ServerDataModel::startRollerServing() {
    const auto& rc = m_serverConfig.recording;
    const auto& shadow = m_serverConfig.rollerShadow;
    if (!rc.enabled) {
        sLog_App("Recording disabled (recording.enabled=false)");
        return;
    }
    if (const auto problem = recordingSourceProblem(m_serverConfig); !problem.empty()) {
        sLog_Error("Recording not served: " << problem);
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
        m_recordingLive = std::make_shared<recording::LiveService>(recording::liveCadenceMs(rc.livePublishMs));
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

std::function<void(const std::string&)> ServerDataModel::rollerRetract() {
    if (!m_servesRoller || !m_recordingLive) return {};
    return [live = m_recordingLive](const std::string& symbol) { live->retractProvisional(symbol); };
}

std::function<bool(const std::string&, const std::string&, recording::RecordPtr)> ServerDataModel::rollerEnsureFinal() {
    if (!m_servesRoller || !m_recordingLive) return {};
    return [live = m_recordingLive](const std::string& symbol, const std::string& layer, recording::RecordPtr final) {
        return live->ensureFinal(symbol, layer, std::move(final));
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
                     << " lastColumnMs=" << stall.lastColumnMs << " overdueMs=" << stall.overdueMs);
}

void ServerDataModel::onProductMetadata(const std::string& symbol,
                                        const nlohmann::json& metadata) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end() || symbol == "BTC-USD") return;
    auto& feed = it->second;
    try {
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
        auto& data = ensureSymbol(symbol);
        data.clearAggregatedBook();
        emit bookSnapshotBroadcast(QString::fromStdString(symbol), {}, {}, 0.0,
                                   QStringLiteral("metadata_unavailable"),
                                   data.liveBook.stateVersion());
        sLog_Warning("Live book metadata unavailable: symbol=" << symbol << " error=" << e.what());
    }
}
void ServerDataModel::onFeedMetadata(const std::string& symbol, const nlohmann::json& metadata) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end()) return;
    onProductMetadata(symbol, metadata);
}
void ServerDataModel::onFeedWatermark(const std::string& symbol, int64_t journalMs) {
    if (auto it = m_flowProducts.find(symbol); it != m_flowProducts.end()) {
        it->second.clock = std::max(it->second.clock, journalMs - kFlowCloseDelayMs);
        // Direct model users and legacy sinks still supply a useful receive M.
        it->second.receiveMax = std::max(it->second.receiveMax, journalMs);
        updateFlowRetentionFloor(symbol, it->second.receiveMax);
        for (auto& [day, state] : it->second.days)
            pruneFlowSeries(symbol, it->second.clock, 1000, state.series.at(1000));
    }
    if (!m_aggregator) return;
    // Receive time leads exchange time by the feed latency: close only buckets
    // that ended kJournalCandleLagMs before it, so in-flight trades still land.
    m_aggregator->advance(symbol, journalMs - kJournalCandleLagMs);
}
void ServerDataModel::flowTradePosted(const std::string& symbol) noexcept {
    if (const auto it = m_flowReceiveStates.find(symbol); it != m_flowReceiveStates.end())
        it->second.pendingTrades.fetch_add(1, std::memory_order_release);
}
void ServerDataModel::flowRecordApplied(const std::string& symbol, int64_t receiveMax) noexcept {
    if (const auto it = m_flowReceiveStates.find(symbol); it != m_flowReceiveStates.end()) {
        auto& value = it->second.maximum;
        auto old = value.load(std::memory_order_relaxed);
        while (old < receiveMax && !value.compare_exchange_weak(old, receiveMax,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }
}
void ServerDataModel::onFlowTrade(const Trade& trade, int64_t receiveMax) {
    auto& product = m_flowProducts[trade.product_id];
    product.receiveMax = std::max(product.receiveMax, receiveMax);
    product.clock = std::max(product.clock, product.receiveMax - kFlowCloseDelayMs);
    updateFlowRetentionFloor(trade.product_id, product.receiveMax);
    onTrade(trade);
    if (const auto it = m_flowReceiveStates.find(trade.product_id); it != m_flowReceiveStates.end())
        it->second.pendingTrades.fetch_sub(1, std::memory_order_release);
}
std::vector<int64_t> ServerDataModel::candleTimeframes() const {
    return m_aggregator ? m_aggregator->timeframes() : std::vector<int64_t>{};
}
void ServerDataModel::setCandleHistoryWarming(const std::vector<std::string>& products) {
    for (const auto& product : products) m_candleHistoryReady.try_emplace(product, false);
    sLog_App("Candle history warming products=" << m_candleHistoryReady.size());
}
bool ServerDataModel::candleHistoryReady(const std::string& symbol) const {
    const auto it = m_candleHistoryReady.find(symbol);
    return it == m_candleHistoryReady.end() || it->second.load(std::memory_order_acquire);
}
void ServerDataModel::onCandleHistoryRestored(const std::string& symbol,
                                              std::optional<sentinel::roller::CandleRestore> restore) {
    const auto it = m_candleHistoryReady.find(symbol);
    if (it == m_candleHistoryReady.end()) {
        sLog_Error("Candle history restore for a product that is not warming symbol=" << symbol);
        return;
    }
    const char* restored = "none";
    size_t bars100 = 0, bars1000 = 0, openBars = 0;
    int64_t installUs = 0;
    if (restore && m_aggregator) {
        const auto began = std::chrono::steady_clock::now();
        bool empty = true;
        for (const auto& [tf, series] : restore->series)
            empty = empty && m_aggregator->canRestoreSeries(symbol, tf);
        if (empty) {
            restored = restore->outcome == sentinel::roller::RestoreOutcome::complete ? "complete" : "partial";
            for (auto& [tf, series] : restore->series) {
                if (tf == 100) bars100 += series.closed.size();
                else if (tf == 1'000) bars1000 += series.closed.size();
                else if (series.active.count != 0 || series.active.timestamp_ms != 0) ++openBars;
                m_aggregator->restoreSeries(symbol, tf, std::move(series));
            }
        } else {
            restored = "refused";
            sLog_Error("Candle history restore refused: series not empty symbol=" << symbol);
        }
        installUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - began).count();
    }
    it->second.store(true, std::memory_order_release);
    const auto through = restore ? restore->through : sentinel::roller::JournalPos{};
    sLog_App("Candle history ready symbol=" << symbol << " restored=" << restored << " bars100=" << bars100
             << " bars1000=" << bars1000 << " openBars=" << openBars << " through=" << through.run << "/" << through.block << "/"
             << through.record << " throughMs=" << (restore ? restore->throughMs : 0)
             << " readMs=" << (restore ? restore->readMs : 0.0) << " installUs=" << installUs);
}
void ServerDataModel::setReseedHandler(std::function<void(const std::string&)> handler) {
    m_reseed = std::move(handler);
    if (!m_reseed) {
        m_reseedTimer.stop();
        m_pendingReseeds.clear();
    }
}
// The journal feed holds the whole book: a fresh snapshot
// replaces "await the next upstream snapshot". At most once per symbol per second.
void ServerDataModel::requestReseed(const std::string& symbol, const char* why) {
    if (!m_reseed) return;
    const auto now = localNowMs();
    auto& next = m_nextReseedMs[symbol];
    if (now < next) {
        // Coalesce: the book stays invalid until a snapshot arrives, so the
        // request is kept and delivered at the deadline.
        m_pendingReseeds.insert(symbol);
        const int wait = int(next - now);
        if (!m_reseedTimer.isActive() || m_reseedTimer.remainingTime() > wait) m_reseedTimer.start(wait);
        sLog_Data("Live book re-seed deferred: symbol=" << symbol << " reason=" << why << " waitMs=" << wait);
        return;
    }
    next = now + 1000;
    m_pendingReseeds.erase(symbol);
    sLog_Data("Live book re-seed requested: symbol=" << symbol << " reason=" << why);
    m_reseed(symbol);
}
void ServerDataModel::deliverReseeds() {
    const auto pending = std::exchange(m_pendingReseeds, {});
    for (const auto& symbol : pending) requestReseed(symbol, "deferred");
}
void ServerDataModel::onMarketDataConnectionChanged(const std::string& symbol, bool connected) {
    const auto it = m_feeds.find(symbol);
    if (it == m_feeds.end()) return; // uncaptured product: never create a series
    auto& feed = it->second;
    if (feed.connected == connected) return;
    feed.connected = connected;
    ++(connected ? feed.ups : feed.downs);
    if (!connected) {
        std::lock_guard lock(m_tradeMutex);
        m_tradeHistoryAwaitingResume.insert(symbol);
        m_tradeHistoryFloorMs[symbol] = INT64_MAX;
    }
}

void ServerDataModel::registerMetrics(sentinel::metrics::MetricsRegistry& r) {
    using Value = std::optional<double>;
    const auto load = [](const std::atomic<uint64_t>& a) { return [&a]() -> Value { return double(a.load(std::memory_order_relaxed)); }; };
    using Registry = sentinel::metrics::MetricsRegistry;
    const auto sampleFeeds = [this](auto value) {
        return [this, value] {
            std::vector<Registry::Sample> samples;
            for (const auto& [symbol, feed] : m_feeds)
                samples.push_back({{{"product", symbol}}, double(value(feed))});
            return samples;
        };
    };
    r.familyFn("sentinel_live_feed_up", "1 while this product's journal feed is up.", Registry::Type::Gauge,
               sampleFeeds([](const FeedState& f) { return f.connected; }));
    r.familyFn("sentinel_live_feed_up_total", "Product journal feed up transitions in this process.", Registry::Type::Counter,
               sampleFeeds([](const FeedState& f) { return f.ups; }));
    r.familyFn("sentinel_live_feed_down_total", "Product journal feed down transitions in this process.", Registry::Type::Counter,
               sampleFeeds([](const FeedState& f) { return f.downs; }));
    r.counterFn("sentinel_flow_dropped_trades_total", "Flow series additions rejected by retention, capacity, grid or validation.", {},
                [this]() -> Value { return double(m_flowDroppedTrades); });
    r.counterFn("sentinel_flow_dropped_buckets_total", "Published flow buckets retired from live retention.", {},
                [this]() -> Value { return double(m_flowDroppedBuckets); });
    r.counterFn("sentinel_flow_missing_grid_trades_total", "Flow trades dropped when the day-grid hold overflows or expires.", {},
                [this]() -> Value { return double(m_flowMissingGridTrades); });
    r.counterFn("sentinel_flow_hour_shrink_refusals_total", "Closed flow hour recompositions refused because source minutes were lost.", {},
                [this]() -> Value { return double(m_flowHourShrinkRefusals); });
    r.familyFn("sentinel_flow_live_late_trades_total", "Live flow trades excluded at their receive-time seal point.",
               Registry::Type::Counter, [this] {
                   std::vector<Registry::Sample> samples;
                   for (const auto& [symbol, count] : m_flowLateTrades)
                       samples.push_back({{{"instrument", "coinbase:" + symbol}}, double(count)});
                   return samples;
               });
    r.familyFn("sentinel_flow_live_seal_mismatch_total", "Live-final 1m bars differing from their sealed disk bar.",
               Registry::Type::Counter, [this] {
                   std::vector<Registry::Sample> samples;
                   for (const auto& [symbol, count] : m_flowSealMismatches)
                       samples.push_back({{{"instrument", "coinbase:" + symbol}}, double(count)});
                   return samples;
               });
    r.gaugeFn("sentinel_exchange_clock_offset_ms", "Smoothed local minus exchange clock in ms (0 = not yet measured).", {},
              [this]() -> Value { return double(m_exchangeOffsetMs.load(std::memory_order_relaxed)); });
    r.familyFn("sentinel_candle_late_trades_total", "Trades the candle aggregator dropped as older than its open bucket.",
               Registry::Type::Counter, [this] {
                   std::vector<Registry::Sample> samples;
                   for (const auto& drops : m_aggregator->lateDrops())
                       samples.push_back({{{"product", drops.symbol}, {"timeframe_ms", std::to_string(drops.timeframeMs)}},
                                          double(drops.count)});
                   return samples;
               });
    r.familyFn("sentinel_candle_history_warming",
               "1 while this product's startup candle rebuild (100 ms / 1 s window, 1 m .. 1 d open bars) has not "
               "ended (100 ms / 1 s history requests reply warming).",
               Registry::Type::Gauge, [this] {
                   std::vector<Registry::Sample> samples;
                   for (const auto& [symbol, ready] : m_candleHistoryReady)
                       samples.push_back({{{"product", symbol}}, ready.load(std::memory_order_acquire) ? 0.0 : 1.0});
                   return samples;
               });
    r.gaugeFn("sentinel_recorder_running", "1 when the journal roller is attached.", {},
              [this]() -> Value { return recordingAvailable() ? 1.0 : 0.0; });
    if (!m_servesRoller) return;

    r.counter("sentinel_recorder_columns_written_total",
              "Minute columns the serving roller committed across products and layers.");
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

std::vector<OHLCVBar> ServerDataModel::getHistory(const std::string& symbol, int64_t timeframeMs, size_t limit) const {
    if (m_aggregator) {
        return m_aggregator->getHistory(symbol, timeframeMs, limit);
    }
    return {};
}

std::vector<OHLCVBar> ServerDataModel::getHistory(const std::string& symbol, int64_t timeframeMs,
                                                  int64_t endMs, size_t limit) const {
    if (m_aggregator) return m_aggregator->getHistory(symbol, timeframeMs, endMs, limit);
    return {};
}

trade_overlay::LiveSnapshot ServerDataModel::collectLiveOverlay(const trade_overlay::Request& request) const {
    std::lock_guard lock(m_tradeMutex);
    const auto it = m_recentTrades.find(request.symbol);
    static const std::deque<RetainedTradeSample> empty;
    return trade_overlay::aggregateLive(request, it == m_recentTrades.end() ? empty : it->second);
}

// Worker-only bounded snapshot. False means over budget, never a partial tape.
bool ServerDataModel::collectOverlayTrades(const std::string& symbol, int64_t startMs,
                                          int64_t endMs, size_t limit,
                                          std::vector<RetainedTradeSample>& out,
                                          int64_t* retainedFromMs, OverlayTradeBudget budget,
                                          bool* budgetExceeded, size_t* heapOperations) const {
    OverlayTradeSelection selection(startMs, endMs, limit, out, budget);
    if (retainedFromMs) *retainedFromMs = 0;
    if (budgetExceeded) *budgetExceeded = false;
    if (heapOperations) *heapOperations = 0;
    if (endMs <= startMs) return true;
    {
        std::lock_guard<std::mutex> lock(m_tradeMutex);
        const auto it = m_recentTrades.find(symbol);
        if (it == m_recentTrades.end()) return true;
        if (budget == OverlayTradeBudget::Newest) {
            // The tape is nearly chronological. Starting at the newest end
            // avoids a heap replacement per trade while holding the ingest lock.
            for (auto trade = it->second.rbegin(); trade != it->second.rend(); ++trade)
                selection.add(*trade);
        } else {
            for (const auto& trade : it->second) {
                if (!selection.add(trade)) {
                    if (retainedFromMs) *retainedFromMs = selection.retainedFromMs();
                    return false;
                }
            }
        }
    }
    // Restore arrival order before the stable sort so timestamp ties remain
    // identical to Reject when the whole window fits the budget.
    if (budget == OverlayTradeBudget::Newest && !selection.budgetExceeded())
        std::reverse(out.begin(), out.end());
    selection.finish();
    if (retainedFromMs) *retainedFromMs = selection.retainedFromMs();
    if (budgetExceeded) *budgetExceeded = selection.budgetExceeded();
    if (heapOperations) *heapOperations = selection.heapOperations();
    return true;
}

ServerDataModel::TradeHistorySnapshot ServerDataModel::tradeHistorySnapshot(
    const std::string& symbol, int64_t fromMs, int64_t toMs, size_t limit) const {
    TradeHistorySnapshot result;
    result.store = m_flowStore;
    std::lock_guard lock(m_tradeMutex);
    const auto it = m_recentTrades.find(symbol);
    if (it == m_recentTrades.end() || it->second.empty()) return result;
    const bool fallbackFloor = !m_tradeHistoryFloorMs.contains(symbol);
    if (const auto floor = m_tradeHistoryFloorMs.find(symbol); floor != m_tradeHistoryFloorMs.end())
        result.retainedFromMs = floor->second;
    else result.retainedFromMs = INT64_MAX;
    const auto& trades = it->second;
    if (fallbackFloor) result.retainedFromMs = trades.front().timestampMs;
    const auto first = std::lower_bound(trades.begin(), trades.end(), std::max(fromMs, result.retainedFromMs),
        [](const RetainedTradeSample& row, int64_t ms) { return row.timestampMs < ms; });
    const auto last = std::lower_bound(first, trades.end(), toMs,
        [](const RetainedTradeSample& row, int64_t ms) { return row.timestampMs < ms; });
    result.rows.reserve(std::min(limit, static_cast<size_t>(last - first)));
    for (auto row = first; row != last; ++row) {
        if (result.rows.size() + result.invalidRows >= limit) {
            result.exceededLimit = true;
            break;
        }
        protocol::tradewire::Row candidate{row->id, row->timestampMs, row->price, row->size, row->side, 1};
        if (!protocol::tradewire::validRow(candidate, fromMs, toMs)) { ++result.invalidRows; continue; }
        result.rows.push_back(std::move(candidate));
    }
    return result;
}

void ServerDataModel::onTrade(const Trade& trade) {
    if (!m_feeds.contains(trade.product_id)) return;
    const int64_t exchangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    // Direct model feeds have no roller receive hand-off; their exchange time
    // is the only available clock until a feed watermark arrives.
    auto& flowProduct = m_flowProducts[trade.product_id];
    if (flowProduct.receiveMax == INT64_MIN) flowProduct.receiveMax = exchangeMs;
    updateExchangeOffsetMs(exchangeMs);
    addFlowTrade(trade, exchangeMs);

    if (m_aggregator) {
        m_aggregator->onTrade(trade);
    }
    
    auto& data = ensureSymbol(trade.product_id);
    data.lastTradePrice = trade.price;

    if (exchangeMs > 0 && trade.price > 0.0 && trade.size > 0.0) {
        std::lock_guard<std::mutex> lock(m_tradeMutex);
        auto& trades = m_recentTrades[trade.product_id];
        const int64_t newestMs = trades.empty() ? exchangeMs : std::max(exchangeMs, trades.back().timestampMs);
        const int64_t cutoff = newestMs - m_tradeRetentionMs;
        auto& historyFloor = m_tradeHistoryFloorMs[trade.product_id];
        if (m_tradeHistoryAwaitingResume.contains(trade.product_id)) {
            if (m_feeds.at(trade.product_id).connected) {
                historyFloor = exchangeMs;
                m_tradeHistoryAwaitingResume.erase(trade.product_id);
            }
        } else if (!historyFloor) historyFloor = exchangeMs;
        // A late row does not prove coverage before the first live trade,
        // including before the first reconnect. Read that prefix from the journal.
        historyFloor = std::max(historyFloor, cutoff);
        while (!trades.empty() && trades.front().timestampMs < cutoff) {
            trades.pop_front();
        }
        if (exchangeMs >= cutoff) {
            RetainedTradeSample sample{exchangeMs, trade.price, trade.size, trade.side, trade.trade_id};
            if (trades.empty() || trades.back().timestampMs <= exchangeMs) trades.push_back(std::move(sample));
            else trades.insert(std::upper_bound(trades.begin(), trades.end(), exchangeMs,
                [](int64_t ms, const RetainedTradeSample& row) { return ms < row.timestampMs; }), std::move(sample));
        }
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
                requestReseed(symbol, "raw band exceeded");
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
    {
        std::lock_guard lock(m_tradeMutex);
        if (symbol.empty()) {
            for (const auto& [product, feed] : m_feeds) {
                m_tradeHistoryAwaitingResume.insert(product);
                m_tradeHistoryFloorMs[product] = INT64_MAX;
            }
        } else if (m_feeds.contains(symbol)) {
            m_tradeHistoryAwaitingResume.insert(symbol);
            m_tradeHistoryFloorMs[symbol] = INT64_MAX;
        }
    }
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
    sLog_Data("ServerDataModel: book invalid until next snapshot: symbol="
              << (symbol.empty() ? std::string("*") : symbol) << " symbols=" << count
              << " reason=" << reason);
}

void ServerDataModel::onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, qint64 envelopeMs) {
    std::string symbol = productId.toStdString();
    if (!m_feeds.contains(symbol)) return;
    SymbolHotData& data = ensureSymbol(symbol);
    

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
        requestReseed(symbol, "raw BBO unavailable");
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

ServerDataModel::FlowSeries::FlowSeries(int64_t tf, const sentinel::flow::FlowScales& scales)
    : builder(tf, scales.nativeTick, scales.priceScale, scales.baseScale, scales.quoteScale),
      bucketSlots(tf == 1000 ? 1802 : 422) {}

void ServerDataModel::onFlowGrid(const std::string& symbol, int64_t dayStartMs,
                                 const sentinel::roller::Grid& grid,
                                 const nlohmann::json& metadata) {
    if (!m_feeds.contains(symbol) || dayStartMs < 0 || dayStartMs % 86'400'000 != 0) return;
    try {
        const auto scales = sentinel::flow::flowScales(grid, metadata);
        auto& product = m_flowProducts[symbol];
        if (!product.days.empty() && dayStartMs < product.days.rbegin()->first - 86'400'000) return;
        if (auto it = product.days.find(dayStartMs); it != product.days.end()) {
            const auto& oldGrid = it->second.grid;
            if (it->second.scales == scales && oldGrid.priceScale == grid.priceScale &&
                oldGrid.nearTick == grid.nearTick && oldGrid.deepTick == grid.deepTick &&
                oldGrid.sizeFloor == grid.sizeFloor) return;
            if (it->second.hasBuckets) {
                sLog_Warning("Flow grid changed after trades: symbol=" << symbol << " day=" << dayStartMs
                    << " old=[" << oldGrid.priceScale << ',' << oldGrid.nearTick << ','
                    << oldGrid.deepTick << ',' << oldGrid.sizeFloor << ',' << it->second.scales.baseScale
                    << "] new=[" << grid.priceScale << ',' << grid.nearTick << ','
                    << grid.deepTick << ',' << grid.sizeFloor << ',' << scales.baseScale << ']');
                return;
            }
            product.days.erase(it);
        }
        FlowDay day;
        day.grid = grid;
        day.scales = scales;
        day.series.try_emplace(1000, 1000, scales);
        day.series.try_emplace(60'000, 60'000, scales);
        product.days.emplace(dayStartMs, std::move(day));
        product.scratch.id.reserve(128);
        product.days.erase(product.days.begin(), product.days.lower_bound(dayStartMs - 86'400'000));
        expireGridTrades(symbol, product);
        size_t heldReplayed = 0;
        for (auto it = product.heldTrades.begin(); it != product.heldTrades.end();) {
            if (it->exchangeMs / 86'400'000 * 86'400'000 != dayStartMs) { ++it; continue; }
            addFlowTrade(it->trade, it->exchangeMs, it->lateMinute);
            product.heldBytes -= it->bytes;
            it = product.heldTrades.erase(it);
            ++heldReplayed;
        }
        sLog_Data("Flow grid ready: symbol=" << symbol << " day=" << dayStartMs
            << " rowTickUnits=" << scales.rowTickUnits << " priceScale=" << scales.priceScale
            << " baseScale=" << scales.baseScale << " held_replayed=" << heldReplayed);
    } catch (const std::exception& e) {
        sLog_Warning("Flow grid refused: symbol=" << symbol << " day=" << dayStartMs
            << " error=" << e.what());
    }
}

bool ServerDataModel::flowHourSealed(const std::string& symbol, int64_t hour) const {
    const auto instrument = m_flowSealed.find(symbol);
    if (instrument == m_flowSealed.end()) return false;
    const auto day = instrument->second.find(hour / 86'400'000 * 86'400'000);
    return day != instrument->second.end() && day->second.hours.contains(hour);
}
std::optional<sentinel::flow::FlowBar> ServerDataModel::flowSealedMinute(
        const std::string& symbol, int64_t minute) const {
    const auto instrument = m_flowSealed.find(symbol);
    if (instrument == m_flowSealed.end()) return {};
    const auto day = instrument->second.find(minute / 86'400'000 * 86'400'000);
    if (day == instrument->second.end()) return {};
    const auto bar = day->second.bars.find(minute);
    return bar == day->second.bars.end() ? std::nullopt : std::optional(bar->second);
}
void ServerDataModel::onFlowChunkIndexChange(const std::string& instrument,
        std::optional<int64_t> day, uint64_t serial) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!instrument.starts_with("coinbase:")) return;
    const auto symbol = instrument.substr(9);
    auto& days = m_flowSealed[symbol];
    auto& latestEvent = m_flowLatestIndexSerial[symbol];
    if (!day) {
        if (serial < latestEvent) return;
        latestEvent = serial;
        m_flowEpochFenceSerial[symbol] = serial;
        days.clear(); m_flowLatestSealedDay.erase(symbol); return;
    }
    latestEvent = std::max(latestEvent, serial);
    if (serial < m_flowEpochFenceSerial[symbol]) return;
    if (const auto latest = m_flowLatestSealedDay.find(symbol);
        latest != m_flowLatestSealedDay.end() && *day < latest->second - 86'400'000) return;
    auto& state = days[*day];
    if (serial < state.serial) return;
    state = {}; state.serial = serial;
}
void ServerDataModel::onFlowChunkDayPublished(const std::string& instrument, int64_t day,
        uint64_t serial, std::vector<sentinel::flow::FlowBar> bars, std::vector<int64_t> hours) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!instrument.starts_with("coinbase:")) return;
    const auto symbol = instrument.substr(9);
    m_flowLatestIndexSerial[symbol] = std::max(m_flowLatestIndexSerial[symbol], serial);
    if (serial < m_flowEpochFenceSerial[symbol]) return;
    auto& days = m_flowSealed[symbol];
    auto& latest = m_flowLatestSealedDay[symbol];
    latest = std::max(latest, day);
    days.erase(days.begin(), days.lower_bound(latest - 86'400'000));
    if (day < latest - 86'400'000) return;
    auto& state = days[day];
    if (serial < state.serial) return;
    state = {}; state.serial = serial;
    state.hours.insert(hours.begin(), hours.end());
    for (auto& bar : bars) state.bars.emplace(bar.startMs, std::move(bar));
    const auto product = m_flowProducts.find(symbol);
    if (product == m_flowProducts.end()) return;
    const auto liveDay = product->second.days.find(day);
    if (liveDay == product->second.days.end()) return;
    auto& minutes = liveDay->second.series.at(60'000);
    for (auto& slot : minutes.bucketSlots) {
        if (slot.start < product->second.firstLiveMs ||
            flowLossIn(product->second.lossyMinutes, slot.start, slot.start + 60'000) ||
            product->second.receiveMax < slot.start + 60'000 + m_serverConfig.flow.sealWindowMs ||
            slot.compared || !state.hours.contains(slot.start / 3'600'000 * 3'600'000)) continue;
        auto live = minutes.builder.snapshotBar(slot.start);
        if (!live) continue;
        live->notable.clear(); // FLC1 stores rows and summary, not notable prints.
        const auto sealed = state.bars.find(slot.start);
        if (sealed == state.bars.end() || *live != sealed->second)
            ++m_flowSealMismatches[symbol];
        slot.compared = true;
    }
    for (auto& [key, coarse] : liveDay->second.coarse) coarse.dirty = true;
}

void ServerDataModel::attachFlowStore(recording::FlowStore* store) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_flowStore = store;
    m_flowWakeThrough.clear();
    m_flowRetentionFloor.clear();
    if (store)
        for (const auto& [symbol, product] : m_flowProducts)
            if (product.clock > 0) updateFlowRetentionFloor(symbol, product.clock);
}

void ServerDataModel::updateFlowRetentionFloor(const std::string& symbol, int64_t clock) {
    if (!m_flowStore) return;
    const auto normal = std::max<int64_t>(0, clock - kFlowRolloverGraceMs) /
        3'600'000 * 3'600'000;
    const auto floor = std::max<int64_t>(0, normal - 6 * 3'600'000);
    auto [it, inserted] = m_flowRetentionFloor.try_emplace(symbol, floor);
    if (!inserted && it->second == floor) return;
    it->second = floor;
    m_flowStore->setLiveRetentionFloor(symbol, floor);
}

bool ServerDataModel::flowOwnsBucket(const FlowSubscription& sub, int64_t start) const {
    const auto product = m_flowProducts.find(sub.symbol);
    if (product == m_flowProducts.end() || product->second.receiveMax == INT64_MIN) return false;
    const auto floor = flowMinuteRetainedFrom(sub.symbol, product->second.receiveMax);
    const auto end = start + sub.timeframeMs;
    if (end <= floor || start > product->second.receiveMax) return false;
    const auto sealed = std::atomic_load_explicit(&sub.sealedHours, std::memory_order_acquire);
    for (auto minute = std::max(start, floor) / 60'000 * 60'000;
         minute < end && minute <= product->second.receiveMax; minute += 60'000)
        if (!sealed || !sealed->contains(minute / 3'600'000 * 3'600'000)) return true;
    return false;
}

std::optional<ServerDataModel::LiveFlowBar> ServerDataModel::composeLiveFlowBar(
        const FlowSubscription& sub, int64_t start) const {
    const auto product = m_flowProducts.find(sub.symbol);
    if (product == m_flowProducts.end()) return {};
    const auto tf = sub.timeframeMs;
    const auto sealed = std::atomic_load_explicit(&sub.sealedHours, std::memory_order_acquire);
    std::vector<sentinel::flow::FlowBar> parts;
    LiveFlowBar result;
    result.complete = product->second.firstLiveMs >= 0 && product->second.firstLiveMs <= start &&
        !flowLossIn(product->second.lossyMinutes, start, start + tf);
    const auto dayStart = start / 86'400'000 * 86'400'000;
    if (const auto day = product->second.days.find(dayStart); day != product->second.days.end()) {
        const auto& retired = day->second.retiredRevisionByMinute;
        for (auto it = retired.lower_bound(start); it != retired.end() && it->first < start + tf; ++it)
            result.revision += it->second;
    }
    std::map<int64_t, sentinel::flow::FlowBar> fallbackHours;
    for (auto minute = start; minute < start + tf && minute <= product->second.receiveMax; minute += 60'000) {
        const auto hour = minute / 3'600'000 * 3'600'000;
        const bool disk = flowHourSealed(sub.symbol, hour);
        const bool sessionSealed = sealed && sealed->contains(hour);
        const auto day = product->second.days.find(minute / 86'400'000 * 86'400'000);
        const auto* builder = day == product->second.days.end() ? nullptr :
            &day->second.series.at(60'000).builder;
        if (builder) result.revision += builder->revisionAt(minute).value_or(0);
        if (disk) {
            if (auto bar = flowSealedMinute(sub.symbol, minute)) parts.push_back(std::move(*bar));
            // A sealed empty minute is covered without a row bar.
            continue;
        }
        if (!sealed && tf >= 3'600'000 && hour >= start && hour + 3'600'000 <= start + tf &&
            day != product->second.days.end()) {
            const auto& retired = day->second.retiredRevisionByMinute;
            const auto first = retired.lower_bound(hour);
            if (first != retired.end() && first->first < hour + 3'600'000) {
                // The legacy GUI has no availability handover. When a whole
                // hour has left minute RAM but FLC1 is absent or invalidated,
                // its retained closed-hour composite keeps the forming day
                // growing without duplicating its remaining minute bars.
                if (!fallbackHours.contains(hour)) {
                    const auto old = day->second.hours.find(hour);
                    if (old == day->second.hours.end()) return {};
                    fallbackHours.emplace(hour, old->second.bar);
                    result.complete &= old->second.complete;
                }
                continue;
            }
        }
        if (builder) {
            if (auto bar = builder->snapshotBar(minute)) {
                parts.push_back(std::move(*bar));
                continue;
            }
        }
        // An index epoch can invalidate the decoded seal after its live
        // minutes were retired. Availability-aware sessions see the hole.
        if (day != product->second.days.end() &&
            day->second.retiredRevisionByMinute.contains(minute)) {
            if (!sealed) return {};
            result.complete = false;
            result.building = true;
            continue;
        }
        // An observed empty minute (including the current forming minute) is
        // covered. A session ahead of the model's decoded seal, or an interval
        // before this process's live observation, has no proven content.
        if (sessionSealed || minute < product->second.firstLiveMs) {
            result.complete = false;
            result.building = true;
        }
    }
    if (parts.empty() && fallbackHours.empty()) {
        // Today's client has no availability subscription and its buffer,
        // footprint, and CVD treat count-0 carriers as actual zero columns.
        // Keep its last real bar until content arrives.
        if (!sealed) return {};
        const auto day = product->second.days.find(dayStart);
        if (day == product->second.days.end()) return {};
        const auto& s = day->second.scales;
        result.bar.startMs = start; result.bar.timeframeMs = tf;
        result.bar.nativeTick = s.nativeTick; result.bar.priceScale = s.priceScale;
        result.bar.baseScale = s.baseScale; result.bar.quoteScale = s.quoteScale;
    } else if (tf == 60'000) result.bar = std::move(parts.front());
    else {
        try {
            if (fallbackHours.empty()) result.bar = sentinel::flow::composeFlowBars(parts, tf);
            else {
                // The shared composer requires equal source timeframes. Fold
                // retained minutes into hours, then combine them with closed
                // hour fallbacks at the subscribed timeframe.
                std::vector<sentinel::flow::FlowBar> hours;
                hours.reserve(size_t(tf / 3'600'000));
                size_t next = 0;
                for (auto hour = start; hour < start + tf && hour <= product->second.receiveMax;
                     hour += 3'600'000) {
                    if (const auto old = fallbackHours.find(hour); old != fallbackHours.end()) {
                        hours.push_back(old->second);
                        continue;
                    }
                    const auto first = next;
                    while (next < parts.size() && parts[next].startMs < hour + 3'600'000) ++next;
                    if (next > first)
                        hours.push_back(sentinel::flow::composeFlowBars(
                            std::span<const sentinel::flow::FlowBar>(parts.data() + first, next - first),
                            3'600'000));
                }
                result.bar = sentinel::flow::composeFlowBars(hours, tf);
            }
        }
        catch (const std::exception& e) {
            const auto key = std::tuple{sub.symbol, tf, start};
            if (m_flowComposeWarned.insert(key).second)
                sLog_Warning("Flow live composition failed symbol=" << sub.symbol << " tfMs=" << tf
                    << " start=" << start << " error=" << e.what());
            if (m_flowComposeWarned.size() > 512) m_flowComposeWarned.erase(m_flowComposeWarned.begin());
            result.complete = false; result.building = true;
            if (!sealed) return {};
            const auto day = product->second.days.find(dayStart);
            if (day == product->second.days.end()) return {};
            const auto& s = day->second.scales;
            result.bar.startMs = start; result.bar.timeframeMs = tf;
            result.bar.nativeTick = s.nativeTick; result.bar.priceScale = s.priceScale;
            result.bar.baseScale = s.baseScale; result.bar.quoteScale = s.quoteScale;
        }
    }
    return result;
}

void ServerDataModel::onFlowHourThrough(const std::string& symbol, int64_t through) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_flowStore) return;
    auto& prior = m_flowWakeThrough[symbol];
    if (through > prior) {
        prior = through;
        m_flowStore->wake();
    }
}

void ServerDataModel::wakeFlowStoreOnRecorderHour(const std::string& symbol) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_flowStore || !m_rollerAttached.load()) return;
    const auto through = std::max(recordingWatermarks(symbol, "near").hourThroughMs,
                                  recordingWatermarks(symbol, "deep").hourThroughMs);
    onFlowHourThrough(symbol, through);
}

ServerDataModel::FlowHistorySnapshot ServerDataModel::flowHistorySnapshot(
    const std::string& symbol, int64_t tf, int64_t from, int64_t to) const {
    Q_ASSERT(QThread::currentThread() == thread());
    FlowHistorySnapshot snapshot;
    snapshot.store = m_flowStore;
    const auto found = m_flowProducts.find(symbol);
    snapshot.retainedFromMs = to;
    if (found == m_flowProducts.end() || found->second.firstLiveMs < 0) return snapshot;
    const auto& product = found->second;
    snapshot.observedThroughMs = product.clock + kFlowCloseDelayMs;
    const auto sourceTf = tf == 1000 ? 1000 : 60'000;
    const int64_t retained = sourceTf == 1000 ?
        product.clock / 1000 * 1000 - kFlowSecondRetentionMs + 1000 :
        flowMinuteRetainedFrom(symbol, product.clock);
    snapshot.retainedFromMs = retained;
    for (const auto& [dayStart, day] : product.days) {
        if (tf > 60'000) {
            for (const auto& [key, state] : day.coarse)
                if (key.first == tf && key.second >= from && key.second < to)
                    snapshot.liveRevisions[key.second] = state.revision;
            if (tf == 3'600'000) {
                for (const auto& [start, state] : day.hours)
                    if (start >= from && start < to)
                        snapshot.liveRevisions.try_emplace(start, state.revision);
                if (day.formingHour && day.formingHour->first >= from &&
                    day.formingHour->first < to)
                    snapshot.liveRevisions.try_emplace(day.formingHour->first,
                                                      day.formingHour->second.revision);
            }
        }
        const auto begin = std::max({from, dayStart, retained});
        const auto end = std::min({to, dayStart + 86'400'000, snapshot.observedThroughMs});
        if (begin >= end) continue;
        const auto& builder = day.series.at(sourceTf).builder;
        for (auto start = begin / sourceTf * sourceTf; start < end; start += sourceTf) {
            if (start < from) continue;
            const bool complete = product.firstLiveMs <= start && start >= retained &&
                !flowLossIn(product.lossyMinutes, start, start + sourceTf);
            if (start + sourceTf > product.firstLiveMs)
                snapshot.data.ranges.push_back({start, std::min(start + sourceTf, end),
                                               complete ? "complete" : "partial", day.scales});
            if (auto bar = builder.snapshotBar(start)) {
                snapshot.data.bars.push_back(std::move(*bar));
                snapshot.data.revisions.push_back(*builder.revisionAt(start));
            }
        }
    }
    return snapshot;
}

void ServerDataModel::subscribeFlow(std::shared_ptr<FlowSubscription> sub) {
    if (!sub->active.load()) return;
    const auto tf = sub->timeframeMs;
    const auto& configured = m_serverConfig.chart.timeframesMs;
    const bool supported = tf == 1000 || tf == 60'000 ||
        (tf >= 60'000 && tf <= 86'400'000 && 86'400'000 % tf == 0 &&
         std::find(configured.begin(), configured.end(), tf) != configured.end());
    std::erase_if(m_flowSubscriptions, [](const auto& s) { return !s->active.load(); });
    if (!supported || !m_feeds.contains(sub->symbol) || m_flowSubscriptions.size() >= 256) {
        if (sub->publishError) sub->publishError(nlohmann::json{{"type", "error"},
            {"context", "flow_live_subscribe"}, {"sub", sub->sub}, {"symbol", sub->symbol},
            {"message", "unsupported flow series or capacity reached"}}.dump());
        sub->active.store(false);
        return;
    }
    m_flowProducts.try_emplace(sub->symbol);
    if (const auto state = m_flowReceiveStates.find(sub->symbol);
        state != m_flowReceiveStates.end()) {
        auto& product = m_flowProducts.at(sub->symbol);
        const auto receive = state->second.maximum.load(std::memory_order_acquire);
        // The producer posts each trade before releasing this maximum. An
        // acquire of maximum followed by pending excludes that earlier post.
        if (state->second.pendingTrades.load(std::memory_order_acquire) == 0 &&
            receive > product.receiveMax) {
            product.receiveMax = receive;
            product.clock = std::max(product.clock, receive - kFlowCloseDelayMs);
            updateFlowRetentionFloor(sub->symbol, receive);
        }
    }
    if (tf != 1000) {
        const auto& product = m_flowProducts.at(sub->symbol);
        if (product.receiveMax != INT64_MIN) {
            const auto floor = flowMinuteRetainedFrom(sub->symbol, product.receiveMax);
            for (auto start = floor / tf * tf; start <= product.receiveMax; start += tf)
                if (flowOwnsBucket(*sub, start) && composeLiveFlowBar(*sub, start))
                    sub->replayBuckets.push_back(start);
        }
        if (sub->startReplay) {
            sub->initial = false;
            sub->startReplay(product.receiveMax == INT64_MIN ? 0 :
                flowMinuteRetainedFrom(sub->symbol, product.receiveMax), sub->replayBuckets.size());
        }
    }
    m_flowSubscriptions.push_back(std::move(sub));
}

void ServerDataModel::nextFlowReplayFrame(const std::shared_ptr<FlowSubscription>& sub) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!sub->active.load() || !sub->replayFrame) return;
    while (sub->replayCursor < sub->replayBuckets.size()) {
        const auto start = sub->replayBuckets[sub->replayCursor++];
        if (!flowOwnsBucket(*sub, start)) continue;
        const auto composed = composeLiveFlowBar(*sub, start);
        if (!composed) continue;
        const auto& bar = composed->bar;
        const auto bytes = sentinel::flow::encodeFlowBar(bar);
        const auto encoded = QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()).toBase64();
        const auto& product = m_flowProducts.at(sub->symbol);
        nlohmann::json payload{{"type", "flow_bar"},
            {"schema_version", protocol::SentinelProtocol::kFlowSchemaVersion}, {"sub", sub->sub},
            {"symbol", sub->symbol}, {"timeframe_ms", sub->timeframeMs},
            {"bucket_start_ms", start}, {"closed", product.clock >= start + sub->timeframeMs},
            {"complete", composed->complete}, {"revision", composed->revision},
            {"source", "live"}, {"encoding", "FLB1/base64"}, {"bar", encoded.toStdString()},
            {"row_tick_units", std::llround(bar.nativeTick * bar.priceScale)},
            {"price_scale", bar.priceScale}};
        if (composed->building) payload["coverage"] = "building";
        sub->replayFrame(payload.dump(), false);
        return;
    }
    sub->replayFrame({}, true);
}
void ServerDataModel::onFlowAvailabilityChanged(const std::string& symbol) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto product = m_flowProducts.find(symbol);
    if (product == m_flowProducts.end()) return;
    for (auto& [dayStart, day] : product->second.days) {
        for (auto& [key, state] : day.coarse) state.dirty = true;
        for (auto& slot : day.series.at(60'000).bucketSlots)
            if (slot.start >= 0) slot.dirty = true;
    }
}

void ServerDataModel::markFlowLoss(FlowProduct& product, int64_t ms) {
    const auto minuteStart = ms / 60'000 * 60'000, dayStart = ms / 86'400'000 * 86'400'000;
    if (product.lossyMinutes.insert(minuteStart).second) {
        product.newLossyMinutes.insert(minuteStart);
        if (const auto day = product.days.find(dayStart); day != product.days.end())
            for (auto& [tf, series] : day->second.series) {
                auto& slot = series.bucketSlots[(ms / tf) % series.bucketSlots.size()];
                if (slot.start == ms / tf * tf) slot.dirty = true;
            }
    }
}
void ServerDataModel::dropGridTrade(const std::string& symbol, FlowProduct& product, int64_t ms) {
    ++m_flowDroppedTrades; ++m_flowMissingGridTrades;
    markFlowLoss(product, ms);
    const auto now = std::chrono::steady_clock::now();
    if (!product.lastGridWarning || now - *product.lastGridWarning >= std::chrono::minutes(1)) {
        product.lastGridWarning = now;
        sLog_Warning("Flow trade dropped: symbol=" << symbol << " day=" << ms / 86'400'000 * 86'400'000
            << " reason=missing_grid hold_bytes=" << product.heldBytes << " hold_trades=" << product.heldTrades.size());
    }
}
void ServerDataModel::expireGridTrades(const std::string& symbol, FlowProduct& product) {
    if (product.heldTrades.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    while (!product.heldTrades.empty() && product.heldTrades.front().expires <= now) {
        const auto& held = product.heldTrades.front();
        dropGridTrade(symbol, product, held.exchangeMs);
        product.heldBytes -= held.bytes; product.heldTrades.pop_front();
    }
}
void ServerDataModel::addFlowTrade(const Trade& trade, int64_t ms, std::optional<bool> heldLate) {
    if (ms < 0) return;
    auto& product = m_flowProducts[trade.product_id];
    const bool lateMinute = heldLate.value_or(product.receiveMax >=
        ms / 60'000 * 60'000 + 60'000 + m_serverConfig.flow.sealWindowMs);
    if (product.firstLiveMs < 0) product.firstLiveMs = ms;
    const int64_t dayStart = ms / 86'400'000 * 86'400'000;
    const auto markLoss = [&] { markFlowLoss(product, ms); };
    const auto day = product.days.find(dayStart);
    if (day == product.days.end()) {
        expireGridTrades(trade.product_id, product);
        // Reject oversized inputs before copying either string. The conservative
        // charge includes deque/allocator overhead per admitted element.
        const auto charge = sizeof(FlowProduct::HeldTrade) + trade.trade_id.size() + trade.product_id.size() + 64;
        if (charge > kFlowGridHoldBytes - product.heldBytes || product.heldTrades.size() >= kFlowGridHoldTrades) {
            dropGridTrade(trade.product_id, product, ms); return;
        }
        product.heldTrades.push_back({trade, ms, lateMinute, charge,
            std::chrono::steady_clock::now() + kFlowGridHoldTime});
        product.heldBytes += charge;
        return;
    }
    // Classify only once the grid is available: a lost hold is missing-grid,
    // while a released hold keeps its original admission lateness.
    if (lateMinute) ++m_flowLateTrades[trade.product_id];
    auto& input = product.scratch;
    input.id = trade.trade_id;
    input.exchangeMs = ms; input.price = trade.price; input.size = trade.size;
    input.side = trade.side == AggressorSide::Buy ? sentinel::flow::Side::Buy :
                 trade.side == AggressorSide::Sell ? sentinel::flow::Side::Sell : sentinel::flow::Side::Unknown;
    for (auto& [tf, series] : day->second.series) {
        if (tf == 60'000 && (lateMinute || flowHourSealed(trade.product_id,
                ms / 3'600'000 * 3'600'000))) continue;
        const auto start = ms / tf * tf;
        // An expired RAM second is replayed from the journal; its rejection does not lose the minute.
        const bool expiredSecond = tf == 1000 &&
            ms < product.clock / 1000 * 1000 - kFlowSecondRetentionMs + 1000;
        auto& slot = series.bucketSlots[(ms / tf) % series.bucketSlots.size()];
        if (slot.start >= 0 && slot.start != start) {
            ++series.capacityDrops; ++m_flowDroppedTrades;
            if (!expiredSecond) markLoss();
            continue;
        }
        const auto dropped = series.builder.droppedRejectCount();
        const auto result = series.builder.add(input);
        if (result == sentinel::flow::FlowBarBuilder::AddResult::Accepted) {
            slot.start = start; slot.dirty = true;
            day->second.hasBuckets = true;
        } else if (result == sentinel::flow::FlowBarBuilder::AddResult::Rejected) {
            if (!expiredSecond) markLoss();
            if (series.builder.droppedRejectCount() == dropped) ++m_flowDroppedTrades;
        }
        m_flowDroppedTrades += series.builder.droppedRejectCount() - dropped;
    }
}

void ServerDataModel::publishFlowBar(const std::string& symbol, int64_t tf,
                                     const sentinel::flow::FlowBar& bar, uint32_t revision,
                                     bool closed, bool complete, bool initialOnly) {
    for (const auto& sub : m_flowSubscriptions) {
        if (!sub->active.load() || sub->symbol != symbol || sub->timeframeMs != tf ||
            (initialOnly && !sub->initial)) continue;
        if (tf != 1000 && !flowOwnsBucket(*sub, bar.startMs)) continue;
        const auto composed = tf == 1000 ? std::optional<LiveFlowBar>{} : composeLiveFlowBar(*sub, bar.startMs);
        if (tf != 1000 && !composed) continue;
        const auto& chosen = composed ? composed->bar : bar;
        const auto bytes = sentinel::flow::encodeFlowBar(chosen);
        const auto encoded = QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()).toBase64();
        nlohmann::json payload{{"type", "flow_bar"},
            {"schema_version", protocol::SentinelProtocol::kFlowSchemaVersion},
            {"sub", sub->sub}, {"symbol", symbol}, {"timeframe_ms", tf},
            {"bucket_start_ms", chosen.startMs}, {"closed", closed},
            {"complete", composed ? composed->complete : complete},
            {"revision", composed ? composed->revision : revision}, {"source", "live"},
            {"encoding", "FLB1/base64"}, {"bar", encoded.toStdString()},
            {"row_tick_units", std::llround(chosen.nativeTick * chosen.priceScale)},
            {"price_scale", chosen.priceScale}};
        if (composed && composed->building) payload["coverage"] = "building";
        sub->publish(payload.dump());
    }
}

void ServerDataModel::publishFlowSeries(const std::string& symbol, FlowProduct& product,
                                        int64_t tf, FlowSeries& series) {
    const auto finalEnd = product.clock / tf * tf;
    int64_t newest = -1;
    const bool anyInitial = std::any_of(m_flowSubscriptions.begin(), m_flowSubscriptions.end(),
        [&](const auto& s) { return s->active.load() && s->initial &&
            s->symbol == symbol && s->timeframeMs == tf; });
    if (anyInitial) for (const auto& slot : series.bucketSlots) newest = std::max(newest, slot.start);
    for (auto& slot : series.bucketSlots) {
        if (slot.start < 0) continue;
        // Exchange time can lead receive M. Keep this update dirty until M
        // actually reaches the bucket, rather than swallowing its first bar.
        if (tf == 60'000 && slot.start > product.receiveMax) continue;
        const bool closed = slot.start < finalEnd;
        if (tf == 60'000) {
            slot.final = product.receiveMax >= slot.start + tf + m_serverConfig.flow.sealWindowMs;
            if (slot.final && !slot.compared && slot.start >= product.firstLiveMs &&
                !flowLossIn(product.lossyMinutes, slot.start, slot.start + tf) &&
                flowHourSealed(symbol, slot.start / 3'600'000 * 3'600'000)) {
                auto live = series.builder.snapshotBar(slot.start);
                if (live) {
                    live->notable.clear();
                    const auto disk = flowSealedMinute(symbol, slot.start);
                    if (!disk || *live != *disk) ++m_flowSealMismatches[symbol];
                    slot.compared = true;
                }
            }
        }
        const bool changed = slot.dirty || (closed && !slot.closed);
        if (!changed && !(anyInitial && slot.start == newest)) continue;
        if (const auto bar = series.builder.bar(slot.start))
            publishFlowBar(symbol, tf, *bar, *series.builder.revisionAt(slot.start),
                           closed, product.firstLiveMs <= slot.start &&
                           !flowLossIn(product.lossyMinutes, slot.start, slot.start + tf), !changed);
        slot.dirty = false; slot.closed = closed;
    }
    series.builder.sealThrough(finalEnd);
    pruneFlowSeries(symbol, product.clock, tf, series);
}

int64_t ServerDataModel::flowMinuteRetainedFrom(const std::string& symbol, int64_t clock) const {
    if (clock <= 0) return 0;
    const auto normal = std::max<int64_t>(0, clock - kFlowRolloverGraceMs) / 3'600'000 * 3'600'000;
    (void)symbol;
    return std::max<int64_t>(0, normal - 6 * 3'600'000);
}

void ServerDataModel::pruneFlowSeries(const std::string& symbol, int64_t clock, int64_t tf, FlowSeries& series) {
    const auto finalEnd = clock / tf * tf;
    const int64_t drop = tf == 1000 ? finalEnd - kFlowSecondRetentionMs + tf :
        flowMinuteRetainedFrom(symbol, m_flowProducts.at(symbol).receiveMax);
    if (tf == 60'000) {
        const auto fldSealedThrough = m_flowStore ? m_flowStore->sealedThroughMs(symbol) : 0;
        auto& product = m_flowProducts.at(symbol);
        for (auto& slot : series.bucketSlots) {
            if (slot.start < 0) continue;
            const auto hour = slot.start / 3'600'000 * 3'600'000;
            const bool safeDisk = flowHourSealed(symbol, hour) &&
                fldSealedThrough >= slot.start + tf;
            if (slot.start < drop || safeDisk) {
                // Retain each minute's contribution so sub-hour and day
                // envelopes do not retreat when their source leaves RAM.
                product.days.at(slot.start / 86'400'000 * 86'400'000)
                    .retiredRevisionByMinute[slot.start] += series.builder.revisionAt(slot.start).value_or(0);
                series.builder.dropBucket(slot.start);
                slot = {}; ++m_flowDroppedBuckets;
            }
        }
        series.builder.dropBefore(drop);
        return;
    }
    int64_t safeDrop = drop;
    for (const auto& slot : series.bucketSlots)
        if (slot.start >= 0 && (!slot.closed || slot.dirty)) safeDrop = std::min(safeDrop, slot.start);
    series.builder.dropBefore(safeDrop);
    for (auto& slot : series.bucketSlots) if (slot.start >= 0 && slot.start < safeDrop) {
        slot = {}; ++m_flowDroppedBuckets;
    }
}

void ServerDataModel::updateHour(FlowDay& day, const FlowProduct& product, int64_t hourStart) {
    auto& minutes = day.series.at(60'000);
    std::vector<sentinel::flow::FlowBar> parts;
    uint32_t revision = 0;
    for (const auto& slot : minutes.bucketSlots) if (slot.start >= hourStart && slot.start < hourStart + 3'600'000) {
        if (auto bar = minutes.builder.bar(slot.start)) {
            parts.push_back(std::move(*bar));
            revision += *minutes.builder.revisionAt(slot.start);
        }
    }
    const bool closed = product.clock >= hourStart + 3'600'000;
    const bool complete = product.firstLiveMs <= hourStart &&
        !flowLossIn(product.lossyMinutes, hourStart, hourStart + 3'600'000);
    auto existing = day.hours.find(hourStart);
    FlowComposite* prior = existing != day.hours.end() ? &existing->second : nullptr;
    if (!prior && day.formingHour && day.formingHour->first == hourStart)
        prior = &day.formingHour->second;
    if (parts.empty() && !prior) return;
    std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    if (prior && (parts.size() < prior->sourcePartCount ||
                  (!parts.empty() && parts.front().startMs > prior->firstPartStart))) {
        ++m_flowHourShrinkRefusals;
        if (closed && day.formingHour && day.formingHour->first == hourStart) {
            day.hours[hourStart] = std::move(day.formingHour->second);
            day.formingHour.reset();
            prior = &day.hours.at(hourStart);
        }
        if (prior->complete != complete || prior->closed != closed) prior->dirty = true;
        prior->complete = complete;
        prior->closed = closed;
        return;
    }
    if (parts.empty()) return;
    FlowComposite* hour = nullptr;
    if (closed) {
        hour = &day.hours[hourStart];
    } else {
        if (!day.formingHour || day.formingHour->first != hourStart)
            day.formingHour.emplace(hourStart, FlowComposite{});
        hour = &day.formingHour->second;
    }
    hour->bar = sentinel::flow::composeFlowBars(parts, 3'600'000);
    hour->complete = complete; hour->revision = revision;
    hour->closed = closed; hour->dirty = true;
    hour->sourcePartCount = parts.size();
    hour->firstPartStart = parts.front().startMs;
    if (closed && day.formingHour && day.formingHour->first == hourStart)
        day.formingHour.reset();
}

void ServerDataModel::updateCoarse(FlowDay& day, const FlowProduct& product,
                                   int64_t tf, int64_t bucketStart) {
    std::vector<sentinel::flow::FlowBar> parts;
    uint32_t revision = 0;
    bool complete = product.firstLiveMs <= bucketStart &&
        !flowLossIn(product.lossyMinutes, bucketStart, bucketStart + tf);
    if (tf < 3'600'000) {
        auto& minutes = day.series.at(60'000);
        for (const auto& slot : minutes.bucketSlots) if (slot.start >= bucketStart && slot.start < bucketStart + tf) {
            if (auto bar = minutes.builder.bar(slot.start)) {
                revision += *minutes.builder.revisionAt(slot.start);
                parts.push_back(std::move(*bar));
            }
        }
    } else {
        for (const auto& [start, hour] : day.hours) if (start >= bucketStart && start < bucketStart + tf) {
            parts.push_back(hour.bar); revision += hour.revision; complete &= hour.complete;
        }
        if (day.formingHour && day.formingHour->first >= bucketStart &&
            day.formingHour->first < bucketStart + tf) {
            parts.push_back(day.formingHour->second.bar);
            revision += day.formingHour->second.revision;
            complete &= day.formingHour->second.complete;
        }
    }
    const auto key = std::pair{tf, bucketStart};
    auto existing = day.coarse.find(key);
    if (parts.empty() && existing == day.coarse.end()) return;
    std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    if (existing != day.coarse.end() &&
        (parts.size() < existing->second.sourcePartCount ||
         (!parts.empty() && parts.front().startMs > existing->second.firstPartStart))) {
        if (existing->second.complete != complete) {
            existing->second.complete = complete;
            existing->second.dirty = true;
        }
        return;
    }
    if (parts.empty()) return;
    auto bar = sentinel::flow::composeFlowBars(parts, tf);
    auto& state = day.coarse[key];
    if (state.bar != bar || state.complete != complete || state.revision != revision) {
        state.bar = std::move(bar); state.complete = complete;
        state.revision = revision; state.dirty = true;
    }
    state.sourcePartCount = parts.size();
    state.firstPartStart = parts.front().startMs;
}

void ServerDataModel::publishCoarse(const std::string& symbol, FlowProduct& product, FlowDay& day) {
    std::map<int64_t, int64_t> newest;
    for (const auto& [key, state] : day.coarse)
        newest[key.first] = std::max(newest[key.first], key.second);
    for (auto it = day.coarse.begin(); it != day.coarse.end();) {
        const auto [tf, start] = it->first;
        const bool subscribed = std::any_of(m_flowSubscriptions.begin(), m_flowSubscriptions.end(),
            [&](const auto& sub) { return sub->active.load() && sub->symbol == symbol && sub->timeframeMs == tf; });
        const bool expired = start < (product.clock - kFlowRolloverGraceMs) / tf * tf;
        if (!subscribed || (expired && it->second.closed && !it->second.dirty)) {
            it = day.coarse.erase(it); continue;
        }
        auto& state = it->second;
        if (start > product.receiveMax) { ++it; continue; }
        const bool closed = product.clock >= start + tf;
        const bool changed = state.dirty || (closed && !state.closed);
        const bool initial = std::any_of(m_flowSubscriptions.begin(), m_flowSubscriptions.end(),
            [&](const auto& sub) { return sub->active.load() && sub->initial &&
                sub->symbol == symbol && sub->timeframeMs == tf; });
        if (changed || (initial && start == newest[tf]))
            publishFlowBar(symbol, tf, state.bar, state.revision, closed,
                           state.complete, !changed);
        state.dirty = false; state.closed = closed;
        ++it;
    }
}

void ServerDataModel::publishFlow() {
    std::erase_if(m_flowSubscriptions, [](const auto& s) { return !s->active.load(); });
    for (auto& [symbol, product] : m_flowProducts) {
        expireGridTrades(symbol, product);
        if (const auto state = m_flowReceiveStates.find(symbol); state != m_flowReceiveStates.end()) {
            const auto receive = state->second.maximum.load(std::memory_order_acquire);
            if (state->second.pendingTrades.load(std::memory_order_acquire) == 0 &&
                receive > product.receiveMax) {
                product.receiveMax = receive;
                product.clock = std::max(product.clock, receive - kFlowCloseDelayMs);
                updateFlowRetentionFloor(symbol, receive);
            }
        }
        if (m_flowStore) {
            const auto sealedThrough = m_flowStore->sealedThroughMs(symbol);
            const auto lag = sentinel::flow::storeLagMs(product.clock, sealedThrough,
                                                        product.firstLiveMs, kFlowRolloverGraceMs);
            if (lag && *lag > 6 * 3'600'000 &&
                (product.lastLagWarningClock == INT64_MIN || product.clock - product.lastLagWarningClock >= 60'000)) {
                sLog_Warning("Flow store lag exceeds live retention symbol=" << symbol << " lagMs=" << *lag);
                product.lastLagWarningClock = product.clock;
            }
        }
        for (auto& [dayStart, day] : product.days) {
            auto& minute = day.series.at(60'000);
            std::vector<int64_t> changed;
            for (const auto& slot : minute.bucketSlots)
                if (slot.start >= 0 && (slot.dirty || (!slot.closed && product.clock >= slot.start + 60'000)))
                    changed.push_back(slot.start);
            for (const auto start : product.newLossyMinutes)
                if (start >= dayStart && start < dayStart + 86'400'000) changed.push_back(start);
            std::sort(changed.begin(), changed.end());
            changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
            std::vector<int64_t> closedHours;
            for (const auto& [start, hour] : day.hours)
                if (!hour.closed && product.clock >= start + 3'600'000) closedHours.push_back(start);
            if (day.formingHour && product.clock >= day.formingHour->first + 3'600'000)
                closedHours.push_back(day.formingHour->first);
            std::set<int64_t> hoursToUpdate(closedHours.begin(), closedHours.end());
            for (const auto start : changed) hoursToUpdate.insert(start / 3'600'000 * 3'600'000);
            for (const auto start : hoursToUpdate)
                updateHour(day, product, start);
            std::set<std::pair<int64_t, int64_t>> coarseUpdates;
            for (const auto& sub : m_flowSubscriptions) {
                if (!sub->active.load() || sub->symbol != symbol || sub->timeframeMs <= 60'000) continue;
                const auto tf = sub->timeframeMs;
                for (const auto start : changed) coarseUpdates.emplace(tf, start / tf * tf);
                if (sub->initial && !minute.bucketSlots.empty()) {
                    int64_t latest = -1;
                    for (const auto& slot : minute.bucketSlots) latest = std::max(latest, slot.start);
                    if (latest >= 0) coarseUpdates.emplace(tf, latest / tf * tf);
                }
                for (const auto start : closedHours) coarseUpdates.emplace(tf, start / tf * tf);
            }
            for (const auto& [key, state] : day.coarse)
                if (!state.closed && product.clock >= key.second + key.first)
                    coarseUpdates.insert(key);
            for (const auto& [tf, start] : coarseUpdates) updateCoarse(day, product, tf, start);
            publishFlowSeries(symbol, product, 1000, day.series.at(1000));
            publishFlowSeries(symbol, product, 60'000, minute);
            publishCoarse(symbol, product, day);
            if (dayStart + 86'400'000 <= flowMinuteRetainedFrom(symbol, product.receiveMax))
                day.hours.clear();
        }
        product.days.erase(product.days.begin(), product.days.lower_bound(
            std::max<int64_t>(0, std::min(product.clock - kFlowSecondRetentionMs,
                                          flowMinuteRetainedFrom(symbol, product.receiveMax))) /
                86'400'000 * 86'400'000));
        product.newLossyMinutes.clear();
        const auto keepFrom = std::max<int64_t>(0, flowMinuteRetainedFrom(symbol, product.receiveMax)) /
            86'400'000 * 86'400'000;
        product.lossyMinutes.erase(product.lossyMinutes.begin(), product.lossyMinutes.lower_bound(keepFrom));
    }
    for (const auto& sub : m_flowSubscriptions) sub->initial = false;
}
