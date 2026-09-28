#include "RecordingLive.hpp"
#include "SentinelLogging.hpp"
#include <condition_variable>
#include <thread>

namespace recording {
bool LiveCache::publish(RecordPtr r) {
    if (!r || r->header.tfMs != 60'000 || !r->observedMs) return false;
    std::lock_guard lock(mutex_);
    const auto key = std::pair{r->header.symbol, r->header.layer};
    if (!series_.contains(key) && series_.size() >= kMaxSeries) return false;
    auto &s = series_[key];
    if (r->flags & kProvisional) {
        if ((!s.committed.empty() && r->bucketStartMs <= s.committed.back()->bucketStartMs) ||
            (s.provisional && r->bucketStartMs < s.provisional->bucketStartMs)) return false;
        s.provisional = std::move(r);
    } else {
        if (!s.committed.empty() && r->bucketStartMs <= s.committed.back()->bucketStartMs) return false;
        if (s.provisional && s.provisional->bucketStartMs <= r->bucketStartMs) s.provisional.reset();
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
struct LiveService::Impl {
    std::filesystem::path root;
    LiveCache cache;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::vector<std::weak_ptr<Subscription>> subscriptions;
    std::thread worker;
    explicit Impl(std::filesystem::path p) : root(std::move(p)), worker([this] { run(); }) {}
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one();
        worker.join();
    }
    void run() {
        sentinel::logging::setCurrentThreadName("recording-live");
        Hmc2Reader reader(root); // constructed, used, and destroyed on this worker
        struct State {
            std::weak_ptr<Subscription> subscription;
            std::unique_ptr<LiveBuilder> builder;
            LiveCadence cadence;
            uint64_t deliveredRevision = 0;
        };
        std::map<const Subscription *, State> states;
        auto nowMs = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(); };
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
            std::erase_if(states, [](const auto &item) {
                auto s = item.second.subscription.lock(); return !s || !s->active.load();
            });
            for (const auto &s : views) {
                if (!s->active.load()) continue;
                auto [it, added] = states.try_emplace(s.get());
                auto &state = it->second;
                if (added) {
                    state.subscription = s;
                    state.builder = std::make_unique<LiveBuilder>(s->view);
                }
                if (!state.cadence.due(nowMs())) continue;
                const auto source = cache.snapshot(s->view.symbol, s->view.layer);
                if (!source.revision || source.revision == state.deliveredRevision) continue;
                bool accepted = false;
                try {
                    const auto page = state.builder->build(reader, source);
                    accepted = page.status == BuildStatus::Complete && s->active.load() && s->deliver(s->view, page);
                    if (page.status == BuildStatus::InvalidRequest || page.status == BuildStatus::IncompatibleGrid) {
                        sLog_Warning("Recording live view disabled: symbol=" << s->view.symbol
                            << " gen=" << s->view.generation << " reason=" << page.message);
                        s->active.store(false);
                    }
                    if (page.status != BuildStatus::Complete)
                        sLog_Probe("recording.live.build", "symbol=" << s->view.symbol << " status="
                            << static_cast<int>(page.status) << " message=" << page.message);
                    sLog_Probe("recording.live.send", "symbol=" << s->view.symbol << " tf=" << s->view.tfMs
                        << " gen=" << s->view.generation << " columns=" << page.columns.size()
                        << " accepted=" << accepted << " diskRecords=" << page.sourceRecords);
                } catch (const std::exception &e) {
                    sLog_Error("Recording live failed: symbol=" << s->view.symbol << " error=" << e.what());
                }
                if (accepted) state.deliveredRevision = source.revision;
                state.cadence.completed(nowMs(), accepted);
            }
        }
    }
};
LiveService::LiveService(std::filesystem::path root) : impl_(std::make_unique<Impl>(std::move(root))) {}
LiveService::~LiveService() = default;
bool LiveService::publish(RecordPtr record) { return impl_->cache.publish(std::move(record)); }
std::shared_ptr<LiveService::Subscription> LiveService::subscribe(LiveView view, Deliver deliver) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->subscriptions, [](const auto &w) { auto s = w.lock(); return !s || !s->active.load(); });
    if (impl_->subscriptions.size() >= 64) return {};
    auto subscription = std::make_shared<Subscription>(std::move(view), std::move(deliver));
    impl_->subscriptions.push_back(subscription);
    return subscription;
}
} // namespace recording
