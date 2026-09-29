#include "HeatmapRenderNode.hpp"
#include "SentinelLogging.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <rhi/qrhi.h>
#include <chrono>

namespace heatmap::gpu {
HeatmapRenderNode::HeatmapRenderNode(std::shared_ptr<HeatmapRenderStats> stats)
    : stats_(stats ? std::move(stats) : std::make_shared<HeatmapRenderStats>()) {}
HeatmapRenderNode::~HeatmapRenderNode() { releaseResources(); }

void HeatmapRenderNode::releaseResources() {
    binner_.reset();
    rhi_ = nullptr;
    drawable_ = false;
}

void HeatmapRenderNode::noteError(const QString &error) {
    stats_->errors.fetch_add(1);
    // Warn on each distinct failure, not once per frame.
    if (error != lastError_) sLog_Warning("heatmap gpu node: " << error);
    lastError_ = error;
}

double HeatmapRenderNode::displayTickFor(const GpuSource &source, const ViewWindow &view, const TickPolicy &policy) {
    const double common = commonTick(source);
    if (!(common > 0)) return 0;
    if (policy.manualTick > 0 && std::abs(policy.manualTick / common - std::round(policy.manualTick / common)) < 1e-8)
        return policy.manualTick;
    return heatmap::idealTick(view.priceLo, view.priceHi, std::max(1.0, policy.heightPx), policy.minRowPx, common,
                              source.priceScale);
}

void HeatmapRenderNode::prepare() {
    drawable_ = false;
    QRhiCommandBuffer *cb = commandBuffer();
    QRhiRenderTarget *rt = renderTarget();
    if (!cb || !rt) return;
    QRhi *rhi = rt->rhi();
    if (!binner_ || rhi != rhi_) {
        binner_ = std::make_unique<HeatmapGpuBinner>(rhi);
        rhi_ = rhi;
    }
    stats_->frames.fetch_add(1);
    QString error;
    // A failed pending source (allocation, limits) never stops the active one drawing.
    if (!binner_->setSource(frame_.source, &error)) noteError(error);
    else if (!binner_->uploadStep(cb, frame_.uploadBudgetBytes, &error)) noteError(error);
    stats_->uploadPending.store(binner_->uploadPending());
    binner_->runPrecisionSelfTest(cb); // once per device; resolves within a frame or two
    const auto active = binner_->activeSource();
    if (!active || frame_.rect.isEmpty()) return;
    // Grid and tick always follow the ACTIVE source: while a new timeframe
    // uploads, the old columns keep their true time extent (never stretched)
    // and a tick its native grids can build.
    const int64_t tf = active->tfMs;
    const double tick = displayTickFor(*active, frame_.view, frame_.tick);
    if (!(tick > 0)) return;
    const auto &binned = binner_->binnedGrid();
    if (!binned || !binner_->binnedMatches(active->id, frame_.outputScale) ||
        !gridCovers(*binned, frame_.view, tf, tick)) {
        const auto grid = planGrid(frame_.view, tf, tick);
        if (!grid) return;
        const auto started = std::chrono::steady_clock::now();
        if (binner_->bin(cb, *grid, frame_.outputScale, &error)) {
            stats_->binSubmitMs.store(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            stats_->rebins.fetch_add(1);
            stats_->factor.store(tickFactors(*active, grid->displayTick)[0]);
            stats_->columns.store(grid->columns);
            stats_->rows.store(grid->rows);
            stats_->tick.store(grid->displayTick);
            stats_->preciseKernel.store(binner_->currentKernel() == KernelVariant::Precise);
            sLog_Probe("heatmap.gpu.bin", "source=" << active->id << " tf=" << tf << " cols=" << grid->columns
                       << " rows=" << grid->rows << " tick=" << grid->displayTick);
        } else {
            noteError(error); // keep drawing the previous grid, if any, at its true position
            if (!binner_->binnedGrid()) return;
        }
    }
    if (!binner_->prepareDraw(rt->renderPassDescriptor(), rt->sampleCount(), &error)) return noteError(error);
    auto *updates = rhi->nextResourceUpdateBatch();
    binner_->updateDraw(updates, *projectionMatrix() * *matrix(), frame_.rect,
                        mappingFor(*binner_->binnedGrid(), frame_.view), frame_.style);
    cb->resourceUpdate(updates);
    stats_->gpuFrameMs.store(cb->lastCompletedGpuTime() * 1000.0);
    stats_->gpuBytes.store(binner_->gpuBytes());
    stats_->drawnSourceId.store(binner_->binnedSourceId());
    drawable_ = true;
}

void HeatmapRenderNode::render(const RenderState *state) {
    if (!drawable_ || !binner_ || !binner_->canDraw()) return;
    QRhiCommandBuffer *cb = commandBuffer();
    const QSize size = renderTarget()->pixelSize();
    cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
    if (state && state->scissorEnabled()) {
        const QRect r = state->scissorRect();
        cb->setScissor(QRhiScissor(r.x(), r.y(), r.width(), r.height()));
    } else {
        cb->setScissor(QRhiScissor(0, 0, size.width(), size.height()));
    }
    binner_->recordDraw(cb);
}
} // namespace heatmap::gpu
