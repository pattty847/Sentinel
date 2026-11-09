#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/core/FootprintLod.hpp"
#include "render/FootprintOverlayRenderer.hpp"
#include "render/VolumeProfileRenderer.hpp"
#include "render/TpoOverlayRenderer.hpp"

namespace chart {
class FootprintLayer final : public IChartLayer {
public:
    explicit FootprintLayer(const FootprintPresentation* look = nullptr) : look_(look) {}
    std::string_view name() const override { return "footprint"; }
    bool selected(const ChartFrame&) const;
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    FootprintOverlayRenderer renderer;
private:
    const FootprintPresentation* look_ = nullptr;
    std::vector<FootprintOverlayRenderer::PendingUpload> uploads_;
};
class VolumeProfileLayer final : public IChartLayer {
public:
    std::string_view name() const override { return "VP"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    VolumeProfileRenderer renderer;
private:
    std::vector<float> bins_;
    VolumeProfileState::Snapshot snap_;
};
class TpoLayer final : public IChartLayer {
public:
    std::string_view name() const override { return "TPO"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    TpoOverlayRenderer renderer;
};
} // namespace chart
