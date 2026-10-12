#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/AxisTextService.hpp"
#include <memory>

namespace chart {
class HeatmapLayer;
class ChartTextLayer final : public IChartLayer {
public:
    explicit ChartTextLayer(HeatmapLayer& heatmap) : heatmap_(heatmap) {}
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
};
} // namespace chart
