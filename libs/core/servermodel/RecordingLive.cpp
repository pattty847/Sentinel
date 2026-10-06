#include "RecordingLive.hpp"
#include "SentinelLogging.hpp"
#include <condition_variable>
#include <thread>
#include <set>
#include <tuple>

namespace recording {
void LiveCache::releaseSymbol(const std::string &symbol) {
    std::lock_guard lock(mutex_);
    std::erase_if(series_, [&](const auto &entry) {
        if (entry.first.first != symbol) return false;
        revisionFloor_ = std::max(revisionFloor_, entry.second.revision);
        return true;
    });
}
bool LiveCache::publish(RecordPtr r) {
    if (!r || r->header.tfMs != 60'000 || !r->observedMs) return false;
    std::lock_guard lock(mutex_);
    const auto key = std::pair{r->header.symbol, r->header.layer};
    if (!series_.contains(key) && series_.size() >= kMaxSeries) {
        if (!warnedSeriesLimit_) {
            warnedSeriesLimit_ = true;
            capacityWarning_ = key; // logging belongs to the consumer, never the recorder handoff
        }
        return false;
    }
    const auto [it, inserted] = series_.try_emplace(key);
    auto &s = it->second;
    if (inserted) s.revision = revisionFloor_;
    const auto through = std::max(r->committedThroughMs,
        (r->flags & kProvisional) ? int64_t{0} : r->bucketStartMs + 60'000);
    s.committedThroughMs = std::max(s.committedThroughMs, through);
    std::erase_if(s.provisional, [&](const auto &entry) { return entry.first < s.committedThroughMs; });
    if (r->flags & kProvisional) {
        if (r->bucketStartMs < s.committedThroughMs) return false;
        s.provisional[r->bucketStartMs] = std::move(r);
        s.withdrawn = false;
        // Recorder lateness is <=1h: at most 60 pending minutes plus the open one.
        // A malformed external publisher cannot grow the mailbox indefinitely.
        while (s.provisional.size() > 61) s.provisional.erase(s.provisional.begin());
    } else {
        if (!s.committed.empty() && r->bucketStartMs <= s.committed.back()->bucketStartMs) return false;
        s.committed.push_back(std::move(r));
        size_t entries = 0;
        for (const auto &record : s.committed) entries += record->entries.size();
        while (s.committed.size() > 1 && (s.committed.size() > kMaxRecords || entries > kMaxEntries)) {
            entries -= s.committed.front()->entries.size();
            s.committed.pop_front();
        }
    }
    ++s.revision;
    return true;
}
void LiveCache::retractProvisional(const std::string &symbol) {
    std::lock_guard lock(mutex_);
    for (auto it = series_.lower_bound({symbol, std::string()}); it != series_.end() && it->first.first == symbol; ++it) {
        if (it->second.provisional.empty()) continue;
        it->second.provisional.clear();
        it->second.withdrawn = true;
        ++it->second.revision;
    }
}
bool LiveCache::ensureFinal(const std::string &symbol, const std::string &layer, RecordPtr final) {
    std::lock_guard lock(mutex_);
    const auto key = std::pair{symbol, layer};
    auto it = series_.find(key);
    if (it != series_.end() && !it->second.committed.empty()) return true;
    if (!final || final->header.symbol != symbol || final->header.layer != layer || final->header.tfMs != 60'000 ||
        !final->observedMs || (final->flags & kProvisional)) return false;
    if (it == series_.end()) {
        if (series_.size() >= kMaxSeries) return false;
        it = series_.try_emplace(key).first;
        it->second.revision = revisionFloor_;
    }
    auto &s = it->second;
    s.committedThroughMs = std::max(s.committedThroughMs, final->bucketStartMs + 60'000);
    std::erase_if(s.provisional, [&](const auto &entry) { return entry.first < s.committedThroughMs; });
    s.committed.push_back(std::move(final));
    ++s.revision;
    return true;
}
LiveCache::Snapshot LiveCache::snapshot(const std::string &symbol, const std::string &layer) const {
    std::lock_guard lock(mutex_);
    const auto it = series_.find({symbol, layer});
    return it == series_.end() ? Snapshot{} : it->second;
}
std::optional<std::pair<std::string, std::string>> LiveCache::takeCapacityWarning() {
    std::lock_guard lock(mutex_);
    return std::exchange(capacityWarning_, std::nullopt);
}
namespace {
void fillRawColumn(heatmap::SparseColumn& column, const Hmc2Record& record) {
    column.bucketStartMs = record.bucketStartMs;
    column.observedMs = record.observedMs;
    column.flags = record.flags;
    column.native.resize(1);
    auto& native = column.native.front();
    native.grid = {record.header.configHash, record.header.rowTickUnits, record.header.priceScale};
    native.sizeScale = record.header.sizeScale;
    native.observedMs = record.observedMs;
    native.entries.clear();
    native.baseRow = std::numeric_limits<int64_t>::max();
    for (int side = 0; side < 2; ++side) {
        auto& runs = native.coverage[side];
        runs.clear();
        const auto lo = side ? record.askRowLo : record.bidRowLo;
        const auto hi = side ? record.askRowHi : record.bidRowHi;
        if (lo <= hi) {
            runs.push_back({lo, hi, record.observedMs});
            native.baseRow = std::min(native.baseRow, lo);
        }
    }
    // Match fromRecording even when entries outside minute bounds are filtered.
    for (const auto& entry : record.entries) native.baseRow = std::min(native.baseRow, entry.row);
    if (native.baseRow == std::numeric_limits<int64_t>::max()) native.baseRow = 0;
    for (const auto& entry : record.entries) {
        if (entry.row < (entry.isAsk ? record.askRowLo : record.bidRowLo) ||
            entry.row > (entry.isAsk ? record.askRowHi : record.bidRowHi)) continue;
        native.entries.push_back({heatmap::packRowSide(entry.row, native.baseRow, entry.isAsk),
                                  uint16_t(entry.twapCode & kMaxCode)});
    }
    // Open publications originate in a pmr::unordered_map. Packed rowSide itself
    // orders by side first, whereas SHC1 requires (row, side).
    std::sort(native.entries.begin(), native.entries.end(), [](const auto& a, const auto& b) {
        return std::pair(a.row(), a.isAsk()) < std::pair(b.row(), b.isAsk());
    });
}
}
RawTailFrame RawTailBuilder::build(const std::string& symbol, const std::string& source,
                                  const LiveCache::Snapshot& snapshot, int64_t sinceMs) {
    using namespace heatmap;
    const auto* definition = findChunkSource(source);
    if (!definition || sinceMs < 0) throw std::invalid_argument("invalid raw-tail source/cutoff");
    if (snapshot.revision != revision_ || symbol != symbol_ || source != source_) {
        variants_.clear();
        revision_ = snapshot.revision;
        symbol_ = symbol; source_ = source;
    }
    if (!snapshot.revision || (snapshot.provisional.empty() && snapshot.committed.empty())) return {};
    // This is the latest known minute, not a wall-clock assertion that it is
    // forming: at rollover it can be a complete, lateness-held provisional.
    // Preserve the recorder's flags until the next publishOpen arrives.
    int64_t open = snapshot.committedThroughMs;
    if (!snapshot.provisional.empty()) open = std::max(open, snapshot.provisional.rbegin()->first);
    if (open < kHmc2MinMs || open > kHmc2EndMs - kMinuteMs || open % kMinuteMs)
        throw std::invalid_argument("invalid raw-tail open minute");
    const auto chunkStart = open / kHourMs * kHourMs;
    const auto earliest = std::max(kHmc2MinMs, chunkStart - kHourMs);
    auto first = std::find_if(snapshot.committed.begin(), snapshot.committed.end(), [&](const auto& r) {
        return r->bucketStartMs >= std::max(sinceMs, earliest);
    });
    // Provisional minutes withdrawn and every final already delivered: resend
    // the newest final, so a frame still reaches the client and it drops the
    // omitted provisional minutes (a LiveColumn frame carries >= 1 column).
    // Without a withdrawal this is empty work, as before.
    if (first == snapshot.committed.end() && snapshot.withdrawn && !snapshot.committed.empty())
        first = std::prev(snapshot.committed.end());
    const auto variant = first == snapshot.committed.end() ? int64_t{0} : (*first)->bucketStartMs;
    if (const auto it = variants_.find(variant); it != variants_.end()) return it->second;
    records_.clear();
    int64_t finalThrough = 0;
    for (auto it = first; it != snapshot.committed.end(); ++it) {
        const auto& r = *it;
        if (r->bucketStartMs < kHmc2MinMs || r->bucketStartMs > open - kMinuteMs)
            throw std::invalid_argument("invalid raw-tail final minute");
        records_.push_back(r);
        finalThrough = r->bucketStartMs + kMinuteMs;
    }
    for (const auto& [start, r] : snapshot.provisional) {
        if (start < earliest || start != r->bucketStartMs || start > open)
            throw std::invalid_argument("invalid raw-tail provisional extent");
        records_.push_back(r);
    }
    if (records_.empty()) return {};
    frame_.kind = ChunkKind::LiveColumn;
    frame_.key = {symbol, source, kMinuteMs, chunkStart};
    frame_.state = {false, snapshot.committedThroughMs, snapshot.revision};
    auto& columns = frame_.columns;
    columns.symbol = symbol; columns.layer = definition->hmc2Layer;
    columns.tfMs = kMinuteMs; columns.startMs = records_.front()->bucketStartMs;
    columns.endMs = open + kMinuteMs;
    columns.columns.resize(records_.size());
    filledRecords_.resize(records_.size());
    columns.scannedRanges.clear();
    for (size_t i = 0; i < records_.size(); ++i) {
        const auto& r = *records_[i];
        if (r.header.symbol != symbol || r.header.layer != definition->hmc2Layer || r.header.tfMs != kMinuteMs ||
            r.bucketStartMs < kHmc2MinMs || r.bucketStartMs > open)
            throw std::invalid_argument("invalid raw-tail record identity");
        // Frozen pending minutes are immutable. Preserve their sorted storage
        // across open updates; only a changed record refills it.
        if (filledRecords_[i] != records_[i]) {
            fillRawColumn(columns.columns[i], r);
            filledRecords_[i] = records_[i];
        }
        const auto end = r.bucketStartMs + kMinuteMs;
        if (!columns.scannedRanges.empty() && columns.scannedRanges.back().endMs == r.bucketStartMs)
            columns.scannedRanges.back().endMs = end;
        else columns.scannedRanges.push_back({r.bucketStartMs, end});
    }
    auto rebuildRanges = [&] {
        columns.startMs = columns.columns.front().bucketStartMs;
        columns.scannedRanges.clear();
        finalThrough = 0;
        for (const auto& col : columns.columns) {
            const auto end = col.bucketStartMs + kMinuteMs;
            if (!columns.scannedRanges.empty() && columns.scannedRanges.back().endMs == col.bucketStartMs)
                columns.scannedRanges.back().endMs = end;
            else columns.scannedRanges.push_back({col.bucketStartMs, end});
            if (!(col.flags & kProvisional)) finalThrough = end;
        }
    };
    auto dropOldestPending = [&] {
        // Preserve the newest known minute. Finals are removed newest-first:
        // only an included final prefix can advance the subscriber's marker.
        auto drop = std::find_if(columns.columns.begin(), std::prev(columns.columns.end()),
            [](const auto& col) { return col.flags & kProvisional; });
        if (drop == std::prev(columns.columns.end())) {
            auto final = std::find_if(columns.columns.rbegin(), columns.columns.rend(),
                [](const auto& col) { return !(col.flags & kProvisional); });
            if (final == columns.columns.rend()) return false;
            drop = std::prev(final.base());
        }
        columns.columns.erase(drop);
        filledRecords_.clear(); // column positions no longer match the full snapshot
        if (columns.columns.empty()) return false;
        rebuildRanges();
        return true;
    };
    std::vector<uint8_t> bytes;
    for (;;) {
        // Conservative native-minute payload bound (seven bytes/entry plus
        // metadata). Trim before exceeding SHC1's 16 MiB raw allocation limit.
        size_t bound = 2;
        for (const auto& col : columns.columns) bound += 152 + 7 * col.native.front().entries.size();
        if (bound <= 16 * 1024 * 1024) bytes = encodeChunk(frame_, scratch_);
        if (bound <= 16 * 1024 * 1024 && bytes.size() <= kRawLiveByteBudget - 14) break;
        const auto now = sentinel::log_throttle::nowMs();
        if (now >= nextOversizeWarningMs_) {
            nextOversizeWarningMs_ = now + 5000;
            sLog_Warning("Raw heatmap live tail exceeds byte budget: symbol=" << symbol
                << " source=" << source << " bytes=" << (bytes.empty() ? bound : bytes.size() + 14)
                << " limit=" << kRawLiveByteBudget << " trimming oldest pending minutes");
        }
        bytes.clear();
        if (!dropOldestPending()) {
            // One indivisible minute can itself exceed the budget. It stays
            // unscanned here and must arrive through chunks once committed.
            variants_.emplace(variant, RawTailFrame{});
            return {};
        }
    }
    RawTailFrame result{std::make_shared<const std::vector<uint8_t>>(std::move(bytes)), snapshot.revision, finalThrough};
    ++encodings_;
    variants_.emplace(variant, result);
    return result;
}
struct LiveService::Impl {
    std::filesystem::path root;
    const int64_t cadenceMs; // LiveCadence base; set before the worker starts
    LiveCache cache;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::vector<std::weak_ptr<Subscription>> subscriptions;
    std::vector<std::weak_ptr<RawSubscription>> rawSubscriptions;
    std::function<int64_t()> clock = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
    uint64_t requestedTurn = 0, completedTurn = 0;
    std::condition_variable completed;
    std::function<void(const char*)> rawWorkHook;
    std::mutex shutdownMutex;
    std::atomic<uint64_t> builds{0}, buildMicros{0}, deliveries{0}, deliveryMicros{0};
    std::atomic<uint64_t> rawEncodings{0}, rawBuildMicros{0}, rawDeliveries{0}, rawBuilds{0}, rawFailures{0};
    std::thread worker;
    Impl(std::filesystem::path p, int64_t cadence)
        : root(std::move(p)), cadenceMs(std::clamp<int64_t>(cadence, 1, kLiveCadenceMaxMs)), worker([this] { run(); }) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        std::lock_guard joinLock(shutdownMutex);
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (const auto &w : subscriptions) if (auto s = w.lock()) s->active.store(false);
            for (const auto &w : rawSubscriptions) if (auto s = w.lock()) s->active.store(false);
        }
        wake.notify_one();
        completed.notify_all();
        if (worker.joinable()) worker.join();
    }
    void start() {
        std::lock_guard joinLock(shutdownMutex);
        std::lock_guard lock(mutex);
        if (!stopping) return;
        subscriptions.clear();
        rawSubscriptions.clear();
        worker = std::thread([this] { run(); });
        stopping = false;
    }
    void run() {
        sentinel::logging::setCurrentThreadName("recording-live");
        Hmc2Reader reader(root);
        using Key = std::tuple<std::string, std::string, int64_t, double, double, uint32_t>;
        auto keyOf = [](const LiveView &v) { return Key{v.symbol, v.layer, v.tfMs, v.band.lo, v.band.tick, v.band.rows}; };
        struct State {
            std::weak_ptr<Subscription> subscription;
            LiveCadence cadence;
            uint64_t deliveredRevision = 0;
            int64_t deliveredFinalMs = 0;
        };
        struct Group {
            std::unique_ptr<LiveBuilder> builder;
            BuildResult page;
            uint64_t revision = 0;
            int64_t omittedFinalThroughMs = 0;
            LiveCadence cadence;
        };
        std::map<const Subscription *, State> states;
        std::map<Key, Group> groups;
        auto nowMs = [this] { return clock(); };
        struct RawState {
            std::weak_ptr<RawSubscription> subscription;
            LiveCadence cadence;
            uint64_t deliveredRevision = 0;
            int64_t finalThroughMs = 0;
        };
        using Series = std::pair<std::string, std::string>;
        std::map<std::pair<const RawSubscription*, std::string>, RawState> rawStates;
        std::map<Series, RawTailBuilder> rawBuilders;
        size_t nextRaw = 0;
        LiveCadence rawFailureCadence{cadenceMs};
        std::vector<std::pair<std::shared_ptr<RawSubscription>, std::string>> rawJobs;
        auto micros = [](auto start) { return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count(); };
        for (;;) {
            std::vector<std::shared_ptr<Subscription>> views;
            std::vector<std::shared_ptr<RawSubscription>> rawViews;
            uint64_t turn = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping || requestedTurn > completedTurn; });
                if (stopping) break;
                std::erase_if(subscriptions, [](const auto &w) {
                    auto s = w.lock(); return !s || !s->active.load();
                });
                for (const auto &w : subscriptions) if (auto s = w.lock()) views.push_back(std::move(s));
                turn = requestedTurn;
            }
            if (const auto warning = cache.takeCapacityWarning())
                sLog_Warning("Recording live series capacity reached: limit=" << LiveCache::kMaxSeries
                    << " firstDroppedSymbol=" << warning->first << " layer=" << warning->second);
            std::erase_if(states, [](const auto &item) {
                auto s = item.second.subscription.lock(); return !s || !s->active.load();
            });
            std::set<Key> activeKeys;
            for (const auto &s : views) activeKeys.insert(keyOf(s->view));
            std::erase_if(groups, [&](const auto &item) { return !activeKeys.contains(item.first); });
            for (const auto &s : views) {
                if (!s->active.load()) continue;
                auto [it, added] = states.try_emplace(s.get());
                auto &state = it->second;
                if (added) { state.subscription = s; state.cadence = {cadenceMs}; }
                if (!state.cadence.due(nowMs())) continue;
                auto &group = groups[keyOf(s->view)];
                if (!group.builder) { group.builder = std::make_unique<LiveBuilder>(s->view); group.cadence = {cadenceMs}; }
                const auto source = cache.snapshot(s->view.symbol, s->view.layer);
                if (!source.revision || source.revision == state.deliveredRevision) continue;
                bool accepted = false;
                try {
                    const bool needsFinal = state.deliveredFinalMs < group.omittedFinalThroughMs;
                    if (needsFinal && !group.cadence.due(nowMs())) continue;
                    if (group.cadence.due(nowMs()) &&
                        (group.revision != source.revision || group.page.status != BuildStatus::Complete || needsFinal)) {
                        auto deliveredFinal = state.deliveredFinalMs;
                        for (const auto& viewer : views) {
                            if (!viewer->active.load() || keyOf(viewer->view) != keyOf(s->view)) continue;
                            const auto prior = states.find(viewer.get());
                            deliveredFinal = std::min(deliveredFinal, prior == states.end()
                                ? int64_t{0} : prior->second.deliveredFinalMs);
                        }
                        const auto start = std::chrono::steady_clock::now();
                        group.page = group.builder->build(reader, source, deliveredFinal);
                        group.omittedFinalThroughMs = deliveredFinal;
                        if (group.page.status == BuildStatus::IoError)
                            sLog_Warning("Recording live read retry: symbol=" << s->view.symbol
                                << " tf=" << s->view.tfMs << " message=" << group.page.message);
                        ++builds;
                        buildMicros += micros(start);
                        group.revision = source.revision;
                        group.cadence.completed(nowMs(), group.page.status == BuildStatus::Complete);
                    }
                    if (!group.revision || state.deliveredRevision == group.revision) continue;
                    if (group.page.status == BuildStatus::IoError) {
                        // Transient storage failure: keep the builder's proven
                        // prefix, retry even if publication revision is unchanged.
                        // Sending a view error would make the client re-register.
                        state.cadence.completed(nowMs(), false);
                        continue;
                    }
                    BuildResult filtered;
                    const bool hasDeliveredFinal = std::any_of(group.page.columns.begin(), group.page.columns.end(),
                        [&](const auto& c) { return !(c.flags & kProvisional) && c.bucketStartMs <= state.deliveredFinalMs; });
                    if (hasDeliveredFinal) {
                        filtered = group.page; // display rows only, never the native aggregate
                        std::erase_if(filtered.columns, [&](const auto& c) {
                            return !(c.flags & kProvisional) && c.bucketStartMs <= state.deliveredFinalMs;
                        });
                    }
                    const auto &page = hasDeliveredFinal ? filtered : group.page;
                    // Errors are delivered through the same bounded transport path,
                    // carrying the individual subscriber's generation.
                    if (page.status != BuildStatus::Budget && page.status != BuildStatus::Cancelled && s->active.load()) {
                        const auto start = std::chrono::steady_clock::now();
                        accepted = s->deliver(s->view, page);
                        deliveryMicros += micros(start);
                        ++deliveries;
                    }
                    if (accepted) {
                        state.deliveredRevision = group.revision;
                        for (const auto& column : page.columns)
                            if (!(column.flags & kProvisional))
                                state.deliveredFinalMs = std::max(state.deliveredFinalMs, column.bucketStartMs);
                    }
                    if (accepted && page.status != BuildStatus::Complete) s->active.store(false);
                    sLog_Probe("recording.live.send", "symbol=" << s->view.symbol << " tf=" << s->view.tfMs
                        << " gen=" << s->view.generation << " columns=" << page.columns.size()
                        << " accepted=" << accepted << " diskRecords=" << page.sourceRecords);
                } catch (const std::exception &e) {
                    sLog_Error("Recording live failed: symbol=" << s->view.symbol << " error=" << e.what());
                }
                state.cadence.completed(nowMs(), accepted);
            }
            try {
                if (rawFailureCadence.due(nowMs())) {
                    if (rawWorkHook) rawWorkHook("collect");
                    {
                        std::lock_guard lock(mutex);
                        std::erase_if(rawSubscriptions, [](const auto& w) { auto s = w.lock(); return !s || !s->active.load(); });
                        for (const auto& w : rawSubscriptions) if (auto s = w.lock()) rawViews.push_back(std::move(s));
                    }
                    std::erase_if(rawStates, [](const auto& item) {
                        auto s = item.second.subscription.lock(); return !s || !s->active.load();
                    });
                    std::set<Series> activeSeries;
                    for (const auto& s : rawViews) for (const auto& source : s->view.sources)
                        activeSeries.emplace(s->view.symbol, source);
                    std::erase_if(rawBuilders, [&](const auto& item) { return !activeSeries.contains(item.first); });
                    // One snapshot per series per worker turn; no disk reads in this path.
                    std::map<Series, LiveCache::Snapshot> snapshots;
                    rawJobs.clear();
                    for (const auto& s : rawViews) for (const auto& source : s->view.sources) rawJobs.emplace_back(s, source);
                    const auto firstRaw = rawJobs.empty() ? 0 : nextRaw % rawJobs.size();
                    for (size_t job = 0; job < rawJobs.size(); ++job) {
                        const auto index = (firstRaw + job) % rawJobs.size();
                        const auto& [s, sourceId] = rawJobs[index];
                        if (!s->active.load()) continue;
                        if (rawWorkHook) rawWorkHook("state");
                        auto [it, added] = rawStates.try_emplace({s.get(), sourceId});
                        auto& state = it->second;
                        if (added) { state.subscription = s; state.cadence = {cadenceMs}; state.finalThroughMs = s->view.sinceMs; }
                        if (!state.cadence.due(nowMs())) continue;
                        const Series series{s->view.symbol, sourceId};
                        auto [sourceIt, fresh] = snapshots.try_emplace(series);
                        if (fresh) {
                            if (rawWorkHook) rawWorkHook("snapshot");
                            sourceIt->second = cache.snapshot(series.first, std::string(heatmap::findChunkSource(sourceId)->hmc2Layer));
                        }
                        const auto& snapshot = sourceIt->second;
                        if (!snapshot.revision || snapshot.revision == state.deliveredRevision) continue;
                        bool accepted = false;
                        try {
                            auto& builder = rawBuilders[series];
                            const auto before = builder.encodings();
                            const auto start = std::chrono::steady_clock::now();
                            ++rawBuilds;
                            const auto frame = builder.build(series.first, sourceId, snapshot, state.finalThroughMs);
                            rawEncodings += builder.encodings() - before;
                            rawBuildMicros += micros(start);
                            if (!frame.bytes) {
                                state.deliveredRevision = snapshot.revision;
                                continue; // empty work is consumed, not transport congestion
                            }
                            if (s->active.load()) {
                                accepted = s->deliver(s->view, sourceId, frame);
                                ++rawDeliveries;
                            }
                            if (accepted) {
                                state.deliveredRevision = frame.revision;
                                state.finalThroughMs = std::max(state.finalThroughMs, frame.finalThroughMs);
                                // On budget pressure, start after the last
                                // accepted source next turn so a hot symbol cannot starve peers.
                                nextRaw = index + 1;
                            }
                            sLog_Probe("chunks.live.send", "symbol=" << series.first << " source=" << sourceId
                                << " sub=" << s->view.sub << " revision=" << snapshot.revision << " accepted=" << accepted
                                << " finalThrough=" << state.finalThroughMs << " delayMs=" << state.cadence.delayMs);
                        } catch (const std::exception& e) {
                            ++rawFailures;
                            sLog_Error("Raw heatmap live failed: symbol=" << series.first << " source=" << sourceId << " error=" << e.what());
                        }
                        state.cadence.completed(nowMs(), accepted);
                    }
                    rawFailureCadence = {cadenceMs};
                }
            } catch (const std::exception& e) {
                ++rawFailures;
                rawFailureCadence.completed(nowMs(), false);
                sLog_Error("Raw heatmap live worker retry: error=" << e.what()
                    << " delayMs=" << rawFailureCadence.delayMs);
            }
            rawJobs.clear(); // reuse capacity, but never keep weak subscriptions alive between turns
            {
                std::lock_guard lock(mutex);
                completedTurn = turn;
            }
            completed.notify_all();
        }
    }
};
LiveService::LiveService(std::filesystem::path root, int64_t cadenceMs)
    : impl_(std::make_unique<Impl>(std::move(root), cadenceMs)) {}
