#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/AxisTextService.hpp"
#include <memory>

namespace chart {
class HeatmapLayer;
class LadderLayer;
class ChartTextLayer final : public IChartLayer {
public:
    ChartTextLayer(HeatmapLayer& heatmap, LadderLayer& ladder) : heatmap_(heatmap), ladder_(ladder) {}
    std::string_view name() const override { return "text"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    QSGNode* createContainer() override { return renderer.createRoot(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void buildMsdfAtlas();
    QString getMsdfAtlasMemory() const;
    ChartTextAtlas atlas;
    bool atlasBuilt = false;
    ChartTextRenderer renderer;
    std::unique_ptr<AxisTextService> axes;
private:
    HeatmapLayer& heatmap_;
    LadderLayer& ladder_;
};
} // namespace chart
