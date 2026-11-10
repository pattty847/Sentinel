#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/core/FootprintLod.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/AxisTextService.hpp"
#include <memory>
#include <array>

namespace chart {
class HeatmapLayer;
class LadderLayer;
class FootprintCellsLayer;
class CvdLayer;
class FlowFooterLayer;
class ChartTextLayer final : public IChartLayer {
public:
    ChartTextLayer(HeatmapLayer& heatmap, LadderLayer& ladder, FootprintCellsLayer& footprint, CvdLayer& cvd, FlowFooterLayer& footer)
        : heatmap_(heatmap), ladder_(ladder), footprint_(footprint), cvd_(cvd), footer_(footer) {}
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
    void submitOverlayLabels(const ChartFrame&, LayerNodeContext&);
    std::array<QRectF, kFootprintMaxBars + 1> occlusion_{};
    HeatmapLayer& heatmap_;
    LadderLayer& ladder_;
    FootprintCellsLayer& footprint_;
    CvdLayer& cvd_;
    FlowFooterLayer& footer_;
};
} // namespace chart
