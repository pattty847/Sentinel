#include "chart/core/FootprintLod.hpp"
#include <gtest/gtest.h>
#include <limits>
using chart::FootprintStage;
using chart::footprintLod;
TEST(FootprintLod, ThresholdsAndInvalidInputs) {
    EXPECT_EQ(footprintLod(23, 6), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(24, 5), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(24, 6), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(55, 12), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(56, 11), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(56, 12), FootprintStage::Numbers);
    EXPECT_EQ(footprintLod(std::numeric_limits<double>::quiet_NaN(), 12), FootprintStage::Hidden);
}
TEST(FootprintLod, TwoPixelRetentionBandsDoNotFlap) {
    auto stage = footprintLod(24, 6);
    for (double c : {25., 23., 24., 23.}) for (double r : {7., 5., 6., 5.}) {
        stage = footprintLod(c, r, stage);
        EXPECT_EQ(stage, FootprintStage::Cells);
    }
    EXPECT_EQ(footprintLod(21.9, 6, stage), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(24, 3.9, stage), FootprintStage::Hidden);
    stage = footprintLod(56, 12);
    for (double c : {57., 55., 56., 55.}) {
        stage = footprintLod(c, 11, stage);
        EXPECT_EQ(stage, FootprintStage::Numbers);
    }
    EXPECT_EQ(footprintLod(53.9, 12, stage), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(56, 9.9, stage), FootprintStage::Cells);
}
TEST(FootprintLod, SharedHistoryAndKnobs) {
    struct Raster { bool valid = true; double dpr = 2, colPxF = 112, rowPxF = 24; } raster;
    chart::FootprintPresentation look;
    look.enabled = true; look.cellsSource = true;
    EXPECT_EQ(look.stage(raster), FootprintStage::Numbers);
    EXPECT_TRUE(look.hollow(raster));
    raster.colPxF = 110;
    EXPECT_TRUE(look.hollow(raster));
    EXPECT_EQ(look.stage(raster), FootprintStage::Numbers);
    look.numbers = false;
    EXPECT_EQ(look.stage(raster), FootprintStage::Cells);
    look.hollowCandles = false;
    EXPECT_FALSE(look.hollow(raster));
    look.enabled = false;
    EXPECT_EQ(look.stage(raster), FootprintStage::Hidden);
}
TEST(FootprintLod, AbbreviatedNumbers) {
    char out[24];
    chart::footprintNumber(1200, out); EXPECT_STREQ(out, "1.2k");
    chart::footprintNumber(3400000, out); EXPECT_STREQ(out, "3.4M");
    chart::footprintNumber(12, out); EXPECT_STREQ(out, "12");
}
