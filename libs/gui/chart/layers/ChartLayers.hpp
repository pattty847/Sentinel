#pragma once
#include "chart/layers/TradeLayers.hpp"
#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/ChartTextLayer.hpp"
#include "chart/layers/ChartLayerSet.hpp"
namespace chart {
struct ChartLayers {
    HeatmapLayer heatmap;
    FootprintLayer footprint;
    VolumeProfileLayer vp;
    TpoLayer tpo;
    ChartTextLayer text{heatmap};
    LayerRegistry registry;
    ChartLayers() { registerChartLayers(registry, *this); }
};
} // namespace chart
