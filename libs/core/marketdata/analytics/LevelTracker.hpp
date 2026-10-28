#pragma once

#include "WindowState.hpp"
#include <span>
#include <vector>

namespace sentinel::book {

struct TrackerConfig {
    std::size_t maxRows = 256;
    double minimumSize = 0;
    Milliseconds flickerWindowMs = 1000;
};
struct LevelInfo {
    bool present = false;
    Milliseconds seenForMs = 0;
    std::uint64_t flickers = 0;
    Milliseconds sinceLastFlickerMs = -1; // -1: no flicker counted yet
    bool large = false;           // at the reference multiple at the last observation
    Milliseconds largeForMs = 0;  // how long it has been large (0 while not large)
};

// What a large level is measured against: each side's median size (indexed by
// sideIndex; 0 means no level of that side is large) and the multiple.
struct LargeReference {
    std::array<double, 2> medians{};
    double multiple = 4;
};

// The median of the positive finite values (even count: the mean of the middle two),
// 0 when there are none. Reorders `values`.
double positiveMedian(std::span<double> values);

// Presence means positive size >= minimumSize. Age is continuous observed time
// on that side; a disappearance restarts age on return. Flickers count returns
// within flickerWindowMs (inclusive), cumulatively while the price stays observed.
// Overlapping prices retain age/count across a scroll; prices leaving the window
// become unknown and lose tracking. Scrolling away/back is never a flicker.
// Invalid sizes also break observation continuity, without a disappearance.
// Large levels compare with a per-side median: by default the median positive visible
// size on the SAME side, including small levels below the presence threshold (even
// medians use the mean), at multiple 4; or a caller's LargeReference.
// Large age is the time a level has been at or above the multiple. It starts when the
// level crosses up; a drop below (or a disappearance) followed by a crossing within
// flickerWindowMs (inclusive) keeps it, a longer one restarts it. It follows the price
// across a scroll like presence age.
class LevelTracker {
public:
    explicit LevelTracker(TrackerConfig config = {});
    UpdateStatus update(const Snapshot& snapshot);
    UpdateStatus update(const Snapshot& snapshot, const LargeReference& reference);
    void reset();
    LevelInfo level(std::size_t row, Side side) const;
    double median(Side side) const { return medians_[sideIndex(side)]; }
    bool isLarge(std::size_t row, Side side, double multiple = 4) const;

private:
    struct State {
        bool present = false;
        bool vanished = false;
        Milliseconds since = 0;
        Milliseconds disappeared = 0;
        Milliseconds lastFlicker = 0;
        std::uint64_t flickers = 0;
        bool large = false;
        bool largeLapsed = false;
        Milliseconds largeSince = 0;
        Milliseconds largeEnded = 0;
    };
    using RowState = std::array<State, 2>;
    UpdateStatus update(const Snapshot& snapshot, const LargeReference* reference);
    void observe(State& state, double size, Milliseconds now);
    void observeLarge(State& state, bool large, Milliseconds now) const;
    void trackRows(const Snapshot& snapshot, std::int64_t firstTick);
    void computeMedians(const Snapshot& snapshot);
    void trackLarge(const Snapshot& snapshot, double multiple);
    bool largeSize(double size, Side side, double multiple) const;

    TrackerConfig config_;
    detail::WindowState window_;
    std::vector<RowState> states_;
    std::vector<RowState> scratch_;
    std::vector<LevelSizes> sizes_;
    std::vector<double> sorted_;
    std::array<double, 2> medians_{};
};

} // namespace sentinel::book
