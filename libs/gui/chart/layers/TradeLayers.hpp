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
class TpoLayer final : public PaneChartLayer {
public:
    using PaneChartLayer::PaneChartLayer;
    std::string_view name() const override { return "TPO"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    TpoOverlayRenderer renderer;
};
} // namespace chart
