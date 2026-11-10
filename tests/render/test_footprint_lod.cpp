#include "chart/core/FootprintLod.hpp"
#include <gtest/gtest.h>
#include <cstring>
#include <limits>
using chart::FootprintStage;
using chart::footprintLod;
TEST(FootprintLod, ThresholdDerivedFromThirteenPixelMagnitudeWidth) {
    const auto width = chart::kFootprintNumbersColumn;
    const auto height = chart::kFootprintNumbersRow;
    EXPECT_EQ(chart::kFootprintTextPx, 13);
    EXPECT_EQ(width, 2 * 5 * (1229.0 / 2048) * 13 + 16);
    EXPECT_EQ(footprintLod(23, 6), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(24, 6), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(width - .1, height), FootprintStage::Cells);
    auto stage = footprintLod(width, height);
    EXPECT_EQ(stage, FootprintStage::Numbers);
    for (double delta : {-1., 0., 1., -2.}) {
        stage = footprintLod(width + delta, height + delta, stage);
        EXPECT_EQ(stage, FootprintStage::Numbers);
    }
    EXPECT_EQ(footprintLod(width - 2.1, height, stage), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(width, height - 2.1, stage), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(std::numeric_limits<double>::quiet_NaN(), height), FootprintStage::Hidden);
}
TEST(FootprintLod, FormatsBaseUnitsWithoutZerosOrScientificNotation) {
    struct Case { double value; const char* text; };
    for (const auto& c : {Case{0,""}, {1e-8,"<.01"}, {.004,"<.01"}, {.42,".42"}, {2.454,"2.45"},
                         {18.44,"18.4"}, {184.4,"184"}, {1234,"1.23k"}, {34567,"34.5k"}, {3.4e6,"3.4M"},
                         {1200,"1.2k"}, {9.999,"10.0"}, {999999,"999k"}, {1e300,">999B"}, {-2.454,"-2.45"}}) {
        char out[24]; const int length = chart::flowNumber(c.value, out);
        EXPECT_STREQ(out, c.text); EXPECT_EQ(length, std::strlen(out));
        EXPECT_EQ(std::strchr(out, 'e'), nullptr);
        EXPECT_LE(length, chart::kFlowMagnitudeGlyphs + (c.value < 0));
    }
    for (double value = 1e-12; value < 1e20; value *= 1.03) {
        char out[24]; EXPECT_LE(chart::flowNumber(value, out), chart::kFlowMagnitudeGlyphs);
        EXPECT_EQ(std::strchr(out, 'e'), nullptr);
    }
    char out[24]; EXPECT_EQ(chart::flowNumber(std::numeric_limits<double>::quiet_NaN(), out), 0);
}
TEST(FootprintLod, NotionalAndStrictSingleSideHeavyTiers) {
    using chart::FlowTier; using chart::flowTier;
    EXPECT_EQ(flowTier(0, 1e6, 10, 2500), FlowTier::Empty);
    EXPECT_EQ(flowTier(.004, 1e9, 10, 2500), FlowTier::Dust);
    EXPECT_EQ(flowTier(20, 100, 30, 2500), FlowTier::Dust);
    EXPECT_EQ(flowTier(25, 100, 50, 2500), FlowTier::Normal);
    EXPECT_EQ(flowTier(55, 100, 100, 2500), FlowTier::Normal);
    EXPECT_EQ(flowTier(55.01, 100, 100, 2500), FlowTier::Heavy);
    EXPECT_DOUBLE_EQ(chart::flowTextAlpha(FlowTier::Dust), .22);
}
TEST(FootprintLod, SpineOnlyForCoveredBarAndMatchingFrame) {
    struct Raster {
        bool valid = true;
        double dpr = 1, colPxF = 100, rowPxF = 16, tick = 1;
        int64_t tfMs = 1000;
        double drawnMinPrice = 95, drawnMaxPrice = 105;
    } raster;
    chart::FootprintPresentation look;
    look.enabled = true; look.cellsSource = true;
    look.coverage.tick = 1; look.coverage.timeframeMs = 1000;
    EXPECT_FALSE(look.spine(raster));
    look.coverage.barCount = 1; look.coverage.bars[0] = {0,1000,99,103};
    EXPECT_TRUE(look.spineBar(raster, 0)); EXPECT_FALSE(look.spineBar(raster, 1000));
    raster.tick = 2; EXPECT_FALSE(look.spineBar(raster, 0)); raster.tick = 1;
    look.spineCandles = false; EXPECT_FALSE(look.spine(raster));
    look.numbers = false; EXPECT_EQ(look.stage(raster), FootprintStage::Cells);
    look.enabled = false; EXPECT_EQ(look.stage(raster), FootprintStage::Hidden);
}
