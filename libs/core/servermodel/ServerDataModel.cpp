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
    if (!m_aggregator) return;
    // Receive time leads exchange time by the feed latency: close only buckets
    // that ended kJournalCandleLagMs before it, so in-flight trades still land.
    m_aggregator->advance(symbol, journalMs - kJournalCandleLagMs);
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
    size_t bars100 = 0, bars1000 = 0;
    int64_t installUs = 0;
    if (restore && m_aggregator) {
        const auto began = std::chrono::steady_clock::now();
        bool empty = true;
        for (const auto& [tf, series] : restore->series)
            empty = empty && m_aggregator->canRestoreSeries(symbol, tf);
        if (empty) {
            restored = restore->outcome == sentinel::roller::RestoreOutcome::complete ? "complete" : "partial";
            for (auto& [tf, series] : restore->series) {
                (tf == 100 ? bars100 : bars1000) += series.closed.size();
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
             << " bars1000=" << bars1000 << " through=" << through.run << "/" << through.block << "/"
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
               "1 while this product's startup 100 ms / 1 s candle rebuild has not ended (history requests "
               "reply warming).",
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
                                          bool* budgetExceeded) const {
    OverlayTradeSelection selection(startMs, endMs, limit, out, budget);
    if (retainedFromMs) *retainedFromMs = 0;
    if (budgetExceeded) *budgetExceeded = false;
    if (endMs <= startMs) return true;
    {
        std::lock_guard<std::mutex> lock(m_footprintTradeMutex);
        const auto it = m_recentFootprintTrades.find(symbol);
        if (it == m_recentFootprintTrades.end()) return true;
        for (const auto& trade : it->second) {
            if (!selection.add(trade)) {
                if (retainedFromMs) *retainedFromMs = selection.retainedFromMs();
                return false;
            }
        }
    }
    selection.finish();
    if (retainedFromMs) *retainedFromMs = selection.retainedFromMs();
    if (budgetExceeded) *budgetExceeded = selection.budgetExceeded();
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
