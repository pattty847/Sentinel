#include "chart/feeds/TradeBubbleFeed.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
namespace chart {
bool TradeBubbleFeed::append(const Trade& trade) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    return tape_->append({ms, trade.price, trade.size, trade.side, trade.trade_id}) && show_;
}
void TradeBubbleFeed::setSettings(const heatmap::HeatmapChartSettings& settings, const std::function<void()>& notify) {
    const auto gradients = heatmap::gpu::gradientsFor(settings);
    show_ = settings.showTrades;
    if (above_ != settings.tradesAboveCandles) {
        above_ = settings.tradesAboveCandles;
        notify();
    }
    minimum_ = settings.tradeMinNotional;
    const auto& bid = gradients.bid.back();
    const auto& ask = gradients.ask.back();
    buy_ = QColor(bid.r, bid.g, bid.b);
    sell_ = QColor(ask.r, ask.g, ask.b);
}
void TradeBubbleFeed::publish(const TimeAxisMapping& mapping) {
    frame_->mapping = mapping;
    frame_->enabled = show_;
    frame_->minNotional = minimum_;
    frame_->buy = buy_;
    frame_->sell = sell_;
}
} // namespace chart
