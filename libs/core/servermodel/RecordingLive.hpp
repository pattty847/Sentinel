#pragma once
#include "RecordingPage.hpp"
#include "../heatmap/ChunkCodec.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <map>
#include <algorithm>

struct RecordingLiveTest;
struct HeatmapChunkWireTest;
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
struct RawTailView {
    std::string symbol;
    std::vector<std::string> sources;
    uint64_t sub = 0;
    int64_t sinceMs = 0; // exclusive delivered-final cutoff (bucket starts >= it are resent)
};
struct RawTailFrame {
    std::shared_ptr<const std::vector<uint8_t>> bytes; // SHC1, shared before per-sub SHE1 wrapping
    uint64_t revision = 0;
    int64_t finalThroughMs = 0;
};
inline constexpr size_t kRawLiveByteBudget = 1024 * 1024;
// Worker-owned per-series encoder. Variants are keyed by the first held final
// needed; every viewer without pending finals gets the same immutable bytes.
class RawTailBuilder {
public:
    RawTailFrame build(const std::string& symbol, const std::string& source,
                       const LiveCache::Snapshot& snapshot, int64_t sinceMs);
    uint64_t encodings() const { return encodings_; }
private:
    uint64_t revision_ = 0, encodings_ = 0;
    int64_t nextOversizeWarningMs_ = 0;
    std::string symbol_, source_;
    // At most 17 frames per series/revision: the first needed final is one of
    // LiveCache's <=16 held records, or zero for the shared no-finals variant.
    std::map<int64_t, RawTailFrame> variants_;
    heatmap::ChunkFrame frame_;
    heatmap::ChunkEncodeScratch scratch_;
    std::vector<RecordPtr> records_;
    std::vector<RecordPtr> filledRecords_;
};
// The recorder publishes the open minute every recording.live_publish_ms
// (RecorderConfig::livePublishMs, default 500). The live worker checks for a
// new revision at half that interval: frames still follow publications (one
// per revision, so the frame rate is the publish rate), but a worker cadence
// equal to the publish interval plus its 100 ms turn quantization falls behind
// the recorder and skips publications, so the data age would sawtooth through a
// whole interval instead of staying within one worker turn.
inline constexpr int64_t kLivePublishDefaultMs = 500;
inline constexpr int64_t kLiveCadenceMaxMs = 5000;
constexpr int64_t liveCadenceMs(int64_t publishMs) {
    return std::clamp<int64_t>(publishMs / 2, 1, kLiveCadenceMaxMs);
}
inline constexpr int64_t kLiveCadenceDefaultMs = liveCadenceMs(kLivePublishDefaultMs);
// Per-subscriber send pacing: baseMs after an accepted send; a refused one
// doubles the delay up to kLiveCadenceMaxMs; the next acceptance resets it.
struct LiveCadence {
    int64_t baseMs = kLiveCadenceDefaultMs;
    int64_t nextMs = 0;
    int64_t delayMs = baseMs;
    bool due(int64_t now) const { return now >= nextMs; }
    void completed(int64_t now, bool accepted) {
        delayMs = accepted ? baseMs : std::min(std::max(kLiveCadenceMaxMs, baseMs), delayMs * 2);
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
// Raw siblings share a byte budget, not a single in-flight frame. Account for
// the SHE1 envelope too, from worker admission through write completion/drop.
class LiveWriteBudget {
public:
    bool tryAcquire(size_t bytes) {
        auto pending = bytes_.load();
        do {
            if (bytes > kRawLiveByteBudget || pending > kRawLiveByteBudget - bytes) return false;
        } while (!bytes_.compare_exchange_weak(pending, pending + bytes));
        return true;
    }
    void release(size_t bytes) { bytes_.fetch_sub(bytes); }
    size_t bytes() const { return bytes_.load(); }
private:
    std::atomic_size_t bytes_{0};
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
    using RawDeliver = std::function<bool(const RawTailView&, const std::string&, const RawTailFrame&)>;
    struct RawSubscription {
        RawTailView view;
        RawDeliver deliver;
        std::atomic_bool active{true};
        RawSubscription(RawTailView v, RawDeliver d) : view(std::move(v)), deliver(std::move(d)) {}
    };
    static constexpr size_t kMaxRawSubscriptions = 128; // symbols, separate from legacy views
    // cadenceMs: LiveCadence base for every subscription (liveCadenceMs() of
    // the recorder's publish interval).
    explicit LiveService(std::filesystem::path root, int64_t cadenceMs = kLiveCadenceDefaultMs);
    ~LiveService();
    // Idempotent. Deactivates subscriptions and joins in-flight delivery before
    // the transport executor can be stopped/destroyed. Call off the live worker.
    void shutdown();
    // Idempotent restart after shutdown; old subscriptions remain inactive.
    void start();
    struct Diagnostics {
        uint64_t builds = 0, buildMicros = 0, deliveries = 0, deliveryMicros = 0;
        uint64_t rawEncodings = 0, rawBuildMicros = 0, rawDeliveries = 0;
        uint64_t rawBuilds = 0, rawFailures = 0;
    };
    Diagnostics diagnostics() const;
    bool publish(RecordPtr record);
    std::shared_ptr<Subscription> subscribe(LiveView view, Deliver deliver);
    std::shared_ptr<RawSubscription> subscribeRaw(RawTailView view, RawDeliver deliver);
private:
    friend struct ::RecordingLiveTest;
    friend struct ::HeatmapChunkWireTest;
    // Deterministic test seams: set the clock while stopped; run a full worker
    // turn and wait for it to finish. Production uses steady_clock + 100 ms wake.
    void setClockForTest(std::function<int64_t()> clock);
    void pollForTest();
    void setRawWorkHookForTest(std::function<void(const char*)> hook);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
