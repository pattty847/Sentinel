#include "chart/core/FlowFooter.hpp"
#include <gtest/gtest.h>
#include <set>
using namespace chart;
TEST(FlowFooterPolicy, SignedAndUnsignedUseTheSharedMagnitudeFormat) {
    char value[24];
    footerNumber(1250, value, true); EXPECT_STREQ(value, "+1.25k");
    footerNumber(-1250, value, true); EXPECT_STREQ(value, "-1.25k");
    footerNumber(1250, value, false); EXPECT_STREQ(value, "1.25k");
    footerNumber(0, value, true); EXPECT_STREQ(value, "0");
    footerNumber(0, value, false); EXPECT_STREQ(value, "0");
    footerNumber(std::numeric_limits<double>::infinity(), value, true); EXPECT_STREQ(value, "");
}
TEST(FlowFooterPolicy, ThinningIsNonOverlappingAndAnchoredAcrossOneBarPan) {
    for (double barWidth : {1., 7., 30., 80.}) {
        const auto k = FlowFooterLayout::stride(barWidth, 48);
        EXPECT_GE(k * barWidth, 52);
        std::set<int64_t> first, panned;
        for (int64_t i = -20; i <= 40; ++i) {
            if (FlowFooterLayout::selected(i * 1000, 1000, k)) {
                if (i < 40) first.insert(i);
                if (i > -20) panned.insert(i);
            }
        }
        first.erase(-20); panned.erase(40);
        EXPECT_EQ(first, panned);
    }
    EXPECT_FALSE(FlowFooterLayout::selected(0, 0, 1));
}
TEST(FlowFooterPolicy, ThreeRowsStayAboveStripAndTooSmallSurfacesSuppressFooter) {
    const auto layout = FlowFooterLayout::place(10, 310, 96);
    ASSERT_TRUE(layout.fits);
    EXPECT_EQ(layout.bottom, 214);
    EXPECT_EQ(layout.bottom - layout.top, 55);
    for (int row = 0; row < 3; ++row) {
        EXPECT_GE(layout.rowTop(row), layout.top);
        EXPECT_LE(layout.rowTop(row) + FlowFooterLayout::textPx, layout.bottom);
    }
    EXPECT_FALSE(FlowFooterLayout::place(10, 110, 96).fits);
}

TEST(FlowFooterPolicy, SubcentDeltasAreUnsignedZeroWithoutAPill) {
    for (double delta : {-.009, 0., .009}) {
        char value[24]; footerNumber(delta, value, true);
        EXPECT_STREQ(value, "0"); EXPECT_FALSE(footerPill(delta));
    }
    EXPECT_TRUE(footerPill(.01)); EXPECT_TRUE(footerPill(-.01));
}
TEST(FlowFooterPolicy, WidthBoundCoversFormatterTransitionsIndependentlyOfValues) {
    const auto width = footerLabelWidth([](char) { return 8.; });
    EXPECT_EQ(width, 48);
    for (double value : {0., .001, .01, 9.99, 99.9, 999., 1000., 99999., 1e12, -1e12}) {
        for (bool signedValue : {false, true}) {
            char text[24]; const int count = footerNumber(value, text, signedValue);
            EXPECT_LE(count * 8., width);
        }
    }
}
