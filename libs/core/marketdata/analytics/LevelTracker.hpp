#pragma once

#include "WindowState.hpp"
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
};

// Presence means positive size >= minimumSize. Age is continuous observed time
// on that side; a disappearance restarts age on return. Flickers count returns
// within flickerWindowMs (inclusive), cumulatively while the price stays observed.
// Overlapping prices retain age/count across a scroll; prices leaving the window
// become unknown and lose tracking. Scrolling away/back is never a flicker.
// Invalid sizes also break observation continuity, without a disappearance.
// Large levels compare with the median positive visible size on the SAME side,
// including small levels below the presence threshold. Even medians use the mean.
class LevelTracker {
public:
    explicit LevelTracker(TrackerConfig config = {});
    UpdateStatus update(const Snapshot& snapshot);
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
    };
    using RowState = std::array<State, 2>;
    void observe(State& state, double size, Milliseconds now);
    void trackRows(const Snapshot& snapshot, std::int64_t firstTick);
    void computeMedians(const Snapshot& snapshot);

    TrackerConfig config_;
    detail::WindowState window_;
    std::vector<RowState> states_;
    std::vector<RowState> scratch_;
    std::vector<LevelSizes> sizes_;
    std::vector<double> sorted_;
    std::array<double, 2> medians_{};
};

} // namespace sentinel::book
