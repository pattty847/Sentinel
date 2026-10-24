#pragma once
#include <functional>

class QQuickItem;
class GridViewState;
namespace chart {
struct ChartLayers;
class LayerVisibility;
class PublishedFrame;

struct LadderWiring {
    QQuickItem& item;
    ChartLayers& layers;
    LayerVisibility& visibility;
    GridViewState& view;
    const PublishedFrame& published;
    std::function<void(double, double)> top;
};
// The shell side of the book ladder: the feed's ports, its republish triggers (a
// committed pan, a held drag, the layer's request), the layer toggle and the chart's
// exposure (item visible, window shown and not minimized). The rasterChanged trigger
// is UGR's signal and is connected by UGR.
void bindLadder(LadderWiring wiring);
} // namespace chart
