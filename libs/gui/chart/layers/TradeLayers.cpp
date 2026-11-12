#include "chart/layers/TradeLayers.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/CandlePixelGeometry.hpp"

namespace chart {
void VolumeProfileLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    renderer.drainPending(bins_, snap_);
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(content(snapshot, context), frame.overlays.volumeProfile && !bins_.empty(), frame.surfaceBounds,
                    frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, bins_,
                    snap_, frame.surfaceDpr, snap.y);
}
void TpoLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(context.window, content(snapshot, context), frame.overlays.tpo, context.atlas, context.atlasReady,
                    frame.raster, frame.surfaceDpr, snap.x, snap.y, frame.surfaceBounds);
}
} // namespace chart
