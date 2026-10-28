#pragma once
#include "chart/layers/LadderLayer.hpp"
#include "marketdata/analytics/LevelTracker.hpp"
#include <cstdint>
#include <vector>

namespace chart {
// A flicker younger than this colours its wall bar hot.
constexpr int64_t kFlickerHotMs = 10'000;

// The largest native level per [side][direction] outside the drawn window, by real
// price: above when price >= drawnMax (the top edge is exclusive, as a bin's upper
// bound), below when price < drawnMin; a level at exactly drawnMin is on screen.
struct WallCandidates {
    std::array<std::array<WallCandidate, 2>, 2> cells{}; // as LadderSnapshot::walls
    double drawnMin = 0, drawnMax = 0;
    void begin(double minPrice, double maxPrice);
    void consider(int side, double price, double size);
    void setCapped(bool bid, bool ask);
};

// GUI thread. Feeds the published bin grid of complete snapshots into a
// sentinel::book::LevelTracker and writes the wall fields (flags, seen-for,
// flicker counts, medians, off-screen candidates) back into the snapshot.
// Allocates only at construction.
class LadderTracker {
public:
    LadderTracker();
    void reset();
    // A valid and complete snapshot is an observation; an incomplete one keeps the
    // state (its unknown rows are zero-filled, never "empty"); an invalid one resets.
    // `nowMs` is a monotonic clock; stamps are forced strictly increasing.
    void publish(LadderSnapshot& s, uint64_t series, int64_t nowMs, double multiple, const WallCandidates& walls);
    uint64_t rejectedUpdates() const { return rejected_; }
    uint64_t acceptedUpdates() const { return accepted_; }
private:
    bool observe(LadderSnapshot& s, uint64_t series, int64_t nowMs);
    void markRows(LadderSnapshot& s, double multiple) const;
    void markWalls(LadderSnapshot& s, double multiple, const WallCandidates& walls) const;
    sentinel::book::LevelTracker tracker_;
    std::vector<sentinel::book::LevelSizes> sizes_;
    int64_t lastStamp_ = -1;
    uint64_t warnedSeries_ = ~uint64_t(0), rejected_ = 0, accepted_ = 0;
};
} // namespace chart
