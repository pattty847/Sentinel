#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include "RecordingDir.hpp"
#include "flow/FlowCodec.hpp"
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
    for (const auto& symbol : rollerProducts(m_serverConfig)) m_feeds.try_emplace(symbol);
    m_reseedTimer.setSingleShot(true);
    connect(&m_reseedTimer, &QTimer::timeout, this, [this] { deliverReseeds(); });
    int64_t maxTfMs = std::max<int64_t>(1000, m_serverConfig.chart.defaultTimeframeMs);
    for (const int64_t tf : m_serverConfig.chart.timeframesMs) {
        if (tf > maxTfMs) {
            maxTfMs = tf;
        }
    }
    const int64_t gridSpanMs = maxTfMs * std::max<int64_t>(1024, m_serverConfig.tradeOverlays.retentionColumns);
    m_footprintTradeRetentionMs = std::clamp<int64_t>(gridSpanMs * 2, 300'000, 86'400'000);

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
        for (auto& [day, state] : it->second.days)
            pruneFlowSeries(it->second.clock, 1000, state.series.at(1000));
    }
    if (!m_aggregator) return;
    // Receive time leads exchange time by the feed latency: close only buckets
    // that ended kJournalCandleLagMs before it, so in-flight trades still land.
    m_aggregator->advance(symbol, journalMs - kJournalCandleLagMs);
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
    r.counterFn("sentinel_flow_missing_grid_trades_total", "Flow trades dropped because the UTC day has no grid.", {},
                [this]() -> Value { return double(m_flowMissingGridTrades); });
    r.counterFn("sentinel_flow_hour_shrink_refusals_total", "Closed flow hour recompositions refused because source minutes were lost.", {},
                [this]() -> Value { return double(m_flowHourShrinkRefusals); });
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

// Worker-only bounded snapshot. False means over budget, never a partial tape.
bool ServerDataModel::collectOverlayTrades(const std::string& symbol, int64_t startMs,
                                          int64_t endMs, size_t limit,
                                          std::vector<FootprintTradeSample>& out,
                                          int64_t* retainedFromMs, OverlayTradeBudget budget,
                                          bool* budgetExceeded, size_t* heapOperations) const {
    OverlayTradeSelection selection(startMs, endMs, limit, out, budget);
    if (retainedFromMs) *retainedFromMs = 0;
    if (budgetExceeded) *budgetExceeded = false;
    if (heapOperations) *heapOperations = 0;
    if (endMs <= startMs) return true;
    {
        std::lock_guard<std::mutex> lock(m_footprintTradeMutex);
        const auto it = m_recentFootprintTrades.find(symbol);
        if (it == m_recentFootprintTrades.end()) return true;
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
    const int64_t exchangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    updateExchangeOffsetMs(exchangeMs);
    addFlowTrade(trade, exchangeMs);

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
      bucketSlots(tf == 1000 ? 1802 : 62) {}

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
        sLog_Data("Flow grid ready: symbol=" << symbol << " day=" << dayStartMs
            << " rowTickUnits=" << scales.rowTickUnits << " priceScale=" << scales.priceScale
            << " baseScale=" << scales.baseScale);
    } catch (const std::exception& e) {
        sLog_Warning("Flow grid refused: symbol=" << symbol << " day=" << dayStartMs
            << " error=" << e.what());
    }
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
    m_flowSubscriptions.push_back(std::move(sub));
}

