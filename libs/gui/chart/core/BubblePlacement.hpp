#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

namespace chart::bubbles {
enum class Mode { Individual, Auto, Slot };
struct Execution {
    double timeMs = 0, price = 0, size = 0;
    int side = 0; // 0 buy, 1 sell
    std::string_view id;
    int64_t exchangeNs = 0;
};
struct Reference {
    double median = 0, p95 = 1, aggregateP95 = 1;
    bool operator==(const Reference&) const = default;
};
struct Options {
    Mode mode = Mode::Individual;
    Reference reference;
    double minimum = 0;
    bool operator==(const Options&) const = default;
};
inline constexpr double MinRadius = 2.0, MaxRadius = 18.0;
struct Mark {
    double timeMs = 0, price = 0, notional = 0;
    double minPrice = 0, maxPrice = 0;
    int side = 0;
    size_t sweepLevels = 1;
    bool moving = true, aggregate = false;
    double radiusCap = MaxRadius;
};
class PlacementMode {
public:
    Mode select(Mode requested, double columnPx, double tfMs) {
        if (requested != Mode::Auto) { initialized_ = false; return requested; }
        if (tfMs <= 1000) { initialized_ = false; return Mode::Individual; }
        if (!initialized_) { slot_ = columnPx < 40; initialized_ = true; }
        else if (columnPx >= 42) slot_ = false;
        else if (columnPx <= 38) slot_ = true;
        return slot_ ? Mode::Slot : Mode::Individual;
    }
private:
    bool initialized_ = false, slot_ = false;
};
inline double radius(double notional, double reference) {
    if (!(notional > 0) || !std::isfinite(notional) || !(reference > 0) || !std::isfinite(reference)) return 0;
    return std::clamp(9 * std::sqrt(notional / reference), MinRadius, MaxRadius);
}
inline double capsuleHalfWidth(double equivalentRadius, double length) {
    if (!(equivalentRadius > 0) || !std::isfinite(length) || length < 0) return 0;
    constexpr double pi = 3.14159265358979323846;
    const double area = pi * equivalentRadius * equivalentRadius;
    const double preserving = area / (std::sqrt(length*length + pi*area) + length);
    return std::max({1.5, MinRadius, preserving});
}
struct WindowStamp {
    uint64_t first = 0, last = 0, slotFirst = 0, slotLast = 0;
    size_t count = 0, slotCount = 0;
    bool operator==(const WindowStamp&) const = default;
};
struct VisibleView { double startMs = 0, endMs = 0, tfMs = 0, minimum = 0; };
class ReferenceStatistics {
public:
    static constexpr size_t Capacity = 100000;
    ReferenceStatistics() : scratch_(std::make_unique<double[]>(Capacity)),
                            aggregates_(std::make_unique<double[]>(Capacity)) {}
    void reset() { initialized_ = pending_ = false; active_ = {}; updates_ = 0; retryAfterMs_ = 0; }
    uint64_t updates() const { return updates_; }
    bool pending() const { return pending_; }
    int retryAfterMs() const { return retryAfterMs_; }
    static constexpr size_t storageBytes() { return 2 * Capacity * sizeof(double); }
    template<class Rows, class SlotRows>
    Reference update(const Rows& rows, const SlotRows& slotRows, WindowStamp stamp,
                     VisibleView view, Mode mode, int64_t nowMs) {
        const double width = lastView_.endMs - lastView_.startMs;
        const double nextWidth = view.endMs - view.startMs;
        const bool widthJump = initialized_ && width > 0 && nextWidth > 0 &&
                               (nextWidth >= 2*width || width >= 2*nextWidth);
        const bool settingsChanged = mode != lastMode_ || view.tfMs != lastView_.tfMs ||
                                     view.minimum != lastView_.minimum;
        const bool urgent = widthJump || settingsChanged;
        const bool changed = stamp != lastStamp_ || urgent;
        if (defer(changed, mode, urgent, nowMs)) return active_;
        const Reference raw = reference(rows);
        if (!initialized_) active_ = {0, 0, 0};
        settle(active_.median, raw.median, true);
        settle(active_.p95, raw.p95);
        if (mode == Mode::Slot)
            settle(active_.aggregateP95, aggregateReference(slotRows, view, active_.median));
        initialized_ = true; lastStamp_ = stamp; lastView_ = view;
        lastMode_ = mode; lastUpdateMs_ = nowMs; ++updates_;
        return active_;
    }
    template<class Rows> Reference reference(const Rows& rows) {
        size_t count = 0;
        for (size_t i = 0; i < rows.size() && count < Capacity; ++i) {
            const auto t = rows[i];
            const double n = t.price * t.size;
            if (std::isfinite(n) && n > 0) scratch_[count++] = n;
        }
        if (!count) return {};
        return {select(scratch_.get(), count, (count-1)/2),
                select(scratch_.get(), count, size_t(std::ceil(.95*double(count)))-1), 1};
    }
private:
    bool defer(bool changed, Mode mode, bool urgent, int64_t nowMs) {
        const int64_t remaining = interval(mode, active_.aggregateP95, urgent) - (nowMs-lastUpdateMs_);
        pending_ = initialized_ && changed && remaining > 0;
        retryAfterMs_ = pending_ ? int(std::min<int64_t>(remaining, 250)) : 0;
        return initialized_ && (!changed || pending_);
    }
    static int64_t interval(Mode mode, double aggregateP95, bool urgent) {
        if (mode == Mode::Slot && aggregateP95 <= 0) return 0;
        return urgent ? 100 : 250;
    }
    static double select(double* values, size_t count, size_t rank) {
        std::nth_element(values, values+rank, values+count);
        return values[rank];
    }
    static void settle(double& active, double candidate, bool lower = false) {
        if (!(candidate > 0) || !std::isfinite(candidate)) { active = candidate; return; }
        const int next = lower ? int(std::floor(4 * std::log2(candidate))) :
                                 int(std::llround(4 * std::log2(candidate)));
        const int old = active > 0 ? int(std::llround(4 * std::log2(active))) : next;
        if (!(active > 0) || std::abs(next-old) > 1) active = std::exp2(double(next)/4);
    }
    template<class Rows> size_t aggregate(const Rows& rows, double tfMs, double minimum) {
        if (!(tfMs > 0)) return 0;
        size_t count = 0;
        double start = -1, sums[2] = {};
        const auto flush = [&] {
            for (double n : sums) if (n >= minimum && n > 0 && std::isfinite(n) && count < Capacity) aggregates_[count++] = n;
            sums[0] = sums[1] = 0;
        };
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto t = rows[i];
            if (t.side != 0 && t.side != 1) continue;
            const double n = t.price * t.size;
            if (!(n > 0) || !std::isfinite(n)) continue;
            const double bar = std::floor(t.timeMs/tfMs)*tfMs;
            if (start >= 0 && bar != start) flush();
            start = bar;
            sums[t.side] += n;
        }
        if (start >= 0) flush();
        return count;
    }
    template<class Rows> double aggregateReference(const Rows& rows, VisibleView view, double median) {
        const double minimum = view.minimum < 0 ? median : view.minimum;
        const size_t count = aggregate(rows, view.tfMs, minimum);
        return count ? select(aggregates_.get(), count, size_t(std::ceil(.95*double(count)))-1) : 1;
    }
    std::unique_ptr<double[]> scratch_, aggregates_;
    Reference active_;
    WindowStamp lastStamp_;
    VisibleView lastView_;
    Mode lastMode_ = Mode::Individual;
    int64_t lastUpdateMs_ = 0;
    uint64_t updates_ = 0;
    int retryAfterMs_ = 0;
    bool initialized_ = false, pending_ = false;
};
// Scratch and output are allocated at construction, never while publishing or drawing.
class BubblePlacement {
public:
    static constexpr size_t Capacity = 100000;
    static constexpr size_t MaxMarks = Capacity + Capacity / 2;
    static constexpr size_t NotableCount = 3; // Largest merged executions in each live bar.
    BubblePlacement() : marks_(std::make_unique<Mark[]>(Capacity + Capacity / 2)),
                        scratch_(std::make_unique<double[]>(Capacity)) {}
    static constexpr size_t storageBytes() { return MaxMarks * sizeof(Mark) + Capacity * sizeof(double); }
    // Rows are event-time ordered and already windowed; options.mode is resolved by PlacementMode.
    template<class Rows> std::span<const Mark> build(const Rows& rows, const Options& options,
                                                    double tfMs, double previousPrice = 0) {
        count_ = 0;
        if (!std::isfinite(tfMs) || tfMs <= 0) return {};
        const double minimum = options.minimum < 0 ? options.reference.median : options.minimum;
        Bar bar;
        for (size_t i = 0; i < rows.size() && i < Capacity;) {
            const auto first = rows[i];
            if (!valid(first)) { ++i; continue; }
            auto mark = sweep(rows, i, previousPrice);
            if (options.mode == Mode::Slot) {
                const double start = std::floor(mark.timeMs / tfMs) * tfMs;
                if (bar.used && start != bar.start) { flush(bar, minimum, options.reference.aggregateP95); bar = {}; }
                bar.start = start;
                mark.timeMs = start + tfMs / 2;
                accumulate(bar, mark);
            } else if (mark.notional >= minimum) append(mark);
        }
        if (bar.used) flush(bar, minimum, options.reference.aggregateP95);
        return {marks_.get(), count_};
    }
private:
    struct Bar {
        double start = 0;
        bool used = false;
        std::array<Mark, 2> sides{};
        std::array<double, 2> sizes{};
        std::array<size_t, 2> counts{};
        std::array<Mark, NotableCount + 2> notable{};
    };
    static bool valid(const Execution& t) {
        return std::isfinite(t.timeMs) && t.timeMs > 0 && std::isfinite(t.price * t.size) &&
               t.price > 0 && t.size > 0 && (t.side == 0 || t.side == 1);
    }
    static bool consecutive(std::string_view a, std::string_view b) {
        if (a.empty() || b.empty()) return false;
        uint64_t x = 0, y = 0;
        const auto ra = std::from_chars(a.data(), a.data() + a.size(), x);
        const auto rb = std::from_chars(b.data(), b.data() + b.size(), y);
        return ra.ec == std::errc{} && rb.ec == std::errc{} &&
               ra.ptr == a.data() + a.size() && rb.ptr == b.data() + b.size() &&
               x != std::numeric_limits<uint64_t>::max() && y == x + 1;
    }
    static bool joins(const Execution& a, const Execution& b) {
        return valid(b) && a.timeMs == b.timeMs && a.exchangeNs == b.exchangeNs &&
               a.side == b.side && consecutive(a.id, b.id);
    }
    template<class Rows> Mark sweep(const Rows& rows, size_t& i, double& previousPrice) {
        auto t = rows[i];
        Mark mark{t.timeMs, t.price, 0, t.price, t.price, t.side, 1, false};
        bool hasPrevious = previousPrice > 0;
        double size = 0;
        size_t levels = 0;
        do {
            t = rows[i++];
            const double n = t.price * t.size;
            mark.notional += n;
            size += t.size;
            mark.minPrice = std::min(mark.minPrice, t.price);
            mark.maxPrice = std::max(mark.maxPrice, t.price);
            mark.moving = mark.moving || (hasPrevious && t.price != previousPrice);
            hasPrevious = true;
            previousPrice = t.price;
            scratch_[levels++] = t.price;
        } while (i < rows.size() && i < Capacity && joins(t, rows[i]));
        std::sort(scratch_.get(), scratch_.get() + levels);
        mark.sweepLevels = size_t(std::unique(scratch_.get(), scratch_.get() + levels) - scratch_.get());
        mark.price = mark.notional / size;
        return mark;
    }
    static void accumulate(Bar& bar, const Mark& mark) {
        bar.used = true;
        auto& side = bar.sides[size_t(mark.side)];
        side.timeMs = mark.timeMs;
        side.side = mark.side;
        side.notional += mark.notional;
        bar.sizes[size_t(mark.side)] += mark.notional / mark.price;
        ++bar.counts[size_t(mark.side)];
        side.moving = side.notional == mark.notional ? mark.moving : side.moving || mark.moving;
        for (auto& notable : bar.notable) {
            if (mark.notional <= notable.notional) continue;
            auto carry = mark;
            for (auto* p = &notable; p != bar.notable.data() + bar.notable.size(); ++p) std::swap(carry, *p);
            break;
        }
    }
    void append(const Mark& mark) {
        if (count_ < MaxMarks && std::isfinite(mark.notional) && std::isfinite(mark.price))
            marks_[count_++] = mark;
    }
    void flush(const Bar& bar, double minimum, double aggregateP95) {
        for (size_t side = 0; side < 2; ++side) {
            auto mark = bar.sides[side];
            if (mark.notional <= 0 || mark.notional < minimum) continue;
            mark.price = mark.notional / bar.sizes[side];
            mark.minPrice = mark.maxPrice = mark.price;
            mark.aggregate = true;
            append(mark);
        }
        size_t emitted = 0;
        for (auto mark : bar.notable) {
            if (mark.notional > 0 && mark.notional >= minimum &&
                bar.counts[size_t(mark.side)] > 1) {
                mark.radiusCap = radius(bar.sides[size_t(mark.side)].notional, aggregateP95);
                append(mark);
                if (++emitted == NotableCount) break;
            }
        }
    }
    std::unique_ptr<Mark[]> marks_;
    std::unique_ptr<double[]> scratch_;
    size_t count_ = 0;
};
} // namespace chart::bubbles
