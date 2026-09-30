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
    std::shared_ptr<const gpu::GpuSource> gpu;
    ResolutionSummary resolution; // this source's columns of the span
    int64_t commonUnits = 0;      // LCM of every column's common tick (0 = no data)
    size_t bytes = 0;
};
using SpanSourceBuildPtr = std::shared_ptr<const SpanSourceBuild>;
struct SpanSourceInput {
    SpanSourceKey key;
    std::vector<std::shared_ptr<const StoredChunk>> chunks; // the generations of key
};
// Composes the chunks at the span's tf and builds the upload image and the
// resolution summary. Pure; any thread; throws what the builders throw.
SpanSourceBuildPtr buildSpanSource(const SpanSourceInput &input);

// One per process, on the heatmap-data thread, shared by every controller:
// a byte-bounded LRU of span-source builds plus a weak registry, so a build that
// some chart still publishes is shared even after the LRU dropped it. Concurrent
// requests for one key share a single build. At most maxJobs keys are queued or
// running; request() refuses more and emits settled() when a job finishes.
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
    };
    explicit SpanSourceCache(QObject *parent = nullptr);
    explicit SpanSourceCache(Options options, QObject *parent = nullptr);
    ~SpanSourceCache() override;
    using Completion = std::function<void(SpanSourceBuildPtr build, QString error)>;
    // A cached or still-published build (touches the LRU), or nullptr.
    SpanSourceBuildPtr find(const SpanSourceKey &key);
    // Builds input (or joins the build in flight for its key). The completion
    // runs on this thread, unless `context` was destroyed. False when the queue
    // is full: retry after settled().
    bool request(SpanSourceInput input, int priority, QObject *context, Completion completion);
    void setMaxBytes(size_t bytes);
    struct Stats {
        uint64_t builds = 0, hits = 0, sharedBuilds = 0, failures = 0, evictions = 0;
        size_t bytes = 0, entries = 0, jobs = 0;
    };
    Stats stats() const;
signals:
    void settled();
private:
    struct State;
    std::unique_ptr<State> state_;
    QThreadPool pool_;
};

// Written by the render thread (HeatmapTileNode, S5c), read by the controller:
// the node stores its free GPU bytes, then bumps capacityEpoch whenever it frees
// bytes (a transition retires, a revision shrinks a source). Every admission
// consumes the last reported free bytes, so the node reports after every
// release, not only once. No QObject access.
struct HeatmapCapacity {
    std::atomic<size_t> freeBytes{320ull << 20};
    std::atomic<uint64_t> capacityEpoch{0};
    void reportFree(size_t bytes) {
        freeBytes.store(bytes, std::memory_order_relaxed);
        capacityEpoch.fetch_add(1, std::memory_order_release);
    }
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
        size_t gpuBytes = 320ull << 20;       // per-chart cap (HeatmapBudgets::gpuPerChart)
        size_t sourceEstimateBytes = 8ull << 20; // admission estimate of an unbuilt span source
        int capacityPollMs = 16;              // <= 0: tests call pollCapacity()
    };
    HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache, Options options,
                            QObject *parent = nullptr);
    HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                            QObject *parent = nullptr);
    ~HeatmapSourceController() override;
    // Owner thread only (queue from others). A symbol or tf change bumps the
    // serial; the previous tf's built visible spans stay as fallback, then as
    // the recent-tf tier, so switching back republishes them without a build.
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

    // Re-reads the capacity epoch; after a bump, suppressed prefetch is
    // re-admitted strictly by rank while free >= bytes + 10%.
    void pollCapacity();
    struct Stats {
        uint64_t publications = 0, staleResults = 0, evictions = 0, admissions = 0;
        size_t suppressed = 0; // spans suppressed by the last reconcile
    };
    Stats stats() const { return stats_; } // owner thread only
signals:
    // Connect queued, then take latestSnapshot().
    void snapshotChanged();
    void buildFailed(QString message);
private:
    struct SourceSlot {
        SpanSourceBuildPtr ready;
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
    size_t credits_ = 0;
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
    void onBuilt(uint64_t serial, const SpanSourceKey &key, SpanSourceBuildPtr build, const QString &error);
    size_t slotBytes(const Slot &slot) const;
    std::vector<SourceAvailability> sources();
};
} // namespace heatmap
