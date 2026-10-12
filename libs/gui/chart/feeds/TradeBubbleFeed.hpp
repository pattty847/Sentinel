#pragma once
#include "render/TradeBubbleFrame.hpp"
#include "heatmap/HeatmapChartSettings.hpp"
#include <functional>
struct TradeBubbleRendererTest;
namespace chart {
class ITradeBubbleFrameSource {
public:
    virtual ~ITradeBubbleFrameSource() = default;
    virtual std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const = 0;
};
class TradeBubbleFeed final : public ITradeBubbleFrameSource {
    friend struct ::TradeBubbleRendererTest;
public:
    bool append(const Trade& trade);
    void clear() { tape_->clear(); }
    void setSettings(const heatmap::HeatmapChartSettings&, const std::function<void()>& notify);
    void publish(const TimeAxisMapping&);
    void disableFrame() { frame_->enabled = false; }
    bool tradesAboveCandles() const { return above_; }
    std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const override { return frame_; }
private:
    std::shared_ptr<trade_bubbles::Tape> tape_ = std::make_shared<trade_bubbles::Tape>();
    std::shared_ptr<trade_bubbles::RenderFrame> frame_ = std::make_shared<trade_bubbles::RenderFrame>(tape_);
    bool above_ = true, show_ = false;
    double minimum_ = 0;
    QColor buy_, sell_;
};
} // namespace chart
