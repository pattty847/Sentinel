#include "chart/layers/TradeLayers.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/CandlePixelGeometry.hpp"
#include <cmath>
#include <utility>

namespace chart {
namespace {
std::pair<bool, bool> profileVolumePresence(const std::vector<float>& bins, const VolumeProfileState::Snapshot& snap,
                                            const FrameContext& frame) {
    if (!(snap.tickSize > 0) || !std::isfinite(snap.tickSize)) return {false, false};
    const double high = snap.minPrice + double(bins.size()) * snap.tickSize;
    bool any = false, visible = false;
    for (size_t i = 0; i < bins.size(); ++i) {
        if (!std::isfinite(bins[i]) || bins[i] <= 0) continue;
        any = true;
        const double top = high - double(i) * snap.tickSize;
        if (top > frame.mapping.viewMinPrice && top - snap.tickSize < frame.mapping.viewMaxPrice)
            visible = true;
    }
    return {any, visible};
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
        const auto [any, visible] = profileVolumePresence(bins_, snap_, frame);
        anyData_ = any; visibleData_ = visible;
    }
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(content(snapshot, context), frame.overlays.volumeProfile && !bins_.empty(), frame.surfaceBounds,
                    frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, bins_,
                    snap_, frame.surfaceDpr, snap.y);
}
QString VolumeProfileLayer::emptyStateText(const ChartFrame& frame) const {
    if (!frame.context.overlays.volumeProfile || !frame.context.raster.valid ||
        snap_.sessionStartMs <= 0 || snap_.sessionEndMs <= snap_.sessionStartMs || snap_.gridHeight <= 0 ||
        visibleData_) return {};
    const auto& cam = frame.context.raster;
    if (double(snap_.sessionStartMs) >= cam.drawnEndMs || double(snap_.sessionEndMs) <= cam.drawnStartMs)
        return QStringLiteral("Volume profile: current session only");
    return anyData_ ? QString{} : QStringLiteral("Volume profile: no trade data for this window");
}
void TpoLayer::updateNodes(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const auto snap = candle_pixels::axisSnap(frame.raster);
    renderer.render(context.window, content(snapshot, context), frame.overlays.tpo, context.atlas, context.atlasReady,
                    frame.raster, frame.surfaceDpr, snap.x, snap.y, frame.surfaceBounds);
}
} // namespace chart