void ServerDataModel::addFlowTrade(const Trade& trade, int64_t ms) {
    if (ms < 0) return;
    auto& product = m_flowProducts[trade.product_id];
    if (product.firstLiveMs < 0) product.firstLiveMs = ms;
    const int64_t minuteStart = ms / 60'000 * 60'000;
    const int64_t dayStart = ms / 86'400'000 * 86'400'000;
    const auto markLoss = [&] {
        if (product.lossyMinutes.insert(minuteStart).second) {
            product.newLossyMinutes.insert(minuteStart);
            if (const auto day = product.days.find(dayStart); day != product.days.end())
                for (auto& [tf, series] : day->second.series) {
                    auto& slot = series.bucketSlots[(ms / tf) % series.bucketSlots.size()];
                    if (slot.start == ms / tf * tf) slot.dirty = true;
                }
        }
    };
    const auto day = product.days.find(dayStart);
    if (day == product.days.end()) {
        ++m_flowDroppedTrades;
        ++m_flowMissingGridTrades;
        markLoss();
        if (product.missingGridWarnedDays.insert(dayStart).second) {
            sLog_Warning("Flow trade dropped: symbol=" << trade.product_id
                << " day=" << dayStart << " reason=missing_grid");
        }
        return;
    }
    auto& input = product.scratch;
    input.id = trade.trade_id;
    input.exchangeMs = ms; input.price = trade.price; input.size = trade.size;
    input.side = trade.side == AggressorSide::Buy ? sentinel::flow::Side::Buy :
                 trade.side == AggressorSide::Sell ? sentinel::flow::Side::Sell : sentinel::flow::Side::Unknown;
    for (auto& [tf, series] : day->second.series) {
        const auto start = ms / tf * tf;
        auto& slot = series.bucketSlots[(ms / tf) % series.bucketSlots.size()];
        if (slot.start >= 0 && slot.start != start) {
            ++series.capacityDrops; ++m_flowDroppedTrades; markLoss(); continue;
        }
        const auto dropped = series.builder.droppedRejectCount();
        const auto result = series.builder.add(input);
        if (result == sentinel::flow::FlowBarBuilder::AddResult::Accepted) {
            slot.start = start; slot.dirty = true;
            day->second.hasBuckets = true;
        } else if (result == sentinel::flow::FlowBarBuilder::AddResult::Rejected) {
            markLoss();
            if (series.builder.droppedRejectCount() == dropped) ++m_flowDroppedTrades;
        }
        m_flowDroppedTrades += series.builder.droppedRejectCount() - dropped;
    }
}

void ServerDataModel::publishFlowBar(const std::string& symbol, int64_t tf,
                                     const sentinel::flow::FlowBar& bar, uint32_t revision,
                                     bool closed, bool complete, bool initialOnly) {
    std::optional<nlohmann::json> payload;
    for (const auto& sub : m_flowSubscriptions) {
        if (!sub->active.load() || sub->symbol != symbol || sub->timeframeMs != tf ||
            (initialOnly && !sub->initial)) continue;
        if (!payload) {
            const auto bytes = sentinel::flow::encodeFlowBar(bar);
            const auto encoded = QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()).toBase64();
            payload = nlohmann::json{{"type", "flow_bar"},
                {"schema_version", protocol::SentinelProtocol::kFlowSchemaVersion},
                {"symbol", symbol}, {"timeframe_ms", tf}, {"bucket_start_ms", bar.startMs},
                {"closed", closed}, {"complete", complete}, {"revision", revision},
                {"encoding", "FLB1/base64"}, {"bar", encoded.toStdString()},
                {"row_tick_units", std::llround(bar.nativeTick * bar.priceScale)},
                {"price_scale", bar.priceScale}};
        }
        (*payload)["sub"] = sub->sub;
        sub->publish(payload->dump());
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
        const bool closed = slot.start < finalEnd;
        const bool changed = slot.dirty || (closed && !slot.closed);
        if (!changed && !(anyInitial && slot.start == newest)) continue;
        if (const auto bar = series.builder.bar(slot.start))
            publishFlowBar(symbol, tf, *bar, *series.builder.revisionAt(slot.start),
                           closed, product.firstLiveMs <= slot.start &&
                           !flowLossIn(product.lossyMinutes, slot.start, slot.start + tf), !changed);
        slot.dirty = false; slot.closed = closed;
    }
    series.builder.sealThrough(finalEnd);
    pruneFlowSeries(product.clock, tf, series);
}

void ServerDataModel::pruneFlowSeries(int64_t clock, int64_t tf, FlowSeries& series) {
    const auto finalEnd = clock / tf * tf;
    const int64_t drop = tf == 1000 ? finalEnd - kFlowSecondRetentionMs + tf :
        (clock - kFlowRolloverGraceMs) / 3'600'000 * 3'600'000;
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
            if (product.clock >= dayStart + 86'400'000 + kFlowRolloverGraceMs)
                day.hours.clear();
        }
        product.days.erase(product.days.begin(), product.days.lower_bound(
            std::max<int64_t>(0, product.clock - kFlowSecondRetentionMs) / 86'400'000 * 86'400'000));
        product.newLossyMinutes.clear();
        const auto keepFrom = std::max<int64_t>(0, product.clock - kFlowRolloverGraceMs) /
            86'400'000 * 86'400'000;
        product.lossyMinutes.erase(product.lossyMinutes.begin(), product.lossyMinutes.lower_bound(keepFrom));
    }
    for (const auto& sub : m_flowSubscriptions) sub->initial = false;
}
