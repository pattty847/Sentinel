#include "DomGrouping.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <algorithm>
#include <cmath>

namespace dom {
bool GroupChoices::contains(int64_t group) const
{
    const auto l = list();
    return std::find(l.begin(), l.end(), group) != l.end();
}

int64_t GroupChoices::nearest(double group) const
{
    if (count == 0 || !(group > 0)) return 1;
    int64_t best = groups[0];
    double distance = std::abs(std::log(double(best) / group));
    for (int i = 1; i < count; ++i) {
        const double d = std::abs(std::log(double(groups[i]) / group));
        if (d < distance - 1e-12) {
            best = groups[i];
            distance = d;
        }
    }
    return best;
}

int64_t wholeMultiple(double x, double unit)
{
    if (!std::isfinite(x) || !std::isfinite(unit) || !(unit > 0) || !(x > 0)) return 0;
    const double ratio = x / unit;
    if (ratio > 1e15) return 0;
    const int64_t m = std::llround(ratio);
    return std::abs(ratio - double(m)) < 1e-6 ? m : 0;
}

GroupChoices groupChoices(double nativeTick, double mid)
{
    GroupChoices out;
    out.groups[out.count++] = 1;
    if (!std::isfinite(nativeTick) || !(nativeTick > 0)) return out;
    // Integer price units in which the native tick is whole (the presets' units).
    double scale = 1;
    int64_t native = 0;
    for (int d = 0; d <= 12 && native == 0; ++d, scale *= 10) native = wholeMultiple(nativeTick * scale, 1);
    if (native == 0) return out;
    for (const int64_t preset : heatmap::presetLadderUnits()) {
        if (preset <= native || preset % native != 0) continue;
        const int64_t group = preset / native;
        if (group > MaxGroup || (mid > 0 && double(group) * nativeTick > mid * MaxShareOfMid)) break;
        if (out.count == GroupChoices::Capacity) break;
        out.groups[out.count++] = group;
    }
    return out;
}

LinkGroup linkGroup(double heatmapTick, double nativeTick, const GroupChoices& choices)
{
    const int64_t group = wholeMultiple(heatmapTick, nativeTick);
    if (group > 0 && choices.contains(group)) return {group, true};
    return {choices.nearest(nativeTick > 0 ? heatmapTick / nativeTick : 0), false};
}

int AutoGroupRule::filledRows(const AutoBook& book, bool bid, int64_t group)
{
    const auto side = bid ? book.bids : book.asks;
    const std::ptrdiff_t top = bid ? book.bidTop : book.askTop;
    if (top < 0 || size_t(top) >= side.size() || group < 1) return 0;
    const int64_t size = int64_t(side.size());
    const int64_t touch = floorDiv(book.base + top, group);
    int filled = 0;
    for (int j = 0; j < RowsPerSide; ++j) {
        const int64_t bin = bid ? touch - j : touch + j;
        // Native slots of the bin, limited to the replica and the touch side.
        int64_t lo = bin * group - book.base, hi = lo + group - 1;
        if (bid) hi = std::min<int64_t>(hi, top);
        else lo = std::max<int64_t>(lo, top);
        lo = std::max<int64_t>(lo, 0);
        hi = std::min<int64_t>(hi, size - 1);
        for (int64_t i = lo; i <= hi; ++i) {
            if (std::isfinite(side[size_t(i)]) && side[size_t(i)] > 0) {
                ++filled;
                break;
            }
        }
    }
    return filled;
}

int64_t AutoGroupRule::target(const AutoBook& book, const GroupChoices& choices, int64_t current)
{
    const bool bid = book.bidTop >= 0, ask = book.askTop >= 0;
    if ((!bid && !ask) || choices.count == 0) return 0;
    for (const int64_t group : choices.list()) {
        const int needed = int(std::ceil((group < current ? FinerFill : TargetFill) * RowsPerSide - 1e-9));
        if ((!bid || filledRows(book, true, group) >= needed) && (!ask || filledRows(book, false, group) >= needed))
            return group;
    }
    return choices.groups[size_t(choices.count - 1)];
}

int64_t GroupHold::step(int64_t target, int64_t nowMs)
{
    if (target <= 0) return m_current;
    if (m_current == 0) {
        m_current = target;
        m_pending = 0;
        return m_current;
    }
    if (target == m_current) {
        m_pending = 0;
        return m_current;
    }
    if (target != m_pending) {
        m_pending = target;
        m_since = nowMs;
        return m_current;
    }
    if (nowMs - m_since >= HoldMs) {
        m_current = target;
        m_pending = 0;
        ++m_changes;
    }
    return m_current;
}
} // namespace dom
