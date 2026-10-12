#pragma once
#include "chart/layers/ChartLayerSet.hpp"
#include "render/FootprintOverlayRenderer.hpp"
#include "render/VolumeProfileRenderer.hpp"
#include "render/TpoOverlayRenderer.hpp"

namespace chart {
class FootprintLayer final : public IChartLayer {
public:
    std::string_view name() const override { return "footprint"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    FootprintOverlayRenderer renderer;
private:
    std::vector<FootprintOverlayRenderer::PendingUpload> uploads_;
};
class VolumeProfileLayer final : public IChartLayer {
public:
    std::string_view name() const override { return "VP"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    VolumeProfileRenderer renderer;
};
class TpoLayer final : public IChartLayer {
public:
    std::string_view name() const override { return "TPO"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    TpoOverlayRenderer renderer;
};
struct TradeLayers {
    FootprintLayer footprint;
    TpoLayer tpo;
    VolumeProfileLayer vp;
    LayerRegistry registry;
    TradeLayers() { registerChartLayers(registry, footprint, vp, tpo); }
};
} // namespace chart
