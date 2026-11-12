#include "chart/layers/TradeLayers.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/CandlePixelGeometry.hpp"
#include <cmath>

namespace chart {
namespace {
bool hasVisibleProfileData(const std::vector<float>& bins, const VolumeProfileState::Snapshot& snap,
                           const FrameContext& frame) {
    if (!frame.overlays.volumeProfile || !(snap.tickSize > 0) || !std::isfinite(snap.tickSize)) return false;
    if (snap.sessionStartMs > 0 && snap.sessionEndMs > snap.sessionStartMs &&
        (double(snap.sessionStartMs) >= frame.raster.drawnEndMs ||
         double(snap.sessionEndMs) <= frame.raster.drawnStartMs)) return false;
    const double high = snap.minPrice + double(bins.size()) * snap.tickSize;
    for (size_t i = 0; i < bins.size(); ++i) {
        const double top = high - double(i) * snap.tickSize;
        if (top <= frame.mapping.viewMinPrice || top - snap.tickSize >= frame.mapping.viewMaxPrice) continue;
        if (std::isfinite(bins[i]) && bins[i] > 0) return true;
    }
    return false;
}
}
void VolumeProfileLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const bool dataChanged = renderer.drainPending(bins_, snap_);
    const VisibleKey key{frame.raster.drawnStartMs, frame.raster.drawnEndMs,
                         frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice,
                         frame.overlays.volumeProfile};
    if (dataChanged || !visibleKeyValid_ || !(key == visibleKey_)) {
        visibleKey_ = key; visibleKeyValid_ = true;
        visibleData_ = hasVisibleProfileData(bins_, snap_, frame);
    }
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(content(snapshot, context), frame.overlays.volumeProfile && visibleData_, frame.surfaceBounds,
                    frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, bins_,
                    snap_, frame.surfaceDpr, snap.y);
}
QString VolumeProfileLayer::emptyStateText(const ChartFrame& frame) const {
    if (!frame.context.overlays.volumeProfile || !frame.context.raster.valid || visibleData_ || snap_.sessionStartMs <= 0 ||
        snap_.sessionEndMs <= snap_.sessionStartMs || snap_.gridHeight <= 0) return {};
    return QStringLiteral("Volume profile: no trade data for this window");
}
void TpoLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(context.window, content(snapshot, context), frame.overlays.tpo, context.atlas, context.atlasReady,
                    frame.raster, frame.surfaceDpr, snap.x, snap.y, frame.surfaceBounds);
}
} // namespace chart
