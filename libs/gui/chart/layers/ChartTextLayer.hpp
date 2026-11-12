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
class VolumeProfileLayer;
class CvdLayer;
class FlowFooterLayer;
class ChartTextLayer final : public IChartLayer {
public:
    ChartTextLayer(HeatmapLayer& heatmap, LadderLayer& ladder, FootprintCellsLayer& footprint,
                   VolumeProfileLayer& vp, CvdLayer& cvd, FlowFooterLayer& footer)
        : heatmap_(heatmap), ladder_(ladder), footprint_(footprint), vp_(vp), cvd_(cvd), footer_(footer) {
        emptyGlyphs_.reserve(128);
    }
    std::string_view name() const override { return "text"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    QSGNode* createContainer() override { return renderer.createRoot(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void buildMsdfAtlas();
    QString getMsdfAtlasMemory() const;
    const std::vector<ChartGlyphInstance>& emptyGlyphs() const { return emptyGlyphs_; }
    ChartTextAtlas atlas;
    bool atlasBuilt = false;
    ChartTextRenderer renderer;
    std::unique_ptr<AxisTextService> axes;
private:
    void submitOverlayLabels(const ChartFrame&, LayerNodeContext&);
    void submitEmptyLabels(const ChartFrame&, LayerNodeContext&);
    std::array<QRectF, kFootprintMaxBars + 1> occlusion_{};
    std::vector<ChartGlyphInstance> emptyGlyphs_;
    HeatmapLayer& heatmap_;
    LadderLayer& ladder_;
    FootprintCellsLayer& footprint_;
    VolumeProfileLayer& vp_;
    CvdLayer& cvd_;
    FlowFooterLayer& footer_;
};
} // namespace chart
