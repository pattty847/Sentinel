#pragma once
// Per-chart heatmap source controller and the process-wide span-source cache
// (plan docs/research/2026-09-s5-plan.md, slice S5b).
//
// QtCore only: no QSG, QRhi, QtGui or render-thread types, so it tests without a
// GPU. It lives in libs/gui (not core) because it builds gpu::GpuSource upload
// images, which belong to the GUI heatmap library.
//
// Threads: every controller, the SpanSourceCache and the ChunkFetcher live on one
// event-loop thread ("heatmap-data" in production; tests drive them on the test
// thread). Other threads queue calls onto it. Builds run on the cache's bounded
// pool (2 threads). latestSnapshot() and capacity() are safe on any thread.
//
// Data flow: setView -> planSpans (visible > fallback > prefetch > recent-tf) ->
// ChunkFetcher::want with rank priorities -> chunkStored/chunkRevised -> peek the
// store -> build each span's GpuSource per source id on the pool (shared across
// charts through the cache) -> publish an immutable, tick-free SpanSet.
#include "HeatmapGpuSource.hpp"
#include "heatmap/ChunkFetcher.hpp"
#include "heatmap/HeatmapSpanPlanner.hpp"
#include <QThreadPool>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>

namespace heatmap {
// Process RAM tiers and the per-chart GPU cap (owner decision 4). The two CPU
// tiers must fit under the CPU-side ceiling.
struct HeatmapBudgets {
    size_t decodedChunks = 512ull << 20; // ChunkStore
    size_t spanSources = 256ull << 20;   // SpanSourceCache (evictable, rebuilt from chunks)
    size_t cpuCeiling = 1024ull << 20;
    size_t gpuPerChart = 320ull << 20;   // HeatmapSourceController::setGpuBudget
    bool valid() const;
};

struct ChunkGeneration {
    std::string symbol, source;
    int64_t levelMs = 0, startMs = 0;
    uint64_t generation = 0;
    auto operator<=>(const ChunkGeneration &) const = default;
};
// Identity of one span's source build: equal keys build identical sources, so
// every chart showing the span shares one build.
struct SpanSourceKey {
    SpanId span;
    std::string source;
    int64_t availableStartMs = 0, availableEndMs = 0; // SpanSourcePlan bounds
    double priceScale = 100;                          // ResolutionSummary units
    std::vector<ChunkGeneration> generations;         // sorted
    auto operator<=>(const SpanSourceKey &) const = default;
};
struct SpanSourceKeyHash {
    size_t operator()(const SpanSourceKey &key) const;
};
struct SpanSourceBuild {
    SpanSourceKey key;
    // The CPU upload image. nullptr in a snapshot means the node reported the
    // upload and the controller released the image (see HeatmapCapacity).
    std::shared_ptr<const gpu::GpuSource> gpu;
    ResolutionSummary resolution; // this source's columns of the span
    int64_t commonUnits = 0;      // LCM of every column's common tick (0 = no data)
    size_t bytes = 0;             // CPU allocation (vector capacities) of the build
    size_t uploadBytes = 0;       // GPU payload (gpu::GpuSource::bytes())
};
using SpanSourceBuildPtr = std::shared_ptr<const SpanSourceBuild>;
struct SpanSourceInput {
    SpanSourceKey key;
    std::vector<std::shared_ptr<const StoredChunk>> chunks; // the generations of key
};
// Composes the chunks at the span's tf and builds the upload image and the
// resolution summary. Pure; any thread; throws what the builders throw. When
// liveBytes is given, it holds the CPU bytes of the image while it is alive.
SpanSourceBuildPtr buildSpanSource(const SpanSourceInput &input,
                                   std::shared_ptr<std::atomic<int64_t>> liveBytes = {});

class HeatmapSourceController;

// One per process, on the heatmap-data thread, shared by every controller. It
// must outlive them.
// - Builds: a bounded pool (2 threads); concurrent requests for one key share
//   one build; at most maxJobs keys are queued or running (request() refuses
//   more and emits settled() when a job finishes).
// - Sharing: an LRU of builds plus a weak registry, so a build some chart still
//   holds is shared even after the LRU dropped it.
// - CPU tier (maxBytes, HeatmapBudgets::spanSources): controllers claim the
//   images they hold; claimed images plus reservations of running builds are
//   pinned. The LRU keeps unclaimed images only within what is left. When the
//   claimed bytes alone exceed the tier, the cache drops the lowest-rank slots
//   of all controllers (prefetch, recent-tf; never visible or fallback) until
//   they fit. Controllers admit prefetch only with CPU room (size hints of past
//   builds make that admission match the real sizes).
// - liveBytes counts every image alive anywhere (slots, LRU, snapshots, the
//   node's copy of a snapshot): snapshots lag by at most one frame.
class SpanSourceCache final : public QObject {
    Q_OBJECT
public:
    struct Options {
        size_t maxBytes = 256ull << 20;
        size_t maxJobs = 8;
        int threads = 2;
        // Runs a build job; default: the cache's thread pool, by priority. Tests
        // inject a queue they run explicitly (deterministic, no sleeps).
        std::function<void(std::function<void()> job, int priority)> executor;
        std::function<void()> beforeBuild; // tests only: runs on the build thread
    };
    explicit SpanSourceCache(QObject *parent = nullptr);
    explicit SpanSourceCache(Options options, QObject *parent = nullptr);
    ~SpanSourceCache() override;
    using Completion = std::function<void(SpanSourceBuildPtr build, QString error)>;
    // A cached or still-held build (touches the LRU), or nullptr.
    SpanSourceBuildPtr find(const SpanSourceKey &key);
    // Builds input (or joins the build in flight for its key), reserving
    // reserveBytes of the tier until it completes. The completion runs on this
    // thread, unless `context` was destroyed. False when the queue is full:
    // retry after settled().
    bool request(SpanSourceInput input, int priority, size_t reserveBytes, QObject *context, Completion completion);
    // Pins an image in the CPU tier until the returned token is destroyed (on
    // this thread). A key claimed by several holders counts once.
    std::shared_ptr<void> claim(const SpanSourceBuildPtr &build);
    // Sizes of the last build of (span, source) at any generation; 0 unknown.
    struct Hint { size_t bytes = 0, uploadBytes = 0; };
    Hint hint(const SpanId &span, const std::string &source) const;
    size_t pinnedBytes() const; // claimed + reserved
    size_t maxBytes() const;
    void setMaxBytes(size_t bytes);
    struct Stats {
        uint64_t builds = 0, hits = 0, sharedBuilds = 0, failures = 0, evictions = 0, pressureDrops = 0;
        size_t bytes = 0, entries = 0, jobs = 0; // LRU
        size_t claimedBytes = 0, reservedBytes = 0;
        int64_t liveBytes = 0;
    };
    Stats stats() const;
signals:
    void settled();
private:
    friend class HeatmapSourceController;
    struct State;
    std::shared_ptr<State> state_;
    QThreadPool pool_;
    bool relieving_ = false;
    void attach(HeatmapSourceController *controller);
    void detach(HeatmapSourceController *controller);
    void trim();
    void relieve();
};

// The node side of the capacity contract (HeatmapTileNode, S5c; render thread,
// no QObject access). The node calls report() whenever its resident bytes
// change: after an upload as well as after a release (a transition retires, a
// revision shrinks a source). freeBytes is the per-chart cap minus resident
// bytes; `uploaded` names the span sources it has uploaded since its last
// report (their CPU images are then released: the node keeps the GPU copy,
// identified by SpanSourceBuild::key); `lost` says the node lost its GPU copies
// (QRhi loss), and the controller rebuilds the released images.
// The controller's credit is freeBytes minus its outstanding reservations (every
// admitted source not reported uploaded, at its built size or estimate), so a
// build smaller than its estimate returns credit at once, and a static view makes
// progress without new reports.
struct HeatmapCapacity {
    std::atomic<uint64_t> capacityEpoch{0};
    void report(size_t freeBytes, std::vector<SpanSourceKey> uploaded = {}, bool lost = false) {
        {
            std::scoped_lock lock(mutex_);
            free_ = freeBytes;
            for (auto &key : uploaded) uploaded_.push_back(std::move(key));
            lost_ = lost_ || lost;
        }
        capacityEpoch.fetch_add(1, std::memory_order_release);
    }
    void reportFree(size_t freeBytes) { report(freeBytes); }
    struct Report {
        size_t freeBytes = 0;
        std::vector<SpanSourceKey> uploaded;
        bool lost = false;
    };
    // Controller: the latest free bytes and everything reported since the last take.
    Report take() {
        std::scoped_lock lock(mutex_);
        Report out{free_, std::move(uploaded_), lost_};
        uploaded_.clear();
        lost_ = false;
        return out;
    }
private:
    std::mutex mutex_;
    size_t free_ = 0;
    std::vector<SpanSourceKey> uploaded_;
    bool lost_ = false;
};

struct SpanSourceSnapshot {
    std::string source;
    SpanSourceBuildPtr build; // nullptr until built
    bool stale = false;       // build is an older generation; the rebuild is pending
};
struct SpanSnapshot {
    SpanId id;
    SpanRank rank;
    // Coarsest common tick first: the node bins them in this order, and each
    // later (finer) source fills only the cells the earlier ones left veiled.
    std::vector<SpanSourceSnapshot> sources;
    bool complete = false; // every source built at its latest generation
};
// Immutable and tick-free: the GUI thread picks the tick (Auto from `resolution`).
struct SpanSet {
    uint64_t version = 0; // per controller, increments on every publication
    uint64_t serial = 0;  // controller serial (symbol, tf or store change)
    std::string symbol;
    int64_t tfMs = 0;
    double priceScale = 100;
    std::vector<SpanSnapshot> spans; // by rank
    ResolutionSummary resolution;    // built spans of tfMs, ascending columns
};

// The store, fetcher and cache must outlive every controller.
class HeatmapSourceController final : public QObject {
    Q_OBJECT
public:
    struct Options {
        size_t gpuBytes = 320ull << 20;          // per-chart cap (HeatmapBudgets::gpuPerChart)
        size_t sourceEstimateBytes = 8ull << 20; // estimate of an unbuilt span source without a size hint
        int capacityPollMs = 16;                 // <= 0: tests call pollCapacity()
    };
    HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache, Options options,
                            QObject *parent = nullptr);
    HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                            QObject *parent = nullptr);
    ~HeatmapSourceController() override;
    // Owner thread only (queue from others). A symbol or tf change bumps the
    // serial (in-flight builds of the old serial are dropped and rejoined from
    // the cache); the previous tf's built visible spans stay as fallback, then
    // as the recent-tf tier, so switching back republishes them without a build.
    void setView(const std::string &symbol, int64_t tfMs, double timeLoMs, double timeHiMs);
    // The tick the GUI drew. Sources are tick-free, so this never rebuilds;
    // node residency uses it (S5c).
    void setTickRequest(TickMode mode, int64_t units);
    void setGpuBudget(size_t bytes);
    // Applies the CPU tiers to the process-wide store and cache; false (and no
    // change) when the budgets are invalid.
    static bool applyBudgets(const HeatmapBudgets &budgets, ChunkStore &store, SpanSourceCache &cache);

    // Any thread (including updatePaintNode).
    std::shared_ptr<const SpanSet> latestSnapshot() const;
    std::shared_ptr<HeatmapCapacity> capacity() const { return capacity_; }

    // Applies node reports (free bytes, uploads, loss) after a capacity epoch
    // bump; suppressed prefetch is re-admitted strictly by rank while the
    // credit is >= bytes + 10%.
    void pollCapacity();
    struct Stats {
        uint64_t publications = 0, staleResults = 0, evictions = 0, admissions = 0;
        uint64_t pressureDrops = 0, releasedImages = 0;
        size_t suppressed = 0; // spans suppressed by the last reconcile
    };
    Stats stats() const { return stats_; } // owner thread only
