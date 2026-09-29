#include "HeatmapRenderNode.hpp"
#include "SentinelLogging.hpp"
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
    drawable_ = false;
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
    if (!binner_->setSource(frame_.source, &error) ||
        !binner_->uploadStep(cb, frame_.uploadBudgetBytes, &error)) return noteError(error);
    stats_->uploadPending.store(binner_->uploadPending());
    const auto active = binner_->activeSource();
    if (!active || !(frame_.displayTick > 0) || frame_.rect.isEmpty()) return;
    // The grid always uses the ACTIVE source's timeframe: while a new timeframe
    // uploads, the old columns keep their true time extent (never stretched).
    const int64_t tf = active->tfMs;
    const auto &binned = binner_->binnedGrid();
    if (!binned || binner_->binnedSourceId() != active->id ||
        !gridCovers(*binned, frame_.view, tf, frame_.displayTick)) {
        const auto grid = planGrid(frame_.view, tf, frame_.displayTick);
        if (!grid) return;
        const auto started = std::chrono::steady_clock::now();
        if (!binner_->bin(cb, *grid, frame_.outputScale, &error)) return noteError(error);
        stats_->binSubmitMs.store(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        stats_->rebins.fetch_add(1);
        const auto factors = tickFactors(*active, grid->displayTick);
        stats_->factor.store(factors[0]);
        stats_->columns.store(grid->columns);
        stats_->rows.store(grid->rows);
        stats_->tick.store(grid->displayTick);
        sLog_Probe("heatmap.gpu.bin", "source=" << active->id << " tf=" << tf << " cols=" << grid->columns
                   << " rows=" << grid->rows << " tick=" << grid->displayTick);
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
