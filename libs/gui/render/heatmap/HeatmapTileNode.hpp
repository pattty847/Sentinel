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
// - Live edge (S5L-c, docs/research/2026-09-s5l-plan.md section 3). The
//   controller's LiveSnapshot (Frame::live) is uploaded only when its version
//   (client-local; never the server revision) changes: each source into a
//   resident buffer set the node refills in place (HeatmapGpuBinner::
//   refillResident; sets rotate so a frame in flight never sees a CPU write, and
//   steady state creates no QRhiBuffer), then the live window [L, open end) is
//   binned at the frame's tick (coarsest source, then fill passes) into a live
//   bin that is an ordinary drawn entry: it is held with the picture across a
//   timeframe switch, fades with its layer on a tick change (the new tick bins
//   the live window in the same frame), and a new version re-bins the current
//   live bin in place (no crossfade). Frames with no new version do no live work.
//   Draw clip, per picture: a span bin draws its buckets before its complete end
//   E (SpanSourceBuild::completeEndMs, the lowest of its passes); the live bin
//   draws [max(L, E), open end), where E is that of the first span in the live
//   window that is not complete (a tile with no span bin counts as E = its
//   start); span bins after that point give way to the live bin. Every bucket
//   comes from exactly one bin, and a gap [E, L) draws loading. With no new
//   version (disconnected) the last live bin stays drawn, frozen.
// Unprepared time inside the snapshot's availability draws the loading hatch, as
// do visible spans the CPU ceiling refused (SpanSet::refused).
#include "HeatmapGpuBinner.hpp"
#include "HeatmapPalette.hpp"
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
    // Sources released in the frame that uploaded them (their buffers may be
    // referenced by that frame's commands): must stay 0.
    std::atomic<uint64_t> sameFrameReleases{0};
    std::atomic<uint32_t> slotCount{0}, readySlots{0}, fallbackSlots{0}, partialSlots{0}, loadingSlots{0};
    std::atomic<uint32_t> refusedSlots{0}, fadingLayers{0};
    std::atomic<bool> complete{false}, holding{false}, crossfading{false};
    // The last prepare() left work that needs another frame with no new input:
    // budgeted uploads, a crossfade, a live replacement paging in, the precision
    // self-test readback. Hosts schedule a frame while it is set (S6b review 3).
    std::atomic<bool> wantsFrame{false};
    std::atomic<int64_t> drawnTickUnits{0}, drawnTfMs{0};
    std::atomic<double> prepareMs{0}, lastBinMs{0}, gpuFrameMs{0};
    std::atomic<bool> preciseKernel{true};
    // Live edge (S5L-c). Uploads and bin passes happen only on a new version;
    // liveBufferCreations counts every QRhiBuffer created for live content (sets,
    // pass uniforms, live bin cells): 0 in steady state.
    std::atomic<uint64_t> liveUploads{0}, liveUploadBytes{0}, liveBinPasses{0}, liveBufferCreations{0};
    // Bytes recorded for upload in the last frame (span sources and live sets
    // together: at most Frame::uploadBudgetBytes).
    std::atomic<uint64_t> frameUploadBytes{0};
    // Sources of live-edge spans acknowledged without an upload (out of the view's
    // band: nothing bins them; the controller's L waits for every source).
    std::atomic<uint64_t> acknowledgedOnly{0};
    std::atomic<uint64_t> liveVersion{0}; // live version drawn in the current picture (0: none)
    std::atomic<uint32_t> liveSets{0};    // resident live buffer sets (current, pinned, spare)
    // Current picture: L, the live bin's end (open end rounded up to the tf), where
    // it starts drawing (max(L, E)), and E of the span at that point (0: none).
    std::atomic<int64_t> liveStartMs{0}, liveEndMs{0}, liveDrawFromMs{0}, liveEdgeCompleteMs{0};
    // At the first frame that drew a version: publication (LiveSnapshot::publishedNs)
    // to draw, and the age of its newest observation (wall clock).
    std::atomic<double> livePublishToDrawMs{0}, liveDataAgeMs{0};
    struct Segment {
        enum Kind : uint8_t { Span, Live, Loading };
        int64_t loMs = 0, hiMs = 0;
        uint64_t bin = 0; // 0 for loading
        Kind kind = Span;
        uint8_t layer = 0; // 0: the current (or held) picture; k: fading layer k
        float opacity = 1;
        uint64_t liveVersion = 0; // live bins: the version they hold
    };
    // Bin ids drawn in the last frame (current, held, fading) and resident bin ids.
    std::vector<uint64_t> drawnIds() const {
        std::scoped_lock lock(mutex);
        return drawn;
    }
    std::vector<uint64_t> residentIds() const {
        std::scoped_lock lock(mutex);
        return resident;
    }
    // Time ranges the last frame drew, by kind (tests, telemetry).
    std::vector<Segment> segments() const {
        std::scoped_lock lock(mutex);
        return drawnSegments;
    }
    // (publish-to-draw ms, data age ms) per drawn live version, oldest first (bounded).
    std::vector<std::pair<double, double>> liveSamples() const {
        std::scoped_lock lock(mutex);
        return liveLatency;
    }
    static constexpr size_t kMaxLiveSamples = 4096;
    mutable std::mutex mutex;
    std::vector<uint64_t> drawn, resident;
    std::vector<Segment> drawnSegments;
    std::vector<std::pair<double, double>> liveLatency;
};

