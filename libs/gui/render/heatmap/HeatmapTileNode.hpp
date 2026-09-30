#pragma once
// Production heatmap scene-graph node (slice S5c, docs/research/2026-09-s5-plan.md).
// Render thread only. It consumes the controller's immutable, tick-free SpanSet
// (HeatmapSourceController) and owns every piece of GPU content it draws:
//
// - Sources. Each span source build (SpanSourceKey) is uploaded once into the
//   binner's resident pool, paged within the per-frame upload budget, and then
//   reported uploaded (HeatmapCapacity), so the controller releases its CPU image
//   and republishes the same key with gpu == nullptr. The first (coarsest) source
//   of a span is always wanted; a finer source is wanted while the rows around
//   the view overlap its coverage bands (today the near band), so a refinement
//   to its tick draws in the next frame.
// - Bins. For each span the node bins the rows around the view (the view plus one
//   view height each side) at the frame's tick into its own buffers: the first
//   source, then each finer resident source as a fill pass that replaces only the
//   cells the earlier passes left veiled, and only with valid cells (owner
//   decision 1; heatmap_bin.comp dims.w bit 1). A tick change, a vertical pan that
//   leaves the binned rows or new content is a compute pass in the same frame;
//   pans and zooms inside the rows only change the draw mapping.
// - Transitions (spec rules 7 and 8). When the target (timeframe, tick) changes,
//   the last drawn picture (held) keeps drawing until every visible span of the
//   new target is ready; a tick change then crossfades the old picture out over
//   crossfadeMs (a later change starts another fading layer; earlier layers finish
//   their own fade). A span whose content changed (revision, a new source) keeps
//   drawing its previous bin until the new one is ready (slot fallback).
// - Retention (B1 carry-over (a)). Content that is drawn (current, held, fading,
//   slot fallback) is never freed, together with the sources its bins came from;
//   the node retires it itself when the transition ends. The controller never
//   names tick content.
// - GPU cap. Resident bytes (sources and bins) are kept at or under the per-chart
//   cap by evicting, in order, sources the snapshot no longer lists, recent-tf,
//   then prefetch from far to near. Visible, fallback and drawn content is never
//   evicted (it may exceed the cap, as B1 measured); admission of a source only
//   evicts content ranked below it.
// - Capacity contract (INV-084). After every frame in which resident bytes
//   changed, a source finished uploading, or a source went missing, the node calls
//   HeatmapCapacity::report(cap - resident, uploaded keys, lost, missing keys);
//   lost is reported after the QRhi was lost or recreated (and when the node is
//   destroyed while it held content).
// Unprepared time inside the snapshot's availability draws the loading hatch, as
// do visible spans the CPU ceiling refused (SpanSet::refused).
#include "HeatmapGpuBinner.hpp"
#include "HeatmapSourceController.hpp"
#include <QSGRenderNode>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace heatmap::gpu {

// Written on the render thread, read anywhere.
struct HeatmapTileStats {
    std::atomic<uint64_t> frames{0}, sourcesUploaded{0}, uploadBytes{0}, binPasses{0}, fillPasses{0};
    std::atomic<uint64_t> binsMade{0}, rebins{0}, errors{0}, evictions{0}, missingReports{0}, reports{0};
    std::atomic<uint64_t> residentSources{0}, sourceBytes{0}, binBytes{0}, residentBytes{0}, gpuBytes{0};
    // Draws of content that is not resident, and drawn bins whose sources are not
    // resident: both must stay 0 (retention).
    std::atomic<uint64_t> missingDraws{0}, unpinnedDraws{0};
    std::atomic<uint32_t> slotCount{0}, readySlots{0}, fallbackSlots{0}, partialSlots{0}, loadingSlots{0};
    std::atomic<uint32_t> refusedSlots{0}, fadingLayers{0};
    std::atomic<bool> complete{false}, holding{false}, crossfading{false};
    std::atomic<int64_t> drawnTickUnits{0}, drawnTfMs{0};
    std::atomic<double> prepareMs{0}, lastBinMs{0}, gpuFrameMs{0};
    std::atomic<bool> preciseKernel{true};
    // Bin ids drawn in the last frame (current, held, fading) and resident bin ids.
    std::vector<uint64_t> drawnIds() const {
        std::scoped_lock lock(mutex);
        return drawn;
    }
    std::vector<uint64_t> residentIds() const {
        std::scoped_lock lock(mutex);
        return resident;
    }
    mutable std::mutex mutex;
    std::vector<uint64_t> drawn, resident;
};

class HeatmapTileNode final : public QSGRenderNode {
public:
    static constexpr size_t kMaxSpanSources = 4; // passes per bin (first + fills)
    static constexpr size_t kMaxFadingLayers = 4;
    struct Frame {
        std::shared_ptr<const SpanSet> spans;       // the controller's latest snapshot
        std::shared_ptr<HeatmapCapacity> capacity;  // reports go here (nullptr: none)
        int64_t tfMs = 0;      // the chart's timeframe (the snapshot may still show the previous one)
        int64_t tickUnits = 0; // display tick in price units of spans->priceScale (0: none yet)
        ViewWindow view;
        QRectF rect;
        recording::SizeScale outputScale;
        uint64_t uploadBudgetBytes = 8ull << 20;
        uint64_t gpuCapBytes = 320ull << 20; // HeatmapBudgets::gpuPerChart
        DrawStyle style;
        double crossfadeMs = 150;
    };
    explicit HeatmapTileNode(std::shared_ptr<HeatmapTileStats> stats = {});
    ~HeatmapTileNode() override;
    void setFrame(Frame frame) { frame_ = std::move(frame); }

