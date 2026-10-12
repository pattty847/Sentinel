#include "chart/layers/TradeLayers.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/CandlePixelGeometry.hpp"

namespace chart {
void FootprintLayer::sync(const ChartFrame&) {
    renderer.drainPending(uploads_);
}
void FootprintLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    renderer.render(context.window, context.root, frame.overlays.footprint,
                    frame.mapping.viewStartMs, frame.mapping.viewEndMs,
                    frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, frame.surfaceBounds, uploads_);
    uploads_.clear();
}
void VolumeProfileLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    std::vector<float> localBins;
    VolumeProfileState::Snapshot localSnap;
    renderer.drainPending(localBins, localSnap);
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(context.root, frame.overlays.volumeProfile && !localBins.empty(), frame.surfaceBounds,
                    frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, localBins,
                    localSnap, frame.surfaceDpr, snap.y);
}
void TpoLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(context.window, context.root, frame.overlays.tpo, context.atlas, context.atlasReady,
                    frame.raster, frame.surfaceDpr, snap.x, snap.y, frame.surfaceBounds);
}
} // namespace chart
