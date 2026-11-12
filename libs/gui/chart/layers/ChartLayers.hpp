#pragma once
#include "chart/layers/TradeLayers.hpp"
#include "chart/layers/CvdLayer.hpp"
#include "chart/layers/FlowFooterLayer.hpp"
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
    CvdLayer cvd;
    VolumeProfileLayer vp{&cvd.presentation()};
    TpoLayer tpo{&cvd.presentation()};
    FlowFooterLayer flowFooter{cvd};
    LadderLayer ladder{&cvd.presentation()};
    ChartTextLayer text{heatmap, ladder, footprintCells, cvd, flowFooter};
    LayerRegistry registry;
    BookLadderFeed ladderFeed{ladder.mailbox()};
    FlowFeed flowFeed{footprintCells.mailbox()};
    CvdFeed cvdFeed{cvd.mailbox(), cvd.presentation()};
    void bindFlowFeeds(ChartViewController& view, const PublishedFrame& frames, LayerVisibility& visibility, ChartTpoQml& tpoSettings) {
        flowFeed.bind(view, frames, visibility, footprintCells.presentation());
        cvdFeed.bind(view, frames, visibility, tpoSettings);
    }
    void onFlowFrameChanged() { flowFeed.onFrameChanged(); cvdFeed.onFrameChanged(); }
    ChartLayers() { registerChartLayers(registry, *this); }
};
} // namespace chart
