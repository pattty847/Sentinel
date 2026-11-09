#include "chart/core/CvdSeries.hpp"
#include <gtest/gtest.h>
using namespace chart;
TEST(CvdSeries, UtcDayAndTpoSessionReset) {
    for (auto rule : {CvdReset::UtcDay, CvdReset::TpoSession}) {
        CvdSeries series;
        const int64_t boundary = rule == CvdReset::UtcDay ? 86400000 : 3600000;
        for (int i = -1; i < 2; ++i) {
            const auto time = boundary + i * 1000;
            ASSERT_TRUE(series.push(time, 3, CvdSeries::resetStart(time, rule, i < 0 ? 0 : boundary), true));
        }
        EXPECT_EQ(series.points[0].value, 3); EXPECT_EQ(series.points[1].value, 3);
        EXPECT_EQ(series.points[2].value, 6);
    }
    EXPECT_EQ(CvdSeries::resetStart(-1, CvdReset::UtcDay, 0), -86400000);
}
TEST(CvdSeries, VisibleAnchorsAtZeroAndReplacesTheFormingBar) {
    CvdSeries series;
    EXPECT_FALSE(series.pushVisible(1000, 100, 1500, 5000));
    ASSERT_TRUE(series.pushVisible(2000, 9, 1500, 5000));
    EXPECT_EQ(series.points[0].timeMs, 2000); EXPECT_EQ(series.points[0].value, 0);
    series.pushVisible(2000, 12, 1500, 5000);
    ASSERT_EQ(series.count, 1); EXPECT_EQ(series.points[0].value, 0);
    series.pushVisible(3000, 4, 1500, 5000);
    series.pushVisible(3000, -2, 1500, 5000);
    ASSERT_EQ(series.count, 2); EXPECT_EQ(series.points[1].value, -2);
    series.pushVisible(4000, 3, 1500, 5000);
    EXPECT_EQ(series.points[2].value, 1);
    EXPECT_FALSE(series.pushVisible(5000, 99, 1500, 5000));
    EXPECT_EQ(series.count, 3);
}
TEST(CvdSeries, PanningVisibleAnchorPreservesOverlappingDifferences) {
    CvdSeries original, panned;
    for (int i = 0; i < 8; ++i) {
        original.pushVisible(i * 1000, i - 1, 0, 8000);
        panned.pushVisible(i * 1000, i - 1, 3000, 8000);
    }
    ASSERT_EQ(panned.count, 5); EXPECT_EQ(panned.points[0].value, 0);
    const auto offset = original.points[3].value;
    EXPECT_NE(offset, 0);
    for (int i = 0; i < panned.count; ++i) {
        EXPECT_EQ(original.points[size_t(i + 3)].value - panned.points[size_t(i)].value, offset);
        for (int j = 0; j < panned.count; ++j)
            EXPECT_EQ(original.points[size_t(i + 3)].value - original.points[size_t(j + 3)].value,
                      panned.points[size_t(i)].value - panned.points[size_t(j)].value);
    }
}
TEST(CvdSeries, PagingDoesNotChangeValuesAfterResetBoundary) {
    for (auto rule : {CvdReset::UtcDay, CvdReset::TpoSession}) {
        CvdSeries series;
        for (int older : {0, 1}) {
            series.clear();
            if (older) series.push(1000, 500, CvdSeries::resetStart(1000, rule, 0), false);
            series.push(86400000, 2, CvdSeries::resetStart(86400000, rule, 86400000), true);
            series.push(86401000, -3, CvdSeries::resetStart(86401000, rule, 86400000), true);
            ASSERT_EQ(series.count, 2); EXPECT_EQ(series.points[1].value, -1);
        }
    }
}
TEST(CvdSeries, FormingBarReplacesAndNextBarUsesReplacement) {
    CvdSeries series;
    series.push(0, 5, 0, true); series.push(1000, 3, 0, true);
    series.push(1000, -2, 0, true);
    ASSERT_EQ(series.count, 2); EXPECT_EQ(series.points[1].value, 3);
    series.push(2000, 1, 0, true); EXPECT_EQ(series.points[2].value, 4);
}
TEST(CvdSeries, AutoscaleIncludesZeroAndFlatIsFinite) {
    CvdSeries series;
    EXPECT_TRUE(std::isfinite(series.scale().fraction(0)));
    series.push(0, 0, 0, true); EXPECT_DOUBLE_EQ(series.scale().fraction(0), 0.5);
    series.push(0, 8, 0, true); EXPECT_EQ(series.scale().min, 0); EXPECT_EQ(series.scale().max, 8);
    series.push(0, -8, 0, true); EXPECT_EQ(series.scale().min, -8); EXPECT_EQ(series.scale().max, 0);
    EXPECT_FALSE(series.push(1, std::numeric_limits<double>::infinity(), 0, true));
    EXPECT_EQ(series.count, 1);
}
TEST(CvdSeries, AutoscaleExcludesPointsOutsideTheCurrentCamera) {
    CvdSeries series;
    series.push(0, 100, 0, true); series.push(1000, -95, 0, true);
    const auto scale = series.scale(1000, 2000, 1000);
    EXPECT_EQ(scale.min, 0); EXPECT_EQ(scale.max, 5);
}

#include "chart/core/CvdHistory.hpp"
TEST(CvdHistory, ResetAnchorsUseEarliestVisibleBarAndOffLeavesWindowAlone) {
    constexpr int64_t hour = 3600000;
    EXPECT_EQ(cvdHistoryAnchor(12 * hour, 60000, 96, CvdReset::UtcDay, 4), 0);
    EXPECT_EQ(cvdHistoryAnchor(14 * hour, 60000, 96, CvdReset::TpoSession, 0), 13 * hour);
    EXPECT_EQ(cvdHistoryAnchor(12 * hour, 60000, 0, CvdReset::UtcDay, 4), -1);
    EXPECT_EQ(cvdHistoryAnchor(12 * hour, 60000, 96, CvdReset::Visible, 4), -1);
    EXPECT_EQ(cvdHistoryAnchor(86400000 + 5, 100, 96, CvdReset::UtcDay, 4), 86400000);
}
TEST(CvdSeries, EveryVisibleResetPeriodTracksItsOwnLoadedStart) {
    CvdSeries series;
    series.push(9 * 3600000, 4, 0, true);
    series.push(86400000, 2, 86400000, true);
    EXPECT_EQ(series.incompleteStart(), 9 * 3600000);
    EXPECT_EQ(series.points[1].loadedFromMs, 86400000);
    series.clear();
    series.push(9 * 3600000, 4, 0, true, 0); // complete coverage proves earlier empty bars
    EXPECT_EQ(series.incompleteStart(), -1);
}
