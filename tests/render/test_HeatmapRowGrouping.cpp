#include <gtest/gtest.h>

#include "render/HeatmapRowGrouping.hpp"

using namespace heatmap_rows;

TEST(HeatmapRowGrouping, PicksSmallestOneTwoFiveStepThatReachesTarget) {
    EXPECT_EQ(rowsPerDisplayRow(20.0, 12.0), 1);   // zoomed in: base rows already big
    EXPECT_EQ(rowsPerDisplayRow(12.0, 12.0), 1);
    EXPECT_EQ(rowsPerDisplayRow(7.0, 12.0), 2);
    EXPECT_EQ(rowsPerDisplayRow(3.0, 12.0), 5);
    EXPECT_EQ(rowsPerDisplayRow(0.5, 12.0), 50);   // 20 * 0.5 = 10 < 12
    EXPECT_EQ(rowsPerDisplayRow(0.01, 12.0), 2000);
    EXPECT_EQ(rowsPerDisplayRow(1e-9, 12.0), kMaxRowGroup);
}

TEST(HeatmapRowGrouping, InvalidInputsFallBackToBaseTick) {
    EXPECT_EQ(rowsPerDisplayRow(0.0, 12.0), 1);
    EXPECT_EQ(rowsPerDisplayRow(-3.0, 12.0), 1);
    EXPECT_EQ(rowsPerDisplayRow(std::nan(""), 12.0), 1);
    EXPECT_EQ(rowsPerDisplayRow(3.0, 0.0), 1);
}

// With a $1 base tick and N=10, display rows must cover [$x0, $x9] price bands
// no matter where the recorded band's top sits.
TEST(HeatmapRowGrouping, GroupsSitOnAbsolutePriceGrid) {
    const double tick = 1.0;
    const int n = 10;
    for (double maxPrice : {100007.0, 100000.0, 99999.0, 100013.0}) {
        const int phase = rowPhase(maxPrice, tick, n);
        for (int r = 0; r < 40; ++r) {
            const int first = groupFirstRow(r, n, phase);
            EXPECT_LE(first, r);
            EXPECT_GT(first + n, r);
            // The group's lowest price (last row) is a multiple of n ticks.
            const double lowPrice = maxPrice - static_cast<double>(first + n - 1) * tick;
            EXPECT_DOUBLE_EQ(std::fmod(lowPrice, n * tick), 0.0) << "maxPrice=" << maxPrice << " r=" << r;
        }
    }
}

TEST(HeatmapRowGrouping, RowsInOneGroupShareTheirFirstRow) {
    const int n = 5;
    const int phase = rowPhase(250.0, 0.5, n);  // top index 500, phase 0
    EXPECT_EQ(phase, 0);
    // rel = -r; groups: r in [1..5] -> first 1, [6..10] -> first 6, r=0 alone in [-4..0].
    EXPECT_EQ(groupFirstRow(0, n, phase), -4);
    for (int r = 1; r <= 5; ++r) EXPECT_EQ(groupFirstRow(r, n, phase), 1);
    for (int r = 6; r <= 10; ++r) EXPECT_EQ(groupFirstRow(r, n, phase), 6);
}

TEST(HeatmapRowGrouping, NOfOneIsIdentity) {
    EXPECT_EQ(rowPhase(123.0, 1.0, 1), 0);
    EXPECT_EQ(groupFirstRow(17, 1, 0), 17);
}

TEST(HeatmapRowGrouping, TargetRowFollowsColumnWidthForSquareCells) {
    EXPECT_DOUBLE_EQ(targetRowPx(40.0, 2.0, 0.75), 30.0);  // big columns: big cells
    EXPECT_DOUBLE_EQ(targetRowPx(1.0, 2.0, 0.75), 2.0);    // zoomed out: fine lines, floor applies
    EXPECT_DOUBLE_EQ(targetRowPx(500.0, 2.0, 0.75), 64.0); // capped
    EXPECT_DOUBLE_EQ(targetRowPx(0.0, 2.0, 0.75), 2.0);    // unknown column width
    // $1 rows at 3 px with 8 px columns: target 6 px -> merge 2 rows.
    EXPECT_EQ(rowsPerDisplayRow(3.0, targetRowPx(8.0, 2.0, 0.75)), 2);
}
