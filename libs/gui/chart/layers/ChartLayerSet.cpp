#include "chart/layers/ChartLayers.hpp"
namespace chart {
void registerChartLayers(LayerRegistry& registry, ChartLayers& owned) {
    registry.add(owned.heatmap, FrameProfiler::Uploads);
    registry.add(owned.footprintCells, FrameProfiler::Overlays);
    registry.add(owned.vp, FrameProfiler::Overlays, LayerPlane::Profiles);
    registry.add(owned.tpo, FrameProfiler::Overlays, LayerPlane::Profiles);
    registry.add(owned.ladder, FrameProfiler::Overlays, LayerPlane::Foreground);
    registry.add(owned.cvd, FrameProfiler::Overlays, LayerPlane::Foreground);
    registry.add(owned.flowFooter, FrameProfiler::Overlays, LayerPlane::Foreground);
    registry.add(owned.text, FrameProfiler::TextEnd, LayerPlane::Foreground);
}
} // namespace chart
