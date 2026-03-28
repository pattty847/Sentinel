#pragma once

#include "FrameContext.hpp"

class GridViewState;
class HeatmapStreamState;
class QQuickWindow;
class QElapsedTimer;

namespace FrameContextBuilder {

/// Assemble an immutable FrameContext from the current state of all data sources.
/// Pure data read — no side effects.
FrameContext build(const QRectF& boundingRect,
                   QQuickWindow* window,
                   const QElapsedTimer& heatmapClock,
                   const TimeAuthority& timeAuthority,
                   const HeatmapStreamState* heatmapStream,
                   const GridViewState* viewState,
                   bool heatmapEnabled,
                   bool footprintEnabled,
                   bool tpoEnabled,
                   uint64_t heatmapGen,
                   uint64_t footprintGen,
                   uint64_t candleGen);

} // namespace FrameContextBuilder
