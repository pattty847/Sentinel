#include "chart/core/LivePlacement.hpp"
#include "render/ChartRaster.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include "chart/core/TimeframeSwitch.hpp"

namespace {
chart::TimeframeSwitchInputs inputs() {
    chart::TimeframeSwitchInputs in;
    in.previousTfMs = 1000; in.nextTfMs = 2000;
    in.oldStart = 0; in.oldEnd = 10000; in.previousLiveEndMs = 5000; in.nextLiveEndMs = 6000;
    in.keepColumns = true;
    in.nominalLiveEnd = [](int64_t) { return 8000; };
    in.restAt = [](int64_t end, int64_t span) {
        chart_raster::RasterInputs r;
        r.timeStart = end - span; r.timeEnd = end;
        r.minPrice = 90; r.maxPrice = 110; r.tfMs = 2000; r.tick = 1;
        r.itemWidthLogical = 800; r.itemHeightLogical = 400; r.anchorFracX = 1;
        return chart_raster::computeRaster(r, {});
    };
    in.drawnBefore = in.restAt(10000, 20000);
    in.drawnBefore.tfMs = 1000;
    in.drawnBefore.leftF = 0;
    return in;
}
TEST(TimeframeSwitchPlan, NoOpAndIneligible) {
    auto in = inputs(); in.nextTfMs = in.previousTfMs;
    EXPECT_FALSE(chart::planTimeframeSwitch(in).apply);
    in.nextTfMs = 2000; in.keepColumns = false;
    EXPECT_FALSE(chart::planTimeframeSwitch(in).apply);
}
TEST(TimeframeSwitchPlan, ManualDrawnAndStoredFractions) {
    auto in = inputs();
    auto p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.start, -4000); EXPECT_EQ(p.end, 16000);
    EXPECT_DOUBLE_EQ(p.anchorFracX, .45);
    in.drawnBefore.leftF = 80; // Now drawn at .35 although stored fraction is .45.
    p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.start, -2000); EXPECT_EQ(p.end, 18000);
    EXPECT_DOUBLE_EQ(p.anchorFracX, 1.0 - .65);
    in.drawnBefore.valid = false;
    p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.end, 16000);
    in.previousLiveEndMs = 20000;
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 10000);
    in.drawnBefore.valid = true;
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 10000);
}
TEST(TimeframeSwitchPlan, FollowExactPixelsAndCandidateSearch) {
    auto in = inputs(); in.following = true;
    auto p = chart::planTimeframeSwitch(in);
    ASSERT_TRUE(p.followPadding);
    EXPECT_EQ(p.start, -4000); EXPECT_EQ(p.end, 16000);
    EXPECT_EQ(p.followPadding->padMs, 10000);
    auto rest = in.restAt;
    in.restAt = [rest](int64_t end, int64_t span) {
        auto c = rest(end, span);
        if (end == 16000) c.valid = false;
        return c;
    };
    p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.end, 15999); // e fails, e - 1 matches the drawn whole pixel.
    in.restAt = [rest](int64_t end, int64_t span) {
        auto c = rest(end, span);
        if (end == 16000 || end == 15999) c.valid = false;
        return c;
    };
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 16001);
}
TEST(TimeframeSwitchPlan, FollowCapsAndInvalidProbes) {
    auto in = inputs(); in.following = true; in.maxTimeSpanMs = 5000.9;
    auto p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.start, 3000); EXPECT_EQ(p.end, 8000); EXPECT_FALSE(p.followPadding);
    in.maxTimeSpanMs = 0;
    in.restAt = [](int64_t, int64_t) { return chart_raster::RasterCamera{}; };
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 8000);
    in.drawnBefore.valid = false;
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 8000);
    in.nominalLiveEnd = [](int64_t) { return 0; };
    EXPECT_EQ(chart::planTimeframeSwitch(in).end, 10000);
    in.maxTimeSpanMs = .5;
    p = chart::planTimeframeSwitch(in);
    EXPECT_EQ(p.end - p.start, 1);
}
} // namespace
