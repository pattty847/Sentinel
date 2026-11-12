#include "TradeBubbleData.hpp"
#include <algorithm>
#include <cmath>

namespace trade_bubbles {
bool Tape::append(Sample s) {
    if (s.timeMs <= 0 || !std::isfinite(s.price) || !std::isfinite(s.size) ||
        s.price <= 0 || s.size <= 0 || !std::isfinite(s.price * s.size) ||
        (s.side != AggressorSide::Buy && s.side != AggressorSide::Sell)) return false;
    if (!s.tradeId.empty() && ids_.contains(s.tradeId)) return false;
    // Keep the newest Capacity executions by event time, including late arrivals.
    if (count_ == Capacity) {
        if (s.timeMs < at(0).timeMs ||
            (s.timeMs == at(0).timeMs && s.exchangeNs < at(0).exchangeNs)) return false;
        if (!at(0).tradeId.empty()) ids_.erase(at(0).tradeId);
        head_ = (head_ + 1) % Capacity;
        --count_;
    }
    if (!s.tradeId.empty()) ids_.insert(s.tradeId);
    s.sequence = ++revision_;
    // Normally O(1). Late/reconnect rows take the ordered insertion slow path.
    size_t pos = count_;
    while (pos && (at(pos-1).timeMs > s.timeMs ||
           (at(pos-1).timeMs == s.timeMs && at(pos-1).exchangeNs > s.exchangeNs))) {
        at(pos) = std::move(at(pos-1));
        --pos;
    }
    at(pos) = std::move(s);
    ++count_;
    return true;
}
void Tape::clear() {
    for (size_t i = 0; i < count_; ++i) at(i) = Sample{};
    head_ = count_ = 0;
    ids_.clear();
    ++revision_;
}
Samples Tape::slice(size_t begin, size_t end) const {
    const size_t offset = (head_ + begin) % Capacity;
    const size_t firstCount = std::min(end-begin, Capacity-offset);
    return {{rows_.data()+offset, firstCount}, {rows_.data(), end-begin-firstCount}};
}
Window Tape::visible(double startMs, double endMs) const {
    Window result;
    if (!std::isfinite(startMs) || !std::isfinite(endMs) || endMs <= startMs) return result;
    const auto lower = [&](double time) {
        size_t lo = 0, hi = count_;
        while (lo < hi) {
            ++result.comparisons;
            const size_t mid = lo + (hi-lo)/2;
            if (at(mid).timeMs < time) lo = mid+1;
            else hi = mid;
        }
        return lo;
    };
    const size_t begin = lower(startMs), end = lower(endMs);
    result.rows = slice(begin, end);
    if (begin) { result.predecessor = at(begin - 1).sequence; result.previousPrice = at(begin - 1).price; }
    return result;
}
Layout::Layout() = default;
std::span<const Bubble> Layout::build(Samples trades, const TimeAxisMapping& map,
                                      const chart::bubbles::Options& options, double previousPrice,
                                      const StyleModel& style) {
    count_ = scannedRows_ = 0;
    if (!map.valid || !std::isfinite(options.minimum) || options.minimum < -1 ||
        !std::isfinite(map.drawRect.width()) || !std::isfinite(map.drawRect.height()) ||
        map.drawRect.width() <= 0 || map.drawRect.height() <= 0) return {};
    // Filter executions before the existing sweep/slot policy, without copying
    // tape rows. Invalid rows are skipped by that policy.
    struct FilteredRows {
        PolicyRows source;
        double minimum;
        size_t size() const { return source.size(); }
        chart::bubbles::Execution operator[](size_t i) const {
            auto row = source[i];
            if (row.size < minimum) row.size = 0;
            return row;
        }
    };
    const auto look = clamped(style);
    const auto marks = policy_.build(FilteredRows{{trades}, look.minimumBaseSize}, options, map.appendMs, previousPrice);
    scannedRows_ = trades.size();
    // Keep the newest qualifying marks under overload; never merge nearby
    // executions, and never grow scratch or geometry on the render thread.
    for (auto it = marks.rbegin(); it != marks.rend() && count_ < MaxBubbles; ++it) {
        const auto& t = *it;
        const auto& element = t.side == int(AggressorSide::Buy) ? look.buy : look.sell;
        if (!element.visible) continue;
        if (options.mode != chart::bubbles::Mode::Slot &&
            (t.timeMs < map.viewStartMs || t.timeMs >= map.viewEndMs)) continue;
        if (t.maxPrice < map.viewMinPrice || t.minPrice > map.viewMaxPrice) continue;
        const double x = map.timeToScreenX(t.timeMs), y = map.priceToScreenY(t.price);
        const double lowY = map.priceToScreenY(t.maxPrice), highY = map.priceToScreenY(t.minPrice);
        if (!std::isfinite(float(x)) || !std::isfinite(float(y)) || !std::isfinite(float(lowY)) || !std::isfinite(float(highY))) continue;
        const double reference = options.mode == chart::bubbles::Mode::Slot ? options.reference.aggregateP95 : options.reference.p95;
        // The policy's radius/cap use an 18px scale; the style rescales both.
        const double r = std::min(t.radiusCap, radius(t.notional, reference)) * element.width / MaxRadius;
        bubbles_[count_++] = {float(x), float(y), float(r), AggressorSide(t.side),
                             float(lowY), float(highY), 1, t.notional};
    }
    // Painter's order: small executions remain visible on top of large ones,
    // across sides AND neighbouring cells. Tie breakers keep output deterministic.
    std::sort(bubbles_.begin(), bubbles_.begin()+count_, [](const Bubble& a, const Bubble& b) {
        if (a.radius != b.radius) return a.radius > b.radius;
        if (a.side != b.side) return a.side < b.side;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    return {bubbles_.data(), count_};
}
} // namespace trade_bubbles
