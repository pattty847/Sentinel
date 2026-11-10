#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/ChartPaneClip.hpp"

namespace chart {
class PaneChartLayer : public IChartLayer {
public:
    explicit PaneChartLayer(const CvdPresentation* presentation = nullptr) : presentation_(presentation) {}
    QSGNode* createContainer() override { return new ChartPaneClip; }
    void sync(const ChartFrame& frame) override {
        const auto& context = frame.context;
        inset_ = presentation_ && context.raster.valid ?
            presentation_->bottomInset(context.time.activeTimeframeMs, context.surfaceBounds.height()) : 0;
    }
protected:
    QSGNode* content(const ChartFrame& frame, LayerNodeContext& context) {
        if (!presentation_) return context.root;
        auto* pane = static_cast<ChartPaneClip*>(context.root);
        if (!pane) return nullptr;
        pane->sync(frame.context.surfaceBounds, inset_);
        return pane->content();
    }
private:
    const CvdPresentation* presentation_;
    double inset_ = 0;
};
} // namespace chart
