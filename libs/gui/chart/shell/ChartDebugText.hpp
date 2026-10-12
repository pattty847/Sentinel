#pragma once

#include <QString>

struct TimeAxisMapping;
namespace heatmap::gpu { class HeatmapGpuLayer; }

namespace chart {
QString formatViewportMath(const TimeAxisMapping& mapping, bool dragging);
QString formatDataPipeline(const heatmap::gpu::HeatmapGpuLayer* layer);
}
