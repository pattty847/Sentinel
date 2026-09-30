#pragma once
// Scene-graph host for whole-chunk render-ready heatmap tiles (slice B1, mode W).
// The controller (GUI thread) owns the tile cache policy: it names every tile it
// keeps (Frame::tiles) and the tile slots in view (Frame::visible). This node
// (render thread) mirrors that set on the GPU:
//   prepare(): frees tiles the controller dropped; makes missing tiles resident
//              within the per-frame upload budget, visible ones first:
//              - CPU tiles: upload their render-ready cells;
//              - GPU tiles: page the tile's GpuSource into a HeatmapGpuBinner and
//                bin the WHOLE tile grid into the tile's own buffer (binInto);
//              then records one quad per visible slot. Pan and zoom only change
//              the per-tile draw mapping: there is no re-bin on any pan or zoom.
//   render():  the quads, inside the main pass.
// Transitions (spec rules 7 and 8): when the target (timeframe, tick) changes, the
// last completely drawn set keeps drawing until every visible tile of the new
// target is resident; a tick change then crossfades the old set out over
// crossfadeMs. A revised tile draws its previous version (slot fallback) until
// the replacement is resident. Unprepared available time draws the loading hatch.
#include "HeatmapGpuBinner.hpp"
#include <QSGRenderNode>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace heatmap::gpu {

struct TileRef {
    uint64_t id = 0; // unique per built tile (content + tick + generation)
    BinGrid grid;    // absolute tile grid; rows include the two sentinel rows
    std::shared_ptr<const std::vector<uint32_t>> cells; // CPU-built cells, or
    std::shared_ptr<const GpuSource> source;            // the tile's source, binned on the GPU
    // Hybrid (B1): the source stays resident on the GPU (one per tile span, shared
    // by every tick) and the node bins only the rows around the view (grid.rows
    // is ignored), re-binning in place when the view leaves them.
    bool viewRows = false;
    uint64_t cellBytes() const { return uint64_t(grid.columns) * grid.rows * 4; }
};
struct TileSlot {
    int64_t firstBucket = 0, endBucket = 0; // time span of the slot (absolute buckets)
    uint64_t primary = 0;  // tile wanted here (0 = not built yet)
    uint64_t fallback = 0; // older version drawn until the primary is resident (0 = none)
    bool expected = true;  // available time: draw the loading hatch while nothing is resident
};

// Written on the render thread, read anywhere.
struct HeatmapTileStats {
    std::atomic<uint64_t> frames{0}, tilesUploaded{0}, tilesBinned{0}, uploadBytes{0}, errors{0};
    std::atomic<uint64_t> residentTiles{0}, residentBytes{0}, gpuBytes{0}, binnerBytes{0};
    std::atomic<uint64_t> rebinnedTiles{0}; // hybrid: in-place re-bins after the view left a tile's rows
    std::atomic<uint32_t> slotCount{0}, drawnPrimary{0}, drawnFallback{0}, loadingSlots{0};
    std::atomic<bool> complete{false}, holding{false}, crossfading{false};
    std::atomic<uint64_t> drawnKey{0};
    std::atomic<double> prepareMs{0}, lastTileMs{0}, gpuFrameMs{0};
    std::atomic<bool> preciseKernel{true};
    // Tiles drawn in the last frame (held or fading sets included): the
    // controller must not evict them.
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
    struct Frame {
        std::vector<TileRef> tiles;  // every tile the controller keeps; others are freed
        std::vector<TileSlot> visible; // tile slots in view
        uint64_t key = 0;            // target (timeframe, tick); a change is a transition
        int64_t tfMs = 0;
        ViewWindow view;
        QRectF rect;
        recording::SizeScale outputScale;
        uint64_t uploadBudgetBytes = 8ull << 20;
        uint64_t binnerMemoryCapBytes = HeatmapGpuBinner::kDefaultMemoryCapBytes;
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

private:
    struct GpuTile;
    struct DrawSlot;
    struct Draw { uint64_t id; int64_t firstBucket, endBucket; int64_t tfMs; };
    std::shared_ptr<HeatmapTileStats> stats_;
    Frame frame_;
    QRhi *rhi_ = nullptr;
    std::unique_ptr<HeatmapGpuBinner> binner_;
    std::unordered_map<uint64_t, std::unique_ptr<GpuTile>> tiles_;
    std::vector<std::unique_ptr<DrawSlot>> loadingDraws_;
    std::unique_ptr<QRhiBuffer> loadingCell_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QVector<quint32> pipelineFormat_;
    int pipelineSamples_ = 0;
    uint64_t drawnKey_ = 0;
    int64_t drawnTfMs_ = 0;
    std::vector<Draw> held_, fading_;
    std::chrono::steady_clock::time_point fadeStart_;
    std::vector<std::pair<QRhiShaderResourceBindings *, float>> tileDraws_; // this frame: block bindings, opacity
    size_t loadingUsed_ = 0;
    uint64_t pendingGpuTile_ = 0; // tile whose source is uploading in the binner
    QString lastError_;
    void noteError(const QString &error);
    void releaseAll();
    bool ensureResident(const TileRef &ref, QRhiCommandBuffer *cb, uint64_t &budget, bool &gpuBusy);
    bool binBlocks(GpuTile &tile, const TileRef &ref, QRhiCommandBuffer *cb, bool viewRows);
    bool viewRowsCovered(const GpuTile &tile) const;
    bool ensurePipeline(QRhiRenderPassDescriptor *pass, int samples);
    void addTileDraw(QRhiResourceUpdateBatch *updates, GpuTile &tile, int64_t firstBucket, int64_t endBucket,
                     int64_t tfMs, float opacity);
    void addLoadingDraw(QRhiResourceUpdateBatch *updates, int64_t firstBucket, int64_t endBucket, int64_t tfMs);
    bool addLoadingSlot();
    bool subRect(int64_t firstBucket, int64_t endBucket, int64_t tfMs, QRectF *rect, ViewWindow *sub) const;
};
} // namespace heatmap::gpu
