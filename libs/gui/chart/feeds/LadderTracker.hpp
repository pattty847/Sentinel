#pragma once
#include "chart/layers/LadderLayer.hpp"
#include "marketdata/analytics/LevelTracker.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace chart {
// A flicker younger than this colours its wall bar hot.
constexpr int64_t kFlickerHotMs = 10'000;

// One publication's wall reference (each side's median and the multiple) and, per
// [side][direction] outside the drawn window, the nearest native level of at least
// multiple x its side's median, by real price: above when price >= drawnMax (the top
// edge is exclusive, as a bin's upper bound), below when price < drawnMin; a level at
// exactly drawnMin is on screen. A side with no positive median has no walls.
struct WallCandidates {
    std::array<std::array<WallCandidate, 2>, 2> cells{}; // as LadderSnapshot::walls
    sentinel::book::LargeReference reference;
    double drawnMin = 0, drawnMax = 0;
    void begin(double minPrice, double maxPrice, const sentinel::book::LargeReference& wallReference);
    void consider(int side, double price, double size);
    void setCapped(bool bid, bool ask);
};

// GUI thread. Feeds the published bin grid of complete snapshots into a
// sentinel::book::LevelTracker against the feed's wall reference and writes the wall
// fields (flags, wall age, flicker counts, medians, off-screen walls) back into the
// snapshot. Ages survive any view change on the same bin grid (a pan, a resize, a
// price zoom that keeps the display tick); another display tick is a new series.
// Allocates only at construction.
class LadderTracker {
public:
    LadderTracker();
    void reset();
    // A valid and complete snapshot is an observation; an incomplete one keeps the
    // state (its unknown rows are zero-filled, never "empty"); an invalid one resets.
    // `nowMs` is a monotonic clock; stamps are forced strictly increasing.
    void publish(LadderSnapshot& s, uint64_t series, int64_t nowMs, const WallCandidates& walls);
    // The wall reference of the last accepted observation (zero after a reset).
    std::array<double, 2> medians() const {
        return {tracker_.median(sentinel::book::Side::Bid), tracker_.median(sentinel::book::Side::Ask)};
    }
    uint64_t rejectedUpdates() const { return rejected_; }
    uint64_t acceptedUpdates() const { return accepted_; }
private:
    bool observe(LadderSnapshot& s, uint64_t series, int64_t nowMs, const sentinel::book::LargeReference& reference);
    void markRows(LadderSnapshot& s) const;
    sentinel::book::LevelTracker tracker_;
    std::vector<sentinel::book::LevelSizes> sizes_;
    int64_t lastStamp_ = -1;
    uint64_t warnedSeries_ = ~uint64_t(0), rejected_ = 0, accepted_ = 0;
};
} // namespace chart
