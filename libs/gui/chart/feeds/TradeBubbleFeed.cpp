#include "chart/feeds/TradeBubbleFeed.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
namespace chart {
bool TradeBubbleFeed::append(const Trade& trade) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(trade.timestamp.time_since_epoch()).count();
    return tape_->append({ms, trade.price, trade.size, trade.side, trade.trade_id, ns}) && show_;
}
void TradeBubbleFeed::setSettings(const heatmap::HeatmapChartSettings& settings, const std::function<void()>& notify) {
    const auto gradients = heatmap::gpu::gradientsFor(settings);
    show_ = settings.showTrades;
    if (above_ != settings.tradesAboveCandles) {
        above_ = settings.tradesAboveCandles;
        notify();
    }
    minimum_ = settings.tradeMinNotional;
    requested_ = settings.bubblePlacement == "slot" ? bubbles::Mode::Slot :
                 settings.bubblePlacement == "auto" ? bubbles::Mode::Auto : bubbles::Mode::Individual;
    const auto& bid = gradients.bid.back();
    const auto& ask = gradients.ask.back();
    buy_ = QColor(bid.r, bid.g, bid.b);
    sell_ = QColor(ask.r, ask.g, ask.b);
}
void TradeBubbleFeed::publish(const TimeAxisMapping& mapping) {
    frame_->mapping = mapping;
    frame_->enabled = show_;
    frame_->options.minimum = minimum_;
    const double columnPx = mapping.srcRect.width() > 0 ? mapping.drawRect.width() / mapping.srcRect.width() : 0;
    frame_->options.mode = placement_.select(requested_, columnPx, mapping.appendMs);
    if (show_ && mapping.valid) {
        const auto window = tape_->visible(mapping.viewStartMs, mapping.viewEndMs);
        if (!referenceReady_ || window.key() != referenceKey_) {
            frame_->options.reference = statistics_.reference(trade_bubbles::PolicyRows{window.rows});
            referenceKey_ = window.key();
            referenceReady_ = true;
            ++referenceUpdates_;
        }
    }
    frame_->buy = buy_;
    frame_->sell = sell_;
}
} // namespace chart
