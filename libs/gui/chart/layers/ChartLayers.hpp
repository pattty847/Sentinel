#pragma once
#include "chart/layers/TradeLayers.hpp"
#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/ChartTextLayer.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/feeds/BookLadderFeed.hpp"
#include "chart/layers/ChartLayerSet.hpp"
namespace chart {
struct ChartLayers {
    HeatmapLayer heatmap;
    FootprintLayer footprint;
    VolumeProfileLayer vp;
    TpoLayer tpo;
    LadderLayer ladder;
    ChartTextLayer text{heatmap};
    LayerRegistry registry;
    BookLadderFeed ladderFeed{ladder.mailbox()};
    ChartLayers() { registerChartLayers(registry, *this); }
};
} // namespace chart
