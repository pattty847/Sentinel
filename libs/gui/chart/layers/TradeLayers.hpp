#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/layers/PaneChartLayer.hpp"
#include "render/VolumeProfileRenderer.hpp"
#include "render/TpoOverlayRenderer.hpp"

namespace chart {
class VolumeProfileLayer final : public PaneChartLayer {
public:
    using PaneChartLayer::PaneChartLayer;
    std::string_view name() const override { return "VP"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    VolumeProfileRenderer renderer;
private:
    std::vector<float> bins_;
    VolumeProfileState::Snapshot snap_;
};
// The TPO draws in TpoOverlayItem, above the candle overlay item, so it is not
// in the layer set: this holds the renderer the feeds and look settings drive.
struct TpoLayer {
    TpoOverlayRenderer renderer;
};
} // namespace chart
