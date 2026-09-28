#pragma once
#include "RecordingPage.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <map>
#include <algorithm>

namespace recording {
using RecordPtr = std::shared_ptr<const Hmc2Record>;
struct LiveView {
    std::string symbol, layer;
    int64_t tfMs = 60'000;
    PriceBand band;
    uint64_t generation = 0;
};
// Bounded, immutable publication mailbox. No disk or projection under its lock.
class LiveCache {
public:
    struct Snapshot {
        std::map<int64_t, RecordPtr> provisional;
        int64_t committedThroughMs = 0;
        std::deque<RecordPtr> committed;
        uint64_t revision = 0;
    };
    bool publish(RecordPtr record);
    Snapshot snapshot(const std::string &symbol, const std::string &layer) const;
    std::optional<std::pair<std::string, std::string>> takeCapacityWarning();
    static constexpr size_t kMaxSeries = 128, kMaxRecords = 16, kMaxEntries = 262144;
private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Snapshot> series_;
    bool warnedSeriesLimit_ = false;
    std::optional<std::pair<std::string, std::string>> capacityWarning_;
};
// Worker-owned, incremental two-bucket projection. Reader warmup resumes at its
// proven scan cursor. No cold I/O is repeated each second.
class LiveBuilder {
public:
    explicit LiveBuilder(LiveView view);
    ~LiveBuilder();
    BuildResult build(Hmc2Reader &reader, const LiveCache::Snapshot &source,
                      int64_t deliveredFinalThroughMs = 0);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct LiveCadence {
    int64_t nextMs = 0;
    int64_t delayMs = 1000;
    bool due(int64_t now) const { return now >= nextMs; }
    void completed(int64_t now, bool accepted) {
        delayMs = accepted ? 1000 : std::min<int64_t>(5000, delayMs * 2);
        nextMs = now + delayMs;
    }
};
// Independent transport admission: history bytes cannot consume this one slot.
class LiveWriteSlot {
public:
    bool tryAcquire() { bool expected = false; return busy_.compare_exchange_strong(expected, true); }
    void release() { busy_.store(false); }
private:
    std::atomic_bool busy_{false};
};
struct LiveRegistrationGate {
    int64_t nextMs = 0;
    bool admit(int64_t now) {
        if (now < nextMs) return false;
        nextMs = now + 250;
        return true;
    }
};
class LiveService {
public:
    // Callback runs on live worker; must reserve bounded transport capacity and return.
    using Deliver = std::function<bool(const LiveView &, const BuildResult &)>;
    struct Subscription {
        LiveView view;
        Deliver deliver;
        std::atomic_bool active{true};
        explicit Subscription(LiveView v, Deliver d) : view(std::move(v)), deliver(std::move(d)) {}
    };
    explicit LiveService(std::filesystem::path root);
    ~LiveService();
    // Idempotent. Deactivates subscriptions and joins in-flight delivery before
    // the transport executor can be stopped/destroyed. Call off the live worker.
    void shutdown();
    // Idempotent restart after shutdown; old subscriptions remain inactive.
    void start();
    struct Diagnostics { uint64_t builds = 0, buildMicros = 0, deliveries = 0, deliveryMicros = 0; };
    Diagnostics diagnostics() const;
    bool publish(RecordPtr record);
    std::shared_ptr<Subscription> subscribe(LiveView view, Deliver deliver);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
