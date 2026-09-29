#pragma once
// Scene-graph host for HeatmapGpuBinner. Lives in the ordinary QSG tree and draws
// in line with its siblings (no offscreen texture). Per frame:
//   prepare(): page source upload within the byte budget; choose the display tick
//              (TickPolicy: Auto with hysteresis, or Manual); re-bin with a compute
//              pass only on the spec rule 6 triggers (tick, source, output scale,
//              kernel, or the view leaving the prepared grid); update the draw
//              mapping (a pan inside the prepared grid = mapping change only).
//   render():  one triangle strip over the item rect, inside the main pass.
// Mechanism verified by tests/render/test_qsg_compute_spike.cpp (S4 spike).
#include "HeatmapGpuBinner.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <QSGRenderNode>
#include <atomic>
#include <chrono>
#include <memory>

namespace heatmap::gpu {

// Written on the render thread, read anywhere (telemetry and upload pacing).
struct HeatmapRenderStats {
    std::atomic<uint64_t> frames{0}, rebins{0}, errors{0};
    std::atomic<uint64_t> refusedSourceId{0}; // source the binner refused or is backing off
    std::atomic<bool> uploadPending{false};
    std::atomic<uint64_t> drawnSourceId{0}, gpuBytes{0};
    std::atomic<double> binSubmitMs{0}, gpuFrameMs{0};
    std::atomic<uint32_t> columns{0}, rows{0}, factor{0};
    std::atomic<double> tick{0};             // tick of the binned grid
    std::atomic<double> commonTick{0};       // commonTick() of the active source's data in view
    std::atomic<uint64_t> tickChanges{0};    // re-bins caused by a tick change
    std::atomic<double> tickChangeBinMs{0};  // CPU submit time of the last tick-change re-bin
    std::atomic<bool> crossfading{false};
    std::atomic<bool> preciseKernel{true}; // false once the fast kernel passed its self-test
};

class HeatmapRenderNode final : public QSGRenderNode {
public:
    // The display tick is chosen per frame for the source actually drawn (the
    // active one, which may be older than `source` while it uploads).
    struct TickPolicy {
        heatmap::TickMode mode = heatmap::TickMode::Auto;
        // Manual: the locked preset, drawn even where some visible data cannot
        // build it (those columns veil; the tick never coarsens silently).
        double manualTick = 0;
        double minRowPx = 2;     // Auto: smallest preset at least this tall ...
        double hysteresis = 0.25; // ... with this hysteresis h (spec rule 2)
        double heightPx = 0;     // target height in physical pixels
        double crossfadeMs = 0;  // > 0: crossfade old and new grids at a tick change
    };
    struct Frame {
        std::shared_ptr<const GpuSource> source; // newest wanted source; may still be uploading
        ViewWindow view;
        TickPolicy tick;
        QRectF rect;                             // item-space draw rect
        recording::SizeScale outputScale;
        uint64_t uploadBudgetBytes = 2ull << 20; // per frame
        uint64_t gpuMemoryCapBytes = HeatmapGpuBinner::kDefaultMemoryCapBytes; // both source buffer sets
        DrawStyle style;
    };
    explicit HeatmapRenderNode(std::shared_ptr<HeatmapRenderStats> stats = {});
    ~HeatmapRenderNode() override;
    // Call from QQuickItem::updatePaintNode (render thread, GUI thread blocked).
    void setFrame(Frame frame) { frame_ = std::move(frame); }
    // Tick for `source` at `view`. Auto is stateful: `currentTick` is the tick
    // drawn now (0 = evaluate fresh). Auto uses commonTick() of the data in view.
    static double displayTickFor(const GpuSource &source, const ViewWindow &view, const TickPolicy &policy,
                                 double currentTick = 0);

    void prepare() override;
    void render(const RenderState *state) override;
    void releaseResources() override;
    StateFlags changedStates() const override { return ViewportState | ScissorState; }
    RenderingFlags flags() const override { return BoundedRectRendering | NoExternalRendering; }
    QRectF rect() const override { return frame_.rect; }

private:
    std::shared_ptr<HeatmapRenderStats> stats_;
    std::unique_ptr<HeatmapGpuBinner> binner_;
    QRhi *rhi_ = nullptr;
    Frame frame_;
    bool drawable_ = false;
    double autoTick_ = 0;   // Auto state; reset in Manual so Auto resumes fresh
    std::chrono::steady_clock::time_point fadeStart_;
    QString lastError_;
    void noteError(const QString &error);
};
} // namespace heatmap::gpu