    void prepare() override;
    void render(const RenderState *state) override;
    void releaseResources() override;
    StateFlags changedStates() const override { return ViewportState | ScissorState; }
    RenderingFlags flags() const override { return BoundedRectRendering | NoExternalRendering; }
    QRectF rect() const override { return frame_.rect; }

    // Tests only: pretend the retention rules do not exist (the node then frees
    // everything the current target does not use, like the B1 node), so the
    // retention tests can prove they fail without them.
    static void setRetentionDisabledForTest(bool disabled);

private:
    struct Source;
    struct Bin;
    struct DrawSlot;
    struct Draw {
        uint64_t bin = 0;
        int64_t tile = 0, tfMs = 0;
    };
    struct Layer {
        std::vector<Draw> draws;
        std::chrono::steady_clock::time_point start;
        bool used = false;
    };
    struct SpanRef { // one snapshot span and its sources' node entries (reindexed per snapshot)
        const SpanSnapshot *span = nullptr;
        std::array<Source *, kMaxSpanSources> sources{};
        uint8_t count = 0;
    };
    struct Slot { // one visible tile of the target this frame
        int64_t tile = 0;
        bool expected = false, refused = false, ready = false, complete = false;
        Bin *bin = nullptr; // the target's bin (ready or partial)
    };
    std::shared_ptr<HeatmapTileStats> stats_;
    Frame frame_;
    QRhi *rhi_ = nullptr;
    std::unique_ptr<HeatmapGpuBinner> binner_;
    std::unordered_map<SpanSourceKey, std::unique_ptr<Source>, SpanSourceKeyHash> sources_;
    std::vector<std::unique_ptr<Bin>> bins_;
    std::vector<std::unique_ptr<QRhiBuffer>> spareBuffers_; // retired bin cell buffers, reused
    std::vector<std::unique_ptr<DrawSlot>> loadingDraws_;
    std::unique_ptr<QRhiBuffer> loadingCell_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QVector<quint32> pipelineFormat_;
    int pipelineSamples_ = 0;
    uint64_t frameNo_ = 0, nextBinId_ = 1;
    std::unordered_map<uint64_t, Source *> byId_; // resident sources by binner id
    // Snapshot index (rebuilt when the snapshot changes). Holding the snapshot
    // keeps the SpanSnapshot pointers valid.
    std::shared_ptr<const SpanSet> indexedSet_;
    const SpanSet *indexed_ = nullptr;
    uint64_t indexedVersion_ = 0;
    bool reindex_ = true;
    std::vector<SpanRef> spanRefs_;                   // snapshot order (by rank)
    std::vector<std::pair<SpanId, size_t>> byTile_; // sorted: span id -> spanRefs_ index
    // Transition state.
    int64_t drawnTf_ = 0, drawnTick_ = 0;
    std::vector<Draw> held_, now_;
    std::array<Layer, kMaxFadingLayers> fading_;
    std::vector<Slot> slots_;
    std::vector<std::pair<QRhiShaderResourceBindings *, float>> tileDraws_; // this frame: block bindings, opacity
    size_t fadeDrawsFrom_ = 0;                                            // tileDraws_ index of the fading layers
    size_t loadingUsed_ = 0;
    // Capacity reporting.
    std::shared_ptr<HeatmapCapacity> reportedTo_;
    uint64_t reportedBytes_ = UINT64_MAX;
    std::vector<SpanSourceKey> uploaded_, missing_;
    std::vector<SpanSourceKey> reportedMissing_; // not reported again while the snapshot still lists them
    bool lost_ = false;
    std::vector<Source *> order_; // reused per frame
    std::vector<std::pair<int64_t, int64_t>> covered_;
    std::vector<uint64_t> drawnScratch_;
    QString lastError_;

    void noteError(const QString &error);
    void releaseAll(bool reportLoss);
    void reindex();
    const SpanRef *spanAt(int64_t tfMs, int64_t tile) const;
    bool wanted(const SpanRef &ref, size_t index) const;
    void upload(QRhiCommandBuffer *cb);
    bool admit(Source &source); // cap check before creating a source's buffers
    void evictDown(uint64_t target, const SpanRank *incoming);
    void freeSource(Source &source, bool report);
    Bin *binFor(const SpanRef &ref, int64_t tickUnits, QRhiCommandBuffer *cb, bool *complete);
    bool binRows(Bin &bin, QRhiCommandBuffer *cb);
    bool rowsCover(const Bin &bin) const;
    Bin *findBin(uint64_t id) const;
    void retire(bool keepDrawn);
    void pinSources(const Bin &bin);
    uint64_t residentBytes() const; // sources, bins and spare bin buffers
    uint64_t spareBytes() const;
    void report();
    bool ensurePipeline(QRhiRenderPassDescriptor *pass, int samples);
    void addBinDraw(QRhiResourceUpdateBatch *updates, Bin &bin, float opacity);
    bool addLoadingDraw(QRhiResourceUpdateBatch *updates, int64_t loMs, int64_t hiMs);
    bool addLoadingSlot();
    bool subRect(int64_t firstBucket, int64_t endBucket, int64_t tfMs, QRectF *rect, ViewWindow *sub) const;
    std::unique_ptr<QRhiBuffer> cellBuffer(uint64_t bytes);
};
} // namespace heatmap::gpu
