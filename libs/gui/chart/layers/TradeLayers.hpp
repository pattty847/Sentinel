#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/layers/PaneChartLayer.hpp"
#include "render/VolumeProfileRenderer.hpp"
#include "render/TpoOverlayRenderer.hpp"
#include <QString>

namespace chart {
class VolumeProfileLayer final : public PaneChartLayer {
public:
    using PaneChartLayer::PaneChartLayer;
    std::string_view name() const override { return "VP"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    QString emptyStateText(const ChartFrame&) const;
    VolumeProfileRenderer renderer;
private:
    struct VisibleKey {
        double timeStart = 0, timeEnd = 0, priceMin = 0, priceMax = 0;
        bool enabled = false;
        bool operator==(const VisibleKey&) const = default;
    };
    VisibleKey visibleKey_;
    bool visibleKeyValid_ = false;
    bool anyData_ = false;
    bool visibleData_ = false;
    std::vector<float> bins_;
    VolumeProfileState::Snapshot snap_;
};
class HeatmapLayer;
class TpoLayer final : public PaneChartLayer {
public:
    // `text`: the layer whose chart settings carry the text rule for the letters.
    explicit TpoLayer(const CvdPresentation* presentation, const HeatmapLayer* text = nullptr)
        : PaneChartLayer(presentation), text_(text) {}
    std::string_view name() const override { return "TPO"; }
    void onRootRebuilt() override { renderer.onRootRebuilt(); }
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    TpoOverlayRenderer renderer;
private:
    const HeatmapLayer* text_ = nullptr;
};
} // namespace chart
