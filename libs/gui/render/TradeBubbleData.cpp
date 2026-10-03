#include "TradeBubbleData.hpp"
#include <algorithm>
#include <cmath>

namespace trade_bubbles {
bool Tape::append(Sample s) {
    if (s.timeMs <= 0 || !std::isfinite(s.price) || !std::isfinite(s.size) ||
        s.price <= 0 || s.size <= 0 || !std::isfinite(s.price * s.size) ||
        (s.side != AggressorSide::Buy && s.side != AggressorSide::Sell)) return false;
    rows_[next_] = s;
    next_ = (next_ + 1) % Capacity;
    count_ = std::min(count_ + 1, Capacity);
    ++revision_;
    return true;
}
double Layout::radius(double notional) { return std::min(MaxRadius, 3 * std::sqrt(notional / 1000)); }
std::span<const Bubble> Layout::build(std::span<const Sample> trades, const TimeAxisMapping& map, double minNotional) {
    count_ = 0;
    if (!map.valid || !std::isfinite(minNotional) || minNotional < 0 ||
        !std::isfinite(map.drawRect.width()) || !std::isfinite(map.drawRect.height()) ||
        map.drawRect.width() <= 0 || map.drawRect.height() <= 0) return {};
    // One bucket per side; coarsens with the view, never drops overflow rows.
    const int nx = int(std::clamp(std::ceil(map.drawRect.width() / 6), 1.0, 64.0));
    const int ny = int(std::clamp(std::ceil(map.drawRect.height() / 6), 1.0, 32.0));
    std::fill(bins_.begin(), bins_.end(), Bin{});
    for (const auto& t : trades) {
        const double n = t.price * t.size;
        if (t.timeMs < map.viewStartMs || t.timeMs >= map.viewEndMs ||
            t.price < map.viewMinPrice || t.price > map.viewMaxPrice ||
            !std::isfinite(n) || n <= 0 || n < minNotional ||
            (t.side != AggressorSide::Buy && t.side != AggressorSide::Sell)) continue;
        const double x = map.timeToScreenX(double(t.timeMs)), y = map.priceToScreenY(t.price);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        const int ix = std::clamp(int((x - map.drawRect.x()) / map.drawRect.width() * nx), 0, nx - 1);
        const int iy = std::clamp(int((y - map.drawRect.y()) / map.drawRect.height() * ny), 0, ny - 1);
        auto& b = bins_[size_t((iy * nx + ix) * 2 + (t.side == AggressorSide::Sell))];
        const double total = b.notional + n;
        if (!std::isfinite(total)) continue;
        // Stable weighted centroid without multiplying epoch times or large sizes.
        b.x += (x - b.x) * (n / total);
        b.y += (y - b.y) * (n / total);
        b.notional = total;
    }
    for (int i = 0; i < nx * ny * 2; ++i) {
        const auto& b = bins_[size_t(i)];
        if (b.notional > 0)
            bubbles_[count_++] = {float(b.x), float(b.y), float(radius(b.notional)),
                                 i % 2 ? AggressorSide::Sell : AggressorSide::Buy};
    }
    return {bubbles_.data(), count_};
}
} // namespace trade_bubbles