LiveService::~LiveService() = default;
void LiveService::shutdown() { impl_->shutdown(); }
void LiveService::start() { impl_->start(); }
LiveService::Diagnostics LiveService::diagnostics() const {
    return {impl_->builds.load(), impl_->buildMicros.load(), impl_->deliveries.load(), impl_->deliveryMicros.load(),
            impl_->rawEncodings.load(), impl_->rawBuildMicros.load(), impl_->rawDeliveries.load(),
            impl_->rawBuilds.load(), impl_->rawFailures.load()};
}
void LiveService::retractProvisional(const std::string &symbol) { impl_->cache.retractProvisional(symbol); }
bool LiveService::ensureFinal(const std::string &symbol, const std::string &layer, RecordPtr final) {
    return impl_->cache.ensureFinal(symbol, layer, std::move(final));
}
void LiveService::releaseSymbol(const std::string &symbol) {
    // Called in recorder order. Subscription ownership stays with Session:
    // a newly acquired view can already exist while this old tail is draining.
    impl_->cache.releaseSymbol(symbol);
}
bool LiveService::publish(RecordPtr record) { return impl_->cache.publish(std::move(record)); }
std::shared_ptr<LiveService::Subscription> LiveService::subscribe(LiveView view, Deliver deliver) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->subscriptions, [](const auto &w) { auto s = w.lock(); return !s || !s->active.load(); });
    if (impl_->stopping || impl_->subscriptions.size() >= 64) return {};
    auto subscription = std::make_shared<Subscription>(std::move(view), std::move(deliver));
    impl_->subscriptions.push_back(subscription);
    return subscription;
}
std::shared_ptr<LiveService::RawSubscription> LiveService::subscribeRaw(RawTailView view, RawDeliver deliver) {
    if (view.symbol.empty() || view.symbol.size() > 64 || view.sources.empty() ||
        view.sources.size() > heatmap::kChunkSources.size() || view.sinceMs < 0 ||
        (view.sinceMs && (view.sinceMs < kHmc2MinMs || view.sinceMs >= kHmc2EndMs || view.sinceMs % 60'000))) return {};
    std::set<std::string> unique;
    for (const auto& source : view.sources)
        if (!heatmap::findChunkSource(source) || !unique.insert(source).second) return {};
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->rawSubscriptions, [](const auto& w) { auto s = w.lock(); return !s || !s->active.load(); });
    if (impl_->stopping || impl_->rawSubscriptions.size() >= kMaxRawSubscriptions) return {};
    auto subscription = std::make_shared<RawSubscription>(std::move(view), std::move(deliver));
    impl_->rawSubscriptions.push_back(subscription);
    return subscription;
}
void LiveService::setClockForTest(std::function<int64_t()> clock) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopping) throw std::logic_error("live clock requires stopped service");
    impl_->clock = std::move(clock);
}
void LiveService::setRawWorkHookForTest(std::function<void(const char*)> hook) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopping) throw std::logic_error("live work hook requires stopped service");
    impl_->rawWorkHook = std::move(hook);
}
void LiveService::pollForTest() {
    std::unique_lock lock(impl_->mutex);
    if (impl_->stopping) return;
    const auto turn = ++impl_->requestedTurn;
    impl_->wake.notify_one();
    impl_->completed.wait(lock, [&] { return impl_->stopping || impl_->completedTurn >= turn; });
}
} // namespace recording
