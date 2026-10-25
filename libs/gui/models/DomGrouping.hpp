#pragma once
// DOM row grouping, client side. A grouped row sums `group` native book levels on the
// absolute grid [k * group, (k + 1) * group) of native level indices (lower inclusive,
// upper exclusive), as the chart-edge ladder bins (BookLadderFeed). Qt-free; GUI thread.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dom {
// Coarsest row the DOM offers, in native levels (BTC 0.1 -> $100).
constexpr int64_t MaxGroup = 1000;
// A row coarser than this share of the mid is the DOM's too-coarse state (DomModel).
constexpr double MaxShareOfMid = 0.01;

inline int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

struct GroupChoices {
    static constexpr int Capacity = 32;
    std::array<int64_t, Capacity> groups{};
    int count = 0;
    std::span<const int64_t> list() const { return {groups.data(), size_t(count)}; }
    bool contains(int64_t group) const;
    // The choice closest to `group` by ratio (1 when empty); ties take the finer one.
    int64_t nearest(double group) const;
};
// Native (1) first, then every heatmap tick preset ({1, 2, 2.5, 5} x 10^k,
// heatmap::presetLadderUnits) that is a whole multiple of the native tick, at most
// MaxGroup levels and, with a known mid, at most MaxShareOfMid of it.
GroupChoices groupChoices(double nativeTick, double mid);

// The integer m with x == m * unit within 1e-6 units; 0 if there is none.
int64_t wholeMultiple(double x, double unit);

struct LinkGroup {
    int64_t group = 1;
    bool exact = false; // false: the heatmap tick is not a valid choice; nearest used
};
// The heatmap's tick as a group. A tick that is not a whole multiple of the native
// tick (heatmap layer off, INV-178), or not offered, falls back to the nearest choice.
LinkGroup linkGroup(double heatmapTick, double nativeTick, const GroupChoices& choices);

// Dense replica view: native slot i is level base + i; tops are slot indices, -1 if absent.
struct AutoBook {
    std::span<const double> bids, asks;
    int64_t base = 0;
    std::ptrdiff_t bidTop = -1, askTop = -1;
};
// Auto target: the smallest choice at which at least TargetFill of the RowsPerSide
// rows on each present side, counted from the touch row outward, hold size. None:
// the coarsest choice. An empty book has no target (0).
struct AutoGroupRule {
    static constexpr int RowsPerSide = 20;
    static constexpr double TargetFill = 0.6;
    static int64_t target(const AutoBook& book, const GroupChoices& choices);
    // Rows of the RowsPerSide from the touch row that hold size, on one side.
    static int filledRows(const AutoBook& book, bool bid, int64_t group);
};

// A hold: a new target replaces the current group only after HoldMs unchanged, so
// a book or zoom near a threshold does not flip between two sizes. The first target
// (after reset) applies at once; a target <= 0 keeps the current group.
class GroupHold {
public:
    static constexpr int64_t HoldMs = 750;
    int64_t step(int64_t target, int64_t nowMs);
    void reset() { m_current = m_pending = 0; }
    int64_t current() const { return m_current; }
    uint64_t changes() const { return m_changes; }
private:
    int64_t m_current = 0, m_pending = 0, m_since = 0;
    uint64_t m_changes = 0;
};
} // namespace dom