// Tests: read back the cells of every bin a frame draws (offscreen frames
// complete synchronously: the results are filled once the frame has ended).
struct HeatmapCellCapture {
    struct Block {
        uint64_t bin = 0;
        bool live = false;
        BinGrid grid;
        std::shared_ptr<QRhiReadbackResult> result;
    };
    std::vector<Block> blocks;
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
        // Colours (S6b): the palette image and tone mapping; nullptr draws the
        // legacy default palette. Uploaded only when the pointer changes.
        std::shared_ptr<const HeatmapPalette> palette;
        double crossfadeMs = 150;
        // The controller's latest live snapshot (nullptr: none; the last live bin
        // then stays only while a drawn picture holds it).
        std::shared_ptr<const LiveSnapshot> live;
        std::shared_ptr<HeatmapCellCapture> capture; // tests only
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
    // Tests: attach to a QRhi outside a scene graph (as prepare() does on a new
    // QRhi), so a test can destroy the QRhi first.
    void attachRhiForTest(QRhi *rhi) { attachRhi(rhi); }

private:
    struct Source;
    struct Bin;
    struct DrawSlot;
    struct LiveSet;
    struct Draw {
        uint64_t bin = 0;
        int64_t tile = 0, tfMs = 0;
        bool live = false;
    };
    struct Piece { // one draw of a picture, clipped in time
        Bin *bin = nullptr;
        int64_t loMs = 0, hiMs = 0;
        bool live = false;
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
    // A binner released inside the QRhi's cleanup traversal: kept alive until the
    // traversal is over (its destructor would remove its own callback from the
    // hash being iterated), destroyed on the next prepare() or with the node.
    std::unique_ptr<HeatmapGpuBinner> retiredBinner_;
    std::unordered_map<SpanSourceKey, std::unique_ptr<Source>, SpanSourceKeyHash> sources_;
    std::vector<std::unique_ptr<Bin>> bins_;
    std::vector<std::unique_ptr<QRhiBuffer>> spareBuffers_; // retired bin cell buffers, reused
    std::vector<std::unique_ptr<DrawSlot>> loadingDraws_;
    std::unique_ptr<QRhiBuffer> loadingCell_;
    // Palette texture: created with the QRhi, before any binding set (every draw
    // samples it), refilled in place when Frame::palette changes.
    std::unique_ptr<QRhiTexture> paletteTex_;
    std::unique_ptr<QRhiSampler> paletteSampler_;
    std::shared_ptr<const HeatmapPalette> uploadedPalette_;
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
    uint64_t reportedBytes_ = UINT64_MAX, reportedCap_ = 0;
    std::vector<SpanSourceKey> uploaded_, missing_;
    std::vector<SpanSourceKey> reportedMissing_; // not reported again while the snapshot still lists them
    bool lost_ = false;
    std::vector<Source *> order_; // reused per frame
    bool uploadPending_ = false;   // upload(): wanted sources left for later frames
    std::vector<std::pair<int64_t, int64_t>> covered_;
    std::vector<uint64_t> drawnScratch_;
    std::vector<Piece> pieces_;                                // reused per picture
    std::vector<HeatmapTileStats::Segment> segmentScratch_;    // reused per frame
    QString lastError_;
    // Live edge: resident buffer sets (refilled in place), the version uploaded
    // into them, and its snapshot (kept for the metadata of the live bins).
    std::vector<std::unique_ptr<LiveSet>> liveSets_;
    std::shared_ptr<const LiveSnapshot> live_;
    uint64_t liveVersion_ = 0, liveSerial_ = 0;
    std::array<LiveSet *, kMaxSpanSources> liveCurrent_{};
    uint8_t liveCount_ = 0;
    int64_t liveTfMs_ = 0, liveStartMs_ = 0, liveEndMs_ = 0;
    uint64_t nextLiveId_ = 1, drawnLiveVersion_ = 0;
    // The replacement paging in (S5L-c review fix 1): drawn once every source is in.
    std::shared_ptr<const LiveSnapshot> pendingLive_;
    std::array<LiveSet *, kMaxSpanSources> pendingSets_{};
    uint8_t pendingCount_ = 0;
    int64_t pendingStartedNs_ = 0;
    uint64_t pendingBytes_ = 0;

