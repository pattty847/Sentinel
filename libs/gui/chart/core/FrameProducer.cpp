#include "chart/core/FrameProducer.hpp"

namespace chart {

void FrameProducer::publishFrameContext(const FrameContext& frame, PublishedFrame& destination) {
    MappingFrameContext published;
    published.surfaceBounds = frame.surfaceBounds;
    published.surfaceDpr = frame.surfaceDpr;
    published.presentationTimeMs = frame.presentationTimeMs;
    published.activeTimeframeMs = frame.time.activeTimeframeMs;
    published.nowEventTimeMs = frame.time.nowEventMs;
    published.currentBoundaryStartMs = frame.time.currentBoundaryStartMs;
    published.nextBoundaryStartMs = frame.time.nextBoundaryStartMs;
    published.boundarySequence = frame.time.boundarySequence;
    published.hasEventTime = frame.time.hasEvent;
    published.viewportValid = frame.viewport.valid;
    published.viewportTimeStart = frame.viewport.timeStart;
    published.viewportTimeEnd = frame.viewport.timeEnd;
    published.viewportMinPrice = frame.viewport.minPrice;
    published.viewportMaxPrice = frame.viewport.maxPrice;
    published.viewportPanVisualOffset = frame.viewport.panVisualOffset;
    published.viewportDragging = frame.viewport.dragging;
    published.viewportAutoScrollEnabled = frame.viewport.autoScrollEnabled;
    published.mapping = frame.mapping;
    published.raster = frame.raster;
    destination.publish(published);
}

void FrameProducer::computeGpuFrameMapping(const FrameInputs& inputs, ITickAuthority& ticks, PublishedFrame& published) {
    auto& frame = frame_.context;
    const double tick = ticks.tickPrice();
    auto in = inputs.camera.raster;
    in.tick = tick;
    const chart_raster::RasterStep previous = published.step();
    // The rest camera (whole pixels), and what this frame draws: the rest camera, or a
    // free one during a zoom glide or gesture (fractional, drawn with coverage).
    const chart::Cameras cameras = chart::camerasFor({in, previous, inputs.camera.glide});
    frame_.rest = cameras.rest;
    frame.raster = cameras.drawn;
    frame.mapping = chart_raster::toMapping(frame.raster);
    frame_.coverageGamma = cameras.drawn.valid && cameras.drawn.free ? 2.2f : 0.0f;
    frame_.binView = cameras.binView;
    const auto& cam = cameras.rest;
    if (!cam.valid) return;
    if (cam.step() != previous) published.setStep(cam.step());
    const double priceSpan = in.maxPrice - in.minPrice;
    ticks.noteRaster(cam.rowPx, cam.colPx, priceSpan > 0 ? double(cam.heightDev) * cam.tick / priceSpan : 0.0);
    const RasterSurface surface{cam.step(), cam.dpr, cam.tick, cam.widthDev, cam.heightDev};
    if (surface != announced_) {
        announced_ = surface;
        frame_.rasterChanged = true;
    }
}

const ChartFrame& FrameProducer::produce(const FrameInputs& inputs, ITickAuthority& ticks, PublishedFrame& published) {
    frame_ = {};
    auto& frame = frame_.context;
    frame = inputs.context;
    // Choose the tick for the continuous view (committed + an active drag at the
    // continuous scale): Auto decides on the continuous camera.
    const auto& vp = frame.viewport;
    const double timeSpan = double(vp.timeEnd - vp.timeStart), priceSpan = vp.maxPrice - vp.minPrice;
    const QRectF& bounds = frame.surfaceBounds;
    const bool viewValid = vp.valid && inputs.timeframeMs > 0 && timeSpan > 0 && priceSpan > 0 && !bounds.isEmpty();
    frame_.drawHeatmap = frame.overlays.heatmap && viewValid;
    if (frame_.drawHeatmap) {
        const QPointF drag = vp.dragging ? vp.panVisualOffset : QPointF();
        const double dt = -drag.x() * timeSpan / bounds.width(), dp = drag.y() * priceSpan / bounds.height();
        ticks.chooseTickForView({double(vp.timeStart) + dt, double(vp.timeEnd) + dt, vp.minPrice + dp,
                                       vp.maxPrice + dp});
    }
    // Publish the raster camera and frame mapping at that tick, so rows follow
    // the drawn tick in heatmapTickSize and PriceAxisModel.
    computeGpuFrameMapping(inputs, ticks, published);
    publishFrameContext(frame, published);
    frame_.ready = true;
    return frame_;
}

} // namespace chart
