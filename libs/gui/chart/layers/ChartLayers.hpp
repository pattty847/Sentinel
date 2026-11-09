#pragma once
#include "chart/layers/TradeLayers.hpp"
#include "chart/layers/FootprintCellsLayer.hpp"
#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/ChartTextLayer.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/feeds/BookLadderFeed.hpp"
#include "chart/layers/ChartLayerSet.hpp"
namespace chart {
struct ChartLayers {
    HeatmapLayer heatmap;
    FootprintCellsLayer footprintCells;
    FootprintLayer footprint{&footprintCells.presentation()};
    VolumeProfileLayer vp;
    TpoLayer tpo;
    LadderLayer ladder;
    ChartTextLayer text{heatmap, ladder, footprintCells};
    LayerRegistry registry;
    BookLadderFeed ladderFeed{ladder.mailbox()};
    FlowFeed flowFeed{footprintCells.mailbox()};
    ChartLayers() { registerChartLayers(registry, *this); }
};
} // namespace chart
