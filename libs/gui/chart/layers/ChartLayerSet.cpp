#include "chart/layers/ChartLayerSet.hpp"

namespace chart {
void registerChartLayers(LayerRegistry& registry, IChartLayer& footprint, IChartLayer& vp, IChartLayer& tpo) {
    registry.add(footprint);
    registry.add(vp);
    registry.add(tpo);
}
} // namespace chart
