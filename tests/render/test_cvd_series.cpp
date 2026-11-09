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
TEST(CvdSeries, NoneAccumulatesInvisibleLoadedHistory) {
    CvdSeries series;
    ASSERT_TRUE(series.push(1000, 4, CvdSeries::resetStart(1000, CvdReset::None, 0), false));
    ASSERT_TRUE(series.push(86400000, -1, CvdSeries::resetStart(86400000, CvdReset::None, 0), true));
    ASSERT_EQ(series.count, 1); EXPECT_EQ(series.points[0].value, 3);
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
