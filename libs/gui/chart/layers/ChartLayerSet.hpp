#pragma once
#include "chart/layers/LayerRegistry.hpp"

namespace chart {
struct ChartLayers;
void registerChartLayers(LayerRegistry& registry, ChartLayers& owned);
} // namespace chart
