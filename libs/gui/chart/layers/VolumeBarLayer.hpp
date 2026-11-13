#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
#include <QColor>
#include <span>
#include <vector>

class QSGGeometryNode;
struct CandleOverlayBar;
namespace chart {
// Render-owned layer hosted by the volume sibling item. It shares the candle
// acquisition path, while its item depth keeps it below the candle subtree.
class VolumeBarLayer final : public IChartLayer {
public:
    struct Style {
        QColor up, down;
        double fraction = 0.85, maxBodyPx = 71, bandPercent = 22, opacity = 0.45;
    };
    VolumeBarLayer();
    ~VolumeBarLayer() override;
    std::string_view name() const override { return "volumeBars"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override { render(); }
    void capture(const MappingFrameContext&, std::span<const CandleOverlayBar>, const Style&);
    void setBottomInset(double);
    void clear();
    void render();
private:
    MappingFrameContext frame_;
    Style style_;
    std::vector<CandleOverlayBar> bars_;
    QSGGeometryNode* node_ = nullptr;
    int capacity_ = 0;
    double inset_ = 0;
    bool dirty_ = true;
};
} // namespace chart
