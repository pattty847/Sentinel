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
    double median = 0, p95 = 1;
    bool operator==(const Reference&) const = default;
};
struct Options {
    Mode mode = Mode::Individual;
    Reference reference;
    double minimum = 0;
    bool operator==(const Options&) const = default;
};
struct Mark {
    double timeMs = 0, price = 0, notional = 0;
    double minPrice = 0, maxPrice = 0;
    int side = 0;
    size_t sweepLevels = 1;
    bool moving = true, aggregate = false;
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
    return std::clamp(9 * std::sqrt(notional / reference), 2.0, 18.0);
}
class ReferenceStatistics {
public:
    static constexpr size_t Capacity = 100000;
    ReferenceStatistics() : scratch_(std::make_unique<double[]>(Capacity)) {}
    template<class Rows> Reference reference(const Rows& rows) {
        size_t n = 0;
        for (size_t i = 0; i < rows.size() && n < Capacity; ++i) {
            const auto t = rows[i];
            if (std::isfinite(t.price * t.size) && t.price > 0 && t.size > 0) scratch_[n++] = t.price * t.size;
        }
        if (!n) return {};
        std::sort(scratch_.get(), scratch_.get() + n);
        return {scratch_[(n - 1) / 2], scratch_[size_t(std::ceil(.95 * double(n))) - 1]};
    }
private:
    std::unique_ptr<double[]> scratch_;
};
// Scratch and output are allocated at construction, never while publishing or drawing.
class BubblePlacement {
public:
    static constexpr size_t Capacity = 100000;
    static constexpr size_t NotableCount = 3; // Largest merged executions in each live bar.
    BubblePlacement() : marks_(std::make_unique<Mark[]>(Capacity * 2)),
                        scratch_(std::make_unique<double[]>(Capacity)) {}
    // Rows are event-time ordered and already windowed; options.mode is resolved by PlacementMode.
    template<class Rows> std::span<const Mark> build(const Rows& rows, const Options& options,
                                                    double tfMs, double previousPrice = 0) {
        count_ = 0;
        if (!std::isfinite(tfMs) || tfMs <= 0) return {};
        const double minimum = options.minimum > 0 ? options.minimum : options.reference.median;
        Bar bar;
        for (size_t i = 0; i < rows.size() && i < Capacity;) {
            const auto first = rows[i];
            if (!valid(first)) { ++i; continue; }
            auto mark = sweep(rows, i, previousPrice);
            if (options.mode == Mode::Slot) {
                const double start = std::floor(mark.timeMs / tfMs) * tfMs;
                if (bar.used && start != bar.start) { flush(bar, minimum); bar = {}; }
                bar.start = start;
                mark.timeMs = start + tfMs / 2;
                accumulate(bar, mark);
            } else if (mark.notional >= minimum) append(mark);
        }
        if (bar.used) flush(bar, minimum);
        return {marks_.get(), count_};
    }
private:
    struct Bar {
        double start = 0;
        bool used = false;
        std::array<Mark, 2> sides{};
        std::array<double, 2> sizes{};
        std::array<Mark, NotableCount> notable{};
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
        double size = 0;
        size_t levels = 0;
        do {
            t = rows[i++];
            const double n = t.price * t.size;
            mark.notional += n;
            size += t.size;
            mark.minPrice = std::min(mark.minPrice, t.price);
            mark.maxPrice = std::max(mark.maxPrice, t.price);
            mark.moving = mark.moving || t.price != previousPrice;
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
        side.moving = side.notional == mark.notional ? mark.moving : side.moving || mark.moving;
        for (auto& notable : bar.notable) {
            if (mark.notional <= notable.notional) continue;
            auto carry = mark;
            for (auto* p = &notable; p != bar.notable.data() + NotableCount; ++p) std::swap(carry, *p);
            break;
        }
    }
    void append(const Mark& mark) {
        if (count_ < Capacity * 2 && std::isfinite(mark.notional) && std::isfinite(mark.price))
            marks_[count_++] = mark;
    }
    void flush(const Bar& bar, double minimum) {
        for (size_t side = 0; side < 2; ++side) {
            auto mark = bar.sides[side];
            if (mark.notional <= 0 || mark.notional < minimum) continue;
            mark.price = mark.notional / bar.sizes[side];
            mark.minPrice = mark.maxPrice = mark.price;
            mark.aggregate = true;
            append(mark);
        }
        for (const auto& mark : bar.notable)
            if (mark.notional > 0 && mark.notional >= minimum) append(mark);
    }
    std::unique_ptr<Mark[]> marks_;
    std::unique_ptr<double[]> scratch_;
    size_t count_ = 0;
};
} // namespace chart::bubbles
