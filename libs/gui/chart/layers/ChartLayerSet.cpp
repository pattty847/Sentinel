#include "chart/layers/ChartLayers.hpp"
namespace chart {
void registerChartLayers(LayerRegistry& registry, ChartLayers& owned) {
    registry.add(owned.heatmap, FrameProfiler::Uploads);
    registry.add(owned.footprintCells, FrameProfiler::Overlays);
    registry.add(owned.vp, FrameProfiler::Overlays);
    registry.add(owned.ladder, FrameProfiler::Overlays);
    registry.add(owned.cvd, FrameProfiler::Overlays);
    registry.add(owned.flowFooter, FrameProfiler::Overlays);
    registry.add(owned.text, FrameProfiler::TextEnd);
}
} // namespace chart
