#include "RecordingLive.hpp"
#include "SentinelLogging.hpp"
#include <condition_variable>
#include <thread>
#include <set>
#include <tuple>

namespace recording {
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
    auto &s = series_[key];
    const auto through = std::max(r->committedThroughMs,
        (r->flags & kProvisional) ? int64_t{0} : r->bucketStartMs + 60'000);
    s.committedThroughMs = std::max(s.committedThroughMs, through);
    std::erase_if(s.provisional, [&](const auto &entry) { return entry.first < s.committedThroughMs; });
    if (r->flags & kProvisional) {
        if (r->bucketStartMs < s.committedThroughMs) return false;
        s.provisional[r->bucketStartMs] = std::move(r);
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
LiveCache::Snapshot LiveCache::snapshot(const std::string &symbol, const std::string &layer) const {
    std::lock_guard lock(mutex_);
    const auto it = series_.find({symbol, layer});
    return it == series_.end() ? Snapshot{} : it->second;
}
std::optional<std::pair<std::string, std::string>> LiveCache::takeCapacityWarning() {
    std::lock_guard lock(mutex_);
    return std::exchange(capacityWarning_, std::nullopt);
}
struct LiveService::Impl {
    std::filesystem::path root;
    LiveCache cache;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::vector<std::weak_ptr<Subscription>> subscriptions;
    std::mutex shutdownMutex;
    std::atomic<uint64_t> builds{0}, buildMicros{0}, deliveries{0}, deliveryMicros{0};
    std::thread worker;
    explicit Impl(std::filesystem::path p) : root(std::move(p)), worker([this] { run(); }) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        std::lock_guard joinLock(shutdownMutex);
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (const auto &w : subscriptions) if (auto s = w.lock()) s->active.store(false);
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }
    void start() {
        std::lock_guard joinLock(shutdownMutex);
        std::lock_guard lock(mutex);
        if (!stopping) return;
        subscriptions.clear();
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
        auto nowMs = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(); };
        auto micros = [](auto start) { return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count(); };
        for (;;) {
            std::vector<std::shared_ptr<Subscription>> views;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping; });
                if (stopping) break;
                std::erase_if(subscriptions, [](const auto &w) {
                    auto s = w.lock(); return !s || !s->active.load();
                });
                for (const auto &w : subscriptions) if (auto s = w.lock()) views.push_back(std::move(s));
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
                if (added) state.subscription = s;
                if (!state.cadence.due(nowMs())) continue;
                auto &group = groups[keyOf(s->view)];
                if (!group.builder) group.builder = std::make_unique<LiveBuilder>(s->view);
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
        }
    }
};
LiveService::LiveService(std::filesystem::path root) : impl_(std::make_unique<Impl>(std::move(root))) {}
LiveService::~LiveService() = default;
void LiveService::shutdown() { impl_->shutdown(); }
void LiveService::start() { impl_->start(); }
LiveService::Diagnostics LiveService::diagnostics() const {
    return {impl_->builds.load(), impl_->buildMicros.load(), impl_->deliveries.load(), impl_->deliveryMicros.load()};
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
} // namespace recording
