#include "chart/layers/ChartLayers.hpp"
namespace chart {
void registerChartLayers(LayerRegistry& registry, ChartLayers& owned) {
    registry.add(owned.heatmap, FrameProfiler::Uploads);
    registry.add(owned.footprint, FrameProfiler::Overlays);
    registry.add(owned.vp, FrameProfiler::Overlays);
    registry.add(owned.tpo, FrameProfiler::Overlays);
    registry.add(owned.ladder, FrameProfiler::Overlays);
    registry.add(owned.text, FrameProfiler::TextEnd);
}
} // namespace chart
