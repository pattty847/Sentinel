#include <gtest/gtest.h>
#include "render/RecordingBandPolicy.hpp"

using namespace recording_view;

TEST(RecordingBandPolicy, MarginExitAndTickStep) {
    View view{0, 6'000'000, 100, 200, 1000, 500, false};
    const auto band = requestBand(view, 60'000, 2, 0.75);
    EXPECT_LE(band.minPrice, 50);
    EXPECT_GE(band.maxPrice, 250);
    EXPECT_DOUBLE_EQ(band.idealTick, 2);
    EXPECT_FALSE(needsReband(view, band, band));
    view.minPrice = band.minPrice - 1;
    view.maxPrice = view.minPrice + 100;
    EXPECT_TRUE(needsReband(view, band, requestBand(view, 60'000, 2, 0.75)));
    view = {0, 6'000'000, 100, 200, 1000, 500, false};
    view.endMs /= 2;
    const auto zoomed = requestBand(view, 60'000, 2, 0.75);
    EXPECT_DOUBLE_EQ(zoomed.idealTick, 5);
    EXPECT_TRUE(needsReband(view, band, zoomed));
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

TEST(RecordingBandPolicy, SquareCellTargetAgreesWithRowGrouping) {
    const double baseTick = 0.5, pxPerBase = 2.5, columnPx = 10;
    const auto target = heatmap_rows::squareCellTick(baseTick / pxPerBase, columnPx, 2, 0.75);
    const int group = heatmap_rows::rowsPerDisplayRow(pxPerBase, heatmap_rows::targetRowPx(columnPx, 2, 0.75));
    EXPECT_GE(group * baseTick, target);
    EXPECT_DOUBLE_EQ(target, 1.5);
    EXPECT_FALSE(requestBand({}, 60'000, 2, 0.75).valid());
    EXPECT_EQ(stepTick(std::numeric_limits<double>::infinity()), 0);
}

TEST(RecordingBandPolicy, NonnegativeProtocolBandRetainsRequestedResolution) {
    View view{0, 6'000'000, 1, 2, 1000, 500, false};
    const auto band = requestBand(view, 60'000, 2, 0.75);
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
