#include "HeatmapRenderNode.hpp"
#include "SentinelLogging.hpp"
#include "HeatmapGpuSelfTest.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <rhi/qrhi.h>
#include <chrono>

namespace heatmap::gpu {
HeatmapRenderNode::HeatmapRenderNode(std::shared_ptr<HeatmapRenderStats> stats)
    : stats_(stats ? std::move(stats) : std::make_shared<HeatmapRenderStats>()) {
    prewarmPrecisionSelfTest(); // worker-built fixture, ready before the first source uploads
}
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

double HeatmapRenderNode::displayTickFor(const GpuSource &source, const ViewWindow &view, const TickPolicy &policy,
                                         double currentTick) {
    if (policy.mode == heatmap::TickMode::Manual) {
        // Manual never coarsens: a locked preset draws as is; columns whose
        // native grid cannot build it veil (tickFactors == 0 in the kernel).
        const int64_t units = heatmap::toUnits(policy.manualTick, source.priceScale);
        return heatmap::isPresetUnits(units) ? heatmap::fromUnits(units, source.priceScale) : 0;
    }
    const double common = commonTickInView(source, view.timeLoMs, view.timeHiMs);
    const int64_t commonUnits = heatmap::toUnits(common, source.priceScale);
    const double span = view.priceHi - view.priceLo;
    if (commonUnits <= 0 || !(span > 0) || !std::isfinite(span)) return 0;
    const double unitsPerPx = span * source.priceScale / std::max(1.0, policy.heightPx);
    const int64_t units = heatmap::autoTickUnits(heatmap::toUnits(currentTick, source.priceScale), commonUnits,
                                                 unitsPerPx, {policy.minRowPx, policy.hysteresis});
    return units > 0 ? heatmap::fromUnits(units, source.priceScale) : 0;
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
    binner_->setMemoryCap(frame_.gpuMemoryCapBytes);
    QString error;
    // A failed pending source (allocation, limits) never stops the active one drawing.
    if (!binner_->setSource(frame_.source, &error)) noteError(error);
    else if (!binner_->uploadStep(cb, frame_.uploadBudgetBytes, &error)) noteError(error);
    stats_->uploadPending.store(binner_->uploadPending());
    stats_->refusedSourceId.store(binner_->refusedSourceId());
    binner_->runPrecisionSelfTest(cb); // once per device; resolves within a frame or two
    const auto active = binner_->activeSource();
    if (!active || frame_.rect.isEmpty()) return;
    // Grid and tick always follow the ACTIVE source: while a new timeframe
    // uploads, the old columns keep their true time extent (never stretched)
    // and a tick its native grids can build.
    const int64_t tf = active->tfMs;
    if (frame_.tick.mode == heatmap::TickMode::Manual) autoTick_ = 0;
    const double tick = displayTickFor(*active, frame_.view, frame_.tick, autoTick_);
    stats_->commonTick.store(commonTickInView(*active, frame_.view.timeLoMs, frame_.view.timeHiMs));
    if (!(tick > 0)) return;
    if (frame_.tick.mode == heatmap::TickMode::Auto) autoTick_ = tick;
    const auto &binned = binner_->binnedGrid();
    // Re-bin triggers (spec rule 6): a new source (timeframe, chunks, generation),
    // output scale or kernel; a tick change; or the view leaving the prepared grid.
    // A pan inside the prepared grid only changes the mapping below.
    if (!binned || !binner_->binnedMatches(active->id, frame_.outputScale) ||
        !gridCovers(*binned, frame_.view, tf, tick)) {
        const auto grid = planGrid(frame_.view, tf, tick);
        if (!grid) return;
        const bool tickChange = binned && binned->tfMs == tf && binned->displayTick != tick;
        const bool fade = tickChange && frame_.tick.crossfadeMs > 0;
        const auto started = std::chrono::steady_clock::now();
        if (binner_->bin(cb, *grid, frame_.outputScale, &error, fade)) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            stats_->binSubmitMs.store(ms);
            stats_->rebins.fetch_add(1);
            if (tickChange) {
                stats_->tickChanges.fetch_add(1);
                stats_->tickChangeBinMs.store(ms);
            }
            if (fade) fadeStart_ = std::chrono::steady_clock::now();
            stats_->factor.store(tickFactors(*active, grid->displayTick)[0]);
            stats_->columns.store(grid->columns);
            stats_->rows.store(grid->rows);
            stats_->tick.store(grid->displayTick);
            stats_->preciseKernel.store(binner_->currentKernel() == KernelVariant::Precise);
            sLog_Probe("heatmap.gpu.bin", "source=" << active->id << " tf=" << tf << " cols=" << grid->columns
                       << " rows=" << grid->rows << " tick=" << grid->displayTick << " tickChange=" << tickChange);
        } else {
            noteError(error); // keep drawing the previous grid, if any, at its true position
            if (!binner_->binnedGrid()) return;
        }
    }
    if (!binner_->prepareDraw(rt->renderPassDescriptor(), rt->sampleCount(), &error)) return noteError(error);
    auto *updates = rhi->nextResourceUpdateBatch();
    const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
    binner_->updateDraw(updates, mvp, frame_.rect, mappingFor(*binner_->binnedGrid(), frame_.view), frame_.style);
    // Crossfade: the new grid draws at full opacity, the previous one over it
    // fading out (a linear blend wherever the previous grid is opaque). Both keep
    // their absolute anchoring, so a pan or zoom during the fade stays aligned.
    if (const auto &previous = binner_->previousGrid()) {
        const double t = frame_.tick.crossfadeMs > 0 ?
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fadeStart_).count() /
                frame_.tick.crossfadeMs : 1.0;
        if (t >= 1.0 || previous->tfMs != binner_->binnedGrid()->tfMs) binner_->dropPrevious();
        else {
            DrawStyle style = frame_.style;
            style.opacity = float(1.0 - t) * frame_.style.opacity;
            binner_->updatePreviousDraw(updates, mvp, frame_.rect, mappingFor(*previous, frame_.view), style);
        }
    }
    stats_->crossfading.store(binner_->previousGrid().has_value());
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
