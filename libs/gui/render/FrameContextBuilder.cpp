#include "FrameContextBuilder.hpp"

#include "GridViewState.hpp"

#include <QElapsedTimer>
#include <QQuickWindow>

namespace FrameContextBuilder {

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
    frame.surfaceBounds = boundingRect;
    frame.surfaceDpr = window ? window->effectiveDevicePixelRatio() : 1.0;
    const qint64 steadyNowMs = frameClock.isValid() ? frameClock.elapsed() : 0;
    frame.time = timeAuthority.snapshot(steadyNowMs);
    frame.presentationTimeMs = frame.time.nowPresentationMs;
    frame.overlays.heatmap = heatmapEnabled;
    frame.overlays.footprint = footprintEnabled;
    frame.overlays.tpo = tpoEnabled;
    frame.streamGenerations.footprint = footprintGen;
    frame.streamGenerations.candle = candleGen;
    if (viewState && viewState->isTimeWindowValid()) {
        frame.viewport.valid = true;
        frame.viewport.timeStart = viewState->getVisibleTimeStart();
        frame.viewport.timeEnd = viewState->getVisibleTimeEnd();
        frame.viewport.minPrice = viewState->getMinPrice();
        frame.viewport.maxPrice = viewState->getMaxPrice();
        frame.viewport.panVisualOffset = viewState->getPanVisualOffset();
        frame.viewport.dragging = viewState->isDragging();
        frame.viewport.autoScrollEnabled = viewState->isAutoScrollEnabled();
    }
    return frame;
}

} // namespace FrameContextBuilder
