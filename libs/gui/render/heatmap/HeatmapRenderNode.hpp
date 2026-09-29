#pragma once
// Scene-graph host for HeatmapGpuBinner. Lives in the ordinary QSG tree and draws
// in line with its siblings (no offscreen texture). Per frame:
//   prepare(): page source upload within the byte budget, re-bin with a compute
//              pass only when the source or the absolute bin grid changed, update
//              the draw mapping (sub-bin pan = mapping change only).
//   render():  one triangle strip over the item rect, inside the main pass.
// Mechanism verified by tests/render/test_qsg_compute_spike.cpp (S4 spike).
#include "HeatmapGpuBinner.hpp"
#include <QSGRenderNode>
#include <atomic>
#include <memory>

namespace heatmap::gpu {

// Written on the render thread, read anywhere (telemetry and upload pacing).
struct HeatmapRenderStats {
    std::atomic<uint64_t> frames{0}, rebins{0}, errors{0};
    std::atomic<bool> uploadPending{false};
    std::atomic<uint64_t> drawnSourceId{0}, gpuBytes{0};
    std::atomic<double> binSubmitMs{0}, gpuFrameMs{0};
    std::atomic<uint32_t> columns{0}, rows{0}, factor{0};
    std::atomic<double> tick{0};
    std::atomic<bool> preciseKernel{true}; // false once the fast kernel passed its self-test
};

class HeatmapRenderNode final : public QSGRenderNode {
public:
    // The display tick is chosen per frame for the source actually drawn (the
    // active one, which may be older than `source` while it uploads).
    struct TickPolicy {
        double manualTick = 0;   // used when > 0 and a multiple of commonTick(active)
        double minRowPx = 2;     // otherwise: smallest ladder tick at least this tall
        double heightPx = 0;     // target height in physical pixels
    };
    struct Frame {
        std::shared_ptr<const GpuSource> source; // newest wanted source; may still be uploading
        ViewWindow view;
        TickPolicy tick;
        QRectF rect;                             // item-space draw rect
        recording::SizeScale outputScale;
        uint64_t uploadBudgetBytes = 2ull << 20; // per frame
        DrawStyle style;
    };
    explicit HeatmapRenderNode(std::shared_ptr<HeatmapRenderStats> stats = {});
    ~HeatmapRenderNode() override;
    // Call from QQuickItem::updatePaintNode (render thread, GUI thread blocked).
    void setFrame(Frame frame) { frame_ = std::move(frame); }
    static double displayTickFor(const GpuSource &source, const ViewWindow &view, const TickPolicy &policy);

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
    QString lastError_;
    void noteError(const QString &error);
};
} // namespace heatmap::gpu