    void noteError(const QString &error);
    void releaseAll(bool reportLoss, bool inRhiCleanup = false);
    void attachRhi(QRhi *rhi);
    void reindex();
    const SpanRef *spanAt(int64_t tfMs, int64_t tile) const;
    bool wanted(const SpanRef &ref, size_t index) const;
    void upload(QRhiCommandBuffer *cb, uint64_t &budget);
    void captureCells(QRhiCommandBuffer *cb); // tests: Frame::capture
    bool admit(Source &source); // cap check before creating a source's buffers
    void evictDown(uint64_t target, const SpanRank *incoming);
    void freeSource(Source &source, bool report);
    Bin *binFor(const SpanRef &ref, int64_t tickUnits, QRhiCommandBuffer *cb, bool *complete);
    void updateLive(QRhiCommandBuffer *cb, uint64_t &budget); // a new version: page into free sets
    bool startLive(const std::shared_ptr<const LiveSnapshot> &snap);
    bool stepLive(QRhiCommandBuffer *cb, uint64_t &budget);
    void cancelPendingLive();
    bool liveSetInUse(const LiveSet *set) const;
    bool overCap() const;
    Bin *liveBinFor(int64_t tickUnits, QRhiCommandBuffer *cb); // the live window at the tick (target)
    void trimLiveSets();
    void layout(const std::vector<Draw> &draws); // pieces_ of one picture (draw clip)
    void noteLiveDrawn(const Bin &bin);
    bool binRows(Bin &bin, QRhiCommandBuffer *cb);
    bool rowsCover(const Bin &bin) const;
    Bin *findBin(uint64_t id) const;
    void retire(bool keepDrawn);
    void pinSources(const Bin &bin);
    uint64_t residentBytes() const; // sources, bins and spare bin buffers
    uint64_t spareBytes() const;
    void report();
    bool ensurePipeline(QRhiRenderPassDescriptor *pass, int samples);
    bool ensurePalette(QRhiCommandBuffer *cb);
    const HeatmapPalette &palette() const;
    bool addBinDraw(QRhiResourceUpdateBatch *updates, Bin &bin, float opacity, int64_t loMs, int64_t hiMs);
    bool addLoadingDraw(QRhiResourceUpdateBatch *updates, int64_t loMs, int64_t hiMs);
    bool addLoadingSlot();
    bool subRect(int64_t firstBucket, int64_t endBucket, int64_t tfMs, QRectF *rect, ViewWindow *sub) const;
    std::unique_ptr<QRhiBuffer> cellBuffer(uint64_t bytes, bool live);
};
} // namespace heatmap::gpu
