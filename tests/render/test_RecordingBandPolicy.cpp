#include <gtest/gtest.h>
#include "render/RecordingBandPolicy.hpp"

using namespace recording_view;

TEST(RecordingBandPolicy, MarginExitAndTickStep) {
    View view{0, 6'000'000, 100, 200, 1000, 500, false};
    const auto band = requestBand(view, 2);
    EXPECT_LE(band.minPrice, 50);
    EXPECT_GE(band.maxPrice, 250);
    EXPECT_DOUBLE_EQ(band.idealTick, 1);  // $100 over 500 px -> 2 px rows need $0.4 -> native $1
    EXPECT_FALSE(needsReband(view, band, band));
    view.minPrice = band.minPrice - 1;
    view.maxPrice = view.minPrice + 100;
    EXPECT_TRUE(needsReband(view, band, requestBand(view, 2)));
    // Zooming time does not change the tick: only price-per-pixel does.
    view = {0, 6'000'000, 100, 200, 1000, 500, false};
    view.endMs /= 2;
    EXPECT_DOUBLE_EQ(requestBand(view, 2).idealTick, 1);
    // Zooming price out 20x does: $2,000 over 500 px -> $8 -> $10.
    view = {0, 6'000'000, 100, 2'100, 1000, 500, false};
    const auto zoomedOut = requestBand(view, 2);
    EXPECT_DOUBLE_EQ(zoomedOut.idealTick, 10);
    EXPECT_TRUE(needsReband(view, band, zoomedOut));
}

TEST(RecordingBandPolicy, TrailingDebounceAndCancellation) {
    Debounce state;
    EXPECT_FALSE(state.ready(1000));
    state.changed(100);
    EXPECT_FALSE(state.ready(249));
    EXPECT_TRUE(state.ready(250));
    state.changed(240);
    EXPECT_FALSE(state.ready(389));
    EXPECT_TRUE(state.ready(390));
    state.cancel();
    EXPECT_FALSE(state.ready(500));
}

// A timer that fires 1 ms early (149 ms on Windows under load) must re-arm for
// the remainder, never for zero or a full new period.
TEST(RecordingBandPolicy, RemainingTimeRearmsAnEarlyTimer) {
    Debounce state;
    state.changed(100);
    EXPECT_EQ(state.remainingMs(249), 1);
    EXPECT_EQ(state.remainingMs(200), 50);
    EXPECT_EQ(state.remainingMs(100), kDebounceMs);
    EXPECT_EQ(state.remainingMs(400), 1);   // already late: fire as soon as possible
    EXPECT_EQ(state.remainingMs(50), kDebounceMs); // clock before the change: at most one period
}

TEST(RecordingBandPolicy, IdealTickIsLadderTickForTwoPixelRows) {
    // $100 over 500 px: $0.2/px -> 2 px rows need $0.4 -> native $1.
    EXPECT_DOUBLE_EQ(idealTick(View{0, 60'000, 83'000, 83'100, 1000, 500, false}, 2), 1.0);
    // $2,000 over 500 px: $4/px -> $8 -> $10.
    EXPECT_DOUBLE_EQ(idealTick(View{0, 60'000, 82'000, 84'000, 1000, 500, false}, 2), 10.0);
    // $5,000 over 500 px: $10/px -> $20 (ladder has 20 and 25).
    EXPECT_DOUBLE_EQ(idealTick(View{0, 60'000, 80'000, 85'000, 1000, 500, false}, 2), 20.0);
    // $11,000 over 500 px: $22/px -> $44 -> $50. Column width plays no part.
    EXPECT_DOUBLE_EQ(idealTick(View{0, 60'000, 78'000, 89'000, 20, 500, false}, 2), 50.0);
    EXPECT_FALSE(requestBand({}, 2).valid());
    EXPECT_DOUBLE_EQ(idealTick(View{0, 60'000, 1, 2, 10, 10, false}, 0), 0.0);
}

TEST(RecordingBandPolicy, NonnegativeProtocolBandRetainsRequestedResolution) {
    View view{0, 6'000'000, 1, 2, 1000, 500, false};
    const auto band = requestBand(view, 2);
    EXPECT_GE(band.minPrice, 0);
    EXPECT_LE(band.maxPrice - band.minPrice, (kRows - 2) * band.idealTick);
    EXPECT_TRUE(band.valid());
}

TEST(RecordingBandPolicy, FirstPageCoversVisibleColumnsWithSmallMargin) {
    View view{0, 158 * 60'000, 100, 200, 1000, 500, true};
    EXPECT_EQ(firstPageColumns(view, 60'000, 976), 174);
    view.endMs = 2000 * 60'000;
    EXPECT_EQ(firstPageColumns(view, 60'000, 976), 976);
    EXPECT_EQ(firstPageColumns({}, 60'000, 976), 0);
}
