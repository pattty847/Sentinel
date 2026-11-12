#include "chart/feeds/TradeBubbleFeed.hpp"
#include "chart/shell/TradeBubbleStyleStore.hpp"
#include <chrono>
namespace chart {
TradeBubbleFeed::TradeBubbleFeed(std::shared_ptr<TradeBubbleStyleStore> store)
    : styleStore_(store ? std::move(store) : std::make_shared<TradeBubbleStyleStore>()) {
    reloadStyle();
    QObject::connect(&TradeBubbleStyleChanges::instance(), &TradeBubbleStyleChanges::changed,
                     &styleContext_, [this] { reloadStyle(); });
}
void TradeBubbleFeed::reloadStyle() {
    frame_->style = styleStore_->load(product_);
    customMinimum_ = styleStore_->hasMinimum(product_);
}
void TradeBubbleFeed::selectProduct(const std::string& product) {
    if (product.empty() || product_ == product) return;
    product_ = product;
    reloadStyle();
}
void TradeBubbleFeed::notePrice(double price, int64_t timeMs) {
    TradeBubbleStyleStore::notePrice(product_, price, timeMs);
    if (!customMinimum_) frame_->style.minimumBaseSize = TradeBubbleStyleStore::minimum(product_);
}

bool TradeBubbleFeed::append(const Trade& trade) {
    selectProduct(trade.product_id);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    notePrice(trade.price, ms);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(trade.timestamp.time_since_epoch()).count();
    return tape_->append({ms, trade.price, trade.size, trade.side, trade.trade_id, ns}) && show_;
}
size_t TradeBubbleFeed::appendHistory(const protocol::tradewire::Page& page) {
    if (page.status == "refused") return 0;
    selectProduct(page.symbol);
    if (!page.rows.empty()) notePrice(page.rows.back().price, page.rows.back().exchangeMs);
    size_t added = 0;
    for (const auto& row : page.rows) {
        if (row.exchangeMs > INT64_MAX / 1'000'000) continue;
        if (tape_->append({row.exchangeMs, row.price, row.size, row.side, row.id,
                           row.exchangeMs * 1'000'000})) ++added;
    }
    return added;
}
void TradeBubbleFeed::setSettings(const heatmap::HeatmapChartSettings& settings, const std::function<void()>& notify) {
    selectProduct(settings.sensitivitySymbol);
    show_ = settings.showTrades;
    if (above_ != settings.tradesAboveCandles) {
        above_ = settings.tradesAboveCandles;
        notify();
    }
    minimum_ = settings.tradeMinNotional;
    requested_ = settings.bubblePlacement == "slot" ? bubbles::Mode::Slot :
                 settings.bubblePlacement == "auto" ? bubbles::Mode::Auto : bubbles::Mode::Individual;
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
}
} // namespace chart
