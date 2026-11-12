#pragma once
#include "render/TradeBubbleFrame.hpp"
#include "heatmap/HeatmapChartSettings.hpp"
#include "protocol/TradeHistoryWire.hpp"
#include <functional>
#include <QObject>
struct TradeBubbleRendererTest;
namespace chart {
class TradeBubbleStyleStore;
class ITradeBubbleFrameSource {
public:
    virtual ~ITradeBubbleFrameSource() = default;
    virtual std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const = 0;
};
class TradeBubbleFeed final : public ITradeBubbleFrameSource {
    friend struct ::TradeBubbleRendererTest;
public:
    explicit TradeBubbleFeed(std::shared_ptr<TradeBubbleStyleStore> store = {});
    bool append(const Trade& trade);
    size_t appendHistory(const protocol::tradewire::Page& page);
    void clear() { tape_->clear(); statistics_.reset(); product_.clear(); }
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
    void selectProduct(const std::string& product);
    void reloadStyle();
    void notePrice(double price, int64_t timeMs);
    std::string product_;
    bool customMinimum_ = false;
    QObject styleContext_;
    std::shared_ptr<TradeBubbleStyleStore> styleStore_;
    bubbles::Mode requested_ = bubbles::Mode::Individual;
    bubbles::PlacementMode placement_;
    bubbles::ReferenceStatistics statistics_;

};
} // namespace chart
