#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/AxisTextService.hpp"
#include <memory>

namespace chart {
class HeatmapLayer;
class LadderLayer;
class FootprintCellsLayer;
class CvdLayer;
class ChartTextLayer final : public IChartLayer {
public:
    ChartTextLayer(HeatmapLayer& heatmap, LadderLayer& ladder, FootprintCellsLayer& footprint, CvdLayer& cvd)
        : heatmap_(heatmap), ladder_(ladder), footprint_(footprint), cvd_(cvd) {}
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
    FootprintCellsLayer& footprint_;
    CvdLayer& cvd_;
};
} // namespace chart
