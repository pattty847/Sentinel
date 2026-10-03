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
        if (s.timeMs < at(0).timeMs) return false;
        if (!at(0).tradeId.empty()) ids_.erase(at(0).tradeId);
        head_ = (head_ + 1) % Capacity;
        --count_;
    }
    if (!s.tradeId.empty()) ids_.insert(s.tradeId);
    s.sequence = ++revision_;
    // Normally O(1). Late/reconnect rows take the ordered insertion slow path.
    size_t pos = count_;
    while (pos && at(pos-1).timeMs > s.timeMs) {
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
    return result;
}
Layout::Layout() : points_(std::make_unique<Point[]>(Tape::Capacity)) {}
double Layout::radius(double notional) { return std::min(MaxRadius, 3 * std::sqrt(notional / 1000)); }
std::span<const Bubble> Layout::build(Samples trades, const TimeAxisMapping& map, double minNotional) {
    count_ = scannedRows_ = 0;
    binSize_ = 0;
    if (!map.valid || !std::isfinite(minNotional) || minNotional < 0 ||
        !std::isfinite(map.drawRect.width()) || !std::isfinite(map.drawRect.height()) ||
        map.drawRect.width() <= 0 || map.drawRect.height() <= 0) return {};
    size_t pointCount = 0;
    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& t = trades[i];
        ++scannedRows_;
        const double n = t.price * t.size;
        if (t.timeMs < map.viewStartMs || t.timeMs >= map.viewEndMs ||
            t.price < map.viewMinPrice || t.price > map.viewMaxPrice ||
            !std::isfinite(n) || n <= 0 || n < minNotional ||
            (t.side != AggressorSide::Buy && t.side != AggressorSide::Sell)) continue;
        const double x = map.timeToScreenX(double(t.timeMs)), y = map.priceToScreenY(t.price);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        // All callers pass a bounded Tape or a smaller test span.
        if (pointCount == Tape::Capacity) break;
        points_[pointCount++] = {x, y, n, t.side};
    }
    const auto appendBubble = [&](const Point& p) {
        bubbles_[count_++] = {float(p.x), float(p.y), float(radius(p.notional)), p.side};
    };
    if (pointCount <= MaxBubbles) {
        for (size_t i = 0; i < pointCount; ++i) appendBubble(points_[i]);
    } else {
        // Start fine; widen only if more than 4096 occupied side/cell pairs remain.
        // A fixed <=8 px grid cannot bound arbitrary spatially distinct executions.
        binSize_ = 6;
        for (;;) {
            std::fill(bins_.begin(), bins_.end(), Bin{});
            size_t occupied = 0;
            for (size_t i = 0; i < pointCount; ++i) {
                const auto& p = points_[i];
                const double cx = std::floor((p.x-map.drawRect.x())/binSize_);
                const double cy = std::floor((p.y-map.drawRect.y())/binSize_);
                size_t slot = (std::hash<double>{}(cx) ^ (std::hash<double>{}(cy) << 1) ^ size_t(p.side)) % bins_.size();
                while (bins_[slot].point.notional && (bins_[slot].cellX != cx || bins_[slot].cellY != cy || bins_[slot].point.side != p.side))
                    slot = (slot+1) % bins_.size();
                auto& b = bins_[slot];
                if (!b.point.notional) {
                    if (++occupied > MaxBubbles) break;
                    b = {p, cx, cy};
                } else {
                    const double total = b.point.notional + p.notional;
                    if (!std::isfinite(total)) continue;
                    b.point.x += (p.x-b.point.x) * (p.notional/total);
                    b.point.y += (p.y-b.point.y) * (p.notional/total);
                    b.point.notional = total;
                }
            }
            if (occupied <= MaxBubbles) break;
            binSize_ *= 4.0/3.0;
        }
        for (const auto& b : bins_) if (b.point.notional) appendBubble(b.point);
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