signals:
    // Connect queued, then take latestSnapshot().
    void snapshotChanged();
    void buildFailed(QString message);
private:
    friend class SpanSourceCache;
    struct SourceSlot {
        SpanSourceBuildPtr ready;   // gpu == nullptr once released after upload
        std::shared_ptr<void> claim; // CPU tier claim while the image is held
        bool uploaded = false;       // the node reported ready's upload
        std::optional<SpanSourceKey> expected, pending, failed;
    };
    struct Slot {
        PlannedSpan plan;
        std::map<std::string, SourceSlot> sources;
    };
    ChunkStore &store_;
    ChunkFetcher &fetcher_;
    SpanSourceCache &cache_;
    Options options_;
    ChunkFetcher::ChartId chart_;
    std::string symbol_;
    int64_t tfMs_ = 0;
    double timeLoMs_ = 0, timeHiMs_ = 0;
    TickMode tickMode_ = TickMode::Auto;
    int64_t tickUnits_ = 0;
    double priceScale_ = 100;
    uint64_t serial_ = 0, version_ = 0, epoch_ = 0;
    size_t reportedFree_ = 0;
    bool reported_ = false;
    bool scheduled_ = false, dirty_ = false, refused_ = false, visibleReady_ = true;
    std::map<SpanId, Slot> slots_;
    std::vector<SpanId> retained_; // previous tf's visible spans
    std::unordered_set<ChunkKey, ChunkKeyHash> wanted_, failedChunks_;
    std::shared_ptr<HeatmapCapacity> capacity_;
    mutable std::mutex latestMutex_;
    std::shared_ptr<const SpanSet> latest_;
    Stats stats_;
    void schedule();
    void reconcile();
    void publish();
    void reset();
    void setReady(SourceSlot &source, SpanSourceBuildPtr build);
    void onBuilt(uint64_t serial, const SpanSourceKey &key, SpanSourceBuildPtr build, const QString &error);
    size_t estimate(const SpanId &span, const std::string &source, bool upload) const;
    size_t gpuBytes(const Slot &slot) const;
    size_t outstanding(const Slot &slot) const;
    std::vector<SourceAvailability> sources();
    // SpanSourceCache pressure relief.
    std::vector<std::pair<SpanRank, SpanId>> releasable() const;
    void dropForMemory(const SpanId &span);
};
} // namespace heatmap
