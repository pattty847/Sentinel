#include "chart/feeds/TradeBubbleFeed.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
#include <chrono>
namespace chart {
bool TradeBubbleFeed::append(const Trade& trade) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(trade.timestamp.time_since_epoch()).count();
    return tape_->append({ms, trade.price, trade.size, trade.side, trade.trade_id, ns}) && show_;
}
size_t TradeBubbleFeed::appendHistory(const protocol::tradewire::Page& page) {
    if (page.status == "refused") return 0;
    size_t added = 0;
    for (const auto& row : page.rows) {
        if (row.exchangeMs > INT64_MAX / 1'000'000) continue;
        if (tape_->append({row.exchangeMs, row.price, row.size, row.side, row.id,
                           row.exchangeMs * 1'000'000})) ++added;
    }
    return added;
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
    frame_->referencePending = false;
    frame_->referenceRetryMs = 0;
    frame_->options.minimum = minimum_;
    const double columnPx = mapping.srcRect.width() > 0 ? mapping.drawRect.width() / mapping.srcRect.width() : 0;
    frame_->options.mode = placement_.select(requested_, columnPx, mapping.appendMs);
    if (show_ && mapping.valid) {
        const auto window = tape_->visible(mapping.viewStartMs, mapping.viewEndMs);
        const bool slot = frame_->options.mode == bubbles::Mode::Slot && mapping.appendMs > 0;
        const auto expanded = slot ? tape_->visible(std::floor(mapping.viewStartMs / mapping.appendMs) * mapping.appendMs,
                                                     std::ceil(mapping.viewEndMs / mapping.appendMs) * mapping.appendMs)
                                   : window;
        const auto key = window.key(), slotKey = expanded.key();
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        frame_->options.reference = statistics_.update(trade_bubbles::PolicyRows{window.rows},
            trade_bubbles::PolicyRows{expanded.rows}, {key.first, key.last, slotKey.first, slotKey.last, key.count, slotKey.count},
            {mapping.viewStartMs, mapping.viewEndMs, mapping.appendMs, minimum_}, frame_->options.mode, nowMs);
        frame_->referencePending = statistics_.pending();
        frame_->referenceRetryMs = statistics_.retryAfterMs();
    }
    frame_->buy = buy_;
    frame_->sell = sell_;
}
} // namespace chart
