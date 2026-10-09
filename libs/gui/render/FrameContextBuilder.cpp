#include "FrameContextBuilder.hpp"

#include "ChartRaster.hpp"
#include "GridViewState.hpp"

#include <QElapsedTimer>
#include <QQuickWindow>

namespace FrameContextBuilder {

FrameViewportSnapshot viewportSnapshot(const GridViewState* viewState) {
    FrameViewportSnapshot viewport;
    if (!viewState || !viewState->isTimeWindowValid()) return viewport;
    viewport.valid = true;
    viewport.timeStart = viewState->getVisibleTimeStart();
    viewport.timeEnd = viewState->getVisibleTimeEnd();
    viewport.minPrice = viewState->getMinPrice();
    viewport.maxPrice = viewState->getMaxPrice();
    viewport.panVisualOffset = viewState->getPanVisualOffset();
    viewport.dragging = viewState->isDragging();
    viewport.autoScrollEnabled = viewState->isAutoScrollEnabled();
    viewport.anchorFracX = viewState->rasterAnchor().fracX;
    viewport.anchorFracY = viewState->rasterAnchor().fracY;
    return viewport;
}

FrameContext build(const QRectF& boundingRect,
                   QQuickWindow* window,
                   const QElapsedTimer& frameClock,
                   const TimeAuthority& timeAuthority,
                   const GridViewState* viewState,
                   bool heatmapEnabled,
                   bool footprintEnabled,
                   bool tpoEnabled,
                   uint64_t footprintGen,
                   uint64_t candleGen) {
    FrameContext frame;
    frame.surfaceDpr = window ? window->effectiveDevicePixelRatio() : 1.0;
    // The whole device pixels of the item (the raster camera's surface): every layer
    // that interpolates the view over surfaceBounds stays on the camera's slopes.
    const double dpr = frame.surfaceDpr > 0 ? frame.surfaceDpr : 1.0;
    frame.surfaceBounds = QRectF(boundingRect.topLeft(),
                                 QSizeF(chart_raster::devicePixels(boundingRect.width(), dpr) / dpr,
                                        chart_raster::devicePixels(boundingRect.height(), dpr) / dpr));
    const qint64 steadyNowMs = frameClock.isValid() ? frameClock.elapsed() : 0;
    frame.time = timeAuthority.snapshot(steadyNowMs);
    frame.presentationTimeMs = frame.time.nowPresentationMs;
    frame.overlays.heatmap = heatmapEnabled;
    frame.overlays.footprint = footprintEnabled;
    frame.overlays.tpo = tpoEnabled;
    frame.streamGenerations.footprint = footprintGen;
    frame.streamGenerations.candle = candleGen;
    frame.viewport = viewportSnapshot(viewState);
    return frame;
}

} // namespace FrameContextBuilder
