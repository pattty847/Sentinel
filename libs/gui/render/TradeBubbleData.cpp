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
namespace {
// Avalanche integer coordinates before taking high bits. libc++ hash<double>
// preserves zero mantissa low bits for these integer-valued coordinates.
uint64_t cellHash(uint32_t x, uint32_t y, AggressorSide side) {
    uint64_t h = (uint64_t(x) << 32) | y;
    h ^= uint64_t(side) * 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}
}
Layout::Layout() : points_(std::make_unique<Point[]>(Tape::Capacity)),
                   fineSlots_(std::make_unique<uint32_t[]>(FineSlots)) {}
double Layout::radius(double notional) { return std::min(MaxRadius, 3 * std::sqrt(notional / 1000)); }
std::span<const Bubble> Layout::build(Samples trades, const TimeAxisMapping& map, double minNotional) {
    count_ = scannedRows_ = 0;
    binSize_ = 0;
    hashProbes_ = coarseCellVisits_ = fineCellCount_ = 0;
    if (!map.valid || !std::isfinite(minNotional) || minNotional < 0 ||
        !std::isfinite(map.drawRect.width()) || !std::isfinite(map.drawRect.height()) ||
        map.drawRect.width() <= 0 || map.drawRect.height() <= 0) return {};
    size_t pointCount = 0;
    double maxX = 0, maxY = 0;
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
        maxX = std::max(maxX, x-map.drawRect.x());
        maxY = std::max(maxY, y-map.drawRect.y());
    }
    const auto appendBubble = [&](const Point& p) {
        bubbles_[count_++] = {float(p.x), float(p.y), float(radius(p.notional)), p.side};
    };
    if (pointCount <= MaxBubbles) {
        for (size_t i = 0; i < pointCount; ++i) appendBubble(points_[i]);
    } else {
        // Build the fine occupancy exactly once. Compact in place: a cell's
        // index never exceeds the source row currently being consumed.
        // Ordinary charts fit a dense fine grid. Direct indexing avoids hashing
        // entirely there; unusually large plots use the bounded mixed-key table.
        const double columns = std::floor(maxX/6)+1, rows = std::floor(maxY/6)+1;
        const bool dense = columns*rows*2 <= FineSlots;
        const size_t denseColumns = dense ? size_t(columns) : 0;
        std::fill_n(fineSlots_.get(), dense ? size_t(columns*rows*2) : FineSlots, 0u);
        const auto merge = [](Point& dst, const Point& src) {
            const double total = dst.notional + src.notional;
            if (!std::isfinite(total)) return;
            dst.x += (src.x-dst.x) * (src.notional/total);
            dst.y += (src.y-dst.y) * (src.notional/total);
            dst.notional = total;
        };
        for (size_t i = 0; i < pointCount; ++i) {
            auto p = points_[i];
            // QQuickItem dimensions are screen pixels, not an unbounded world grid.
            p.cellX = uint32_t(std::clamp(std::floor((p.x-map.drawRect.x())/6), 0.0, double(UINT32_MAX)));
            p.cellY = uint32_t(std::clamp(std::floor((p.y-map.drawRect.y())/6), 0.0, double(UINT32_MAX)));
            size_t slot = dense ? (size_t(p.cellY)*denseColumns+p.cellX)*2 + (p.side == AggressorSide::Sell)
                                : cellHash(p.cellX, p.cellY, p.side) >> (64-18);
            for (;;) {
                ++hashProbes_;
                const auto index = fineSlots_[slot];
                if (!index) {
                    points_[fineCellCount_] = p;
                    fineSlots_[slot] = uint32_t(++fineCellCount_);
                    break;
                }
                auto& cell = points_[index-1];
                if (cell.cellX == p.cellX && cell.cellY == p.cellY && cell.side == p.side) {
                    merge(cell,p);
                    break;
                }
                slot = (slot+1) & (FineSlots-1);
            }
        }
        binSize_ = 6;
        if (fineCellCount_ <= MaxBubbles) {
            for (size_t i=0; i<fineCellCount_; ++i) appendBubble(points_[i]);
        } else {
            // A k-by-k group holds at most k*k fine cells per side. This lower
            // bound skips impossible candidates. Test increasing integer multiples
            // of 6 px and stop at the FIRST fitting size, without rescanning trades.
            uint32_t multiple = uint32_t(std::ceil(std::sqrt(double(fineCellCount_)/MaxBubbles)));
            for (;; ++multiple) {
                std::fill(coarseSlots_.begin(), coarseSlots_.end(), uint16_t(0));
                size_t occupied = 0;
                for (size_t i=0; i<fineCellCount_; ++i) {
                    ++coarseCellVisits_;
                    auto p = points_[i];
                    p.cellX /= multiple;
                    p.cellY /= multiple;
                    size_t slot = cellHash(p.cellX,p.cellY,p.side) >> (64-13);
                    for (;;) {
                        ++hashProbes_;
                        const auto index = coarseSlots_[slot];
                        if (!index) {
                            if (occupied == MaxBubbles) { ++occupied; break; }
                            bins_[occupied] = p;
                            coarseSlots_[slot] = uint16_t(++occupied);
                            break;
                        }
                        auto& cell = bins_[index-1];
                        if (cell.cellX == p.cellX && cell.cellY == p.cellY && cell.side == p.side) {
                            merge(cell,p);
                            break;
                        }
                        slot = (slot+1) & (coarseSlots_.size()-1);
                    }
                    if (occupied > MaxBubbles) break;
                }
                if (occupied <= MaxBubbles) {
                    binSize_ = 6.0 * multiple;
                    for (size_t i=0; i<occupied; ++i) appendBubble(bins_[i]);
                    break;
                }
            }
        }
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
