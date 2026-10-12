#pragma once
#include "chart/layers/LayerRegistry.hpp"

namespace chart {
void registerChartLayers(LayerRegistry& registry, IChartLayer& footprint, IChartLayer& vp, IChartLayer& tpo);
} // namespace chart
