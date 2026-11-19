#include "chart/core/FlowInk.hpp"
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
    // Two five-glyph labels with the chart text rule's 2 px on each side, the 10 px candle pill,
    // and the 2 px stage hysteresis.
    EXPECT_EQ(width, 2 * (5 * (1229.0 / 2048) * 13 + 4) + 12);
    EXPECT_EQ(height, 17) << "13 px with 2 px above and below";
    EXPECT_EQ(footprintLod(23, 6), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(24, 6), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(width - .1, height), FootprintStage::Cells);
    auto stage = footprintLod(width, height);
    EXPECT_EQ(stage, FootprintStage::Numbers);
    for (double delta : {-1., 0., 1., -2.}) {
        stage = footprintLod(width + delta, height + delta, stage);
        EXPECT_EQ(stage, FootprintStage::Numbers);
    }
    EXPECT_EQ(footprintLod(width - 2.1, height, stage), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(width, 1, stage), FootprintStage::Numbers) << "merged bands reach the numbers height";
    EXPECT_EQ(footprintLod(std::numeric_limits<double>::quiet_NaN(), height), FootprintStage::Hidden);
}
TEST(FootprintLod, StripKeepsFootprintAtEveryDrawableColumn) {
    EXPECT_EQ(chart::kFootprintCellsColumn, 24);
    EXPECT_EQ(footprintLod(1.99, 6), FootprintStage::Strip) << "no zoom floor: thin columns stay Strip";
    EXPECT_EQ(footprintLod(.5, 6), FootprintStage::Strip) << "one device px at dpr 2";
    EXPECT_EQ(footprintLod(1e-6, 6), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(2, 6), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(2, .01), FootprintStage::Strip) << "thin rows merge; they never hide";
    EXPECT_EQ(footprintLod(23.99, 1), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(24, .5), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(chart::kFootprintNumbersColumn, .5), FootprintStage::Numbers);
    EXPECT_EQ(footprintLod(10, 0), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(0, 6), FootprintStage::Hidden);
    EXPECT_EQ(footprintLod(-1, 6), FootprintStage::Hidden);
    for (auto previous : {FootprintStage::Hidden, FootprintStage::Strip, FootprintStage::Cells, FootprintStage::Numbers})
        for (double row : {.01, 1., 6., 100.})
            for (double column = 1.0 / 64; column < 400; column *= 1.01)
                ASSERT_NE(footprintLod(column, row, previous), FootprintStage::Hidden) << column << ' ' << row;
}
TEST(FootprintLod, HysteresisAtEveryBoundary) {
    const auto numbers = chart::kFootprintNumbersColumn;
    // Strip has no lower boundary, from any previous stage.
    EXPECT_EQ(footprintLod(.3, 6, FootprintStage::Hidden), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(.3, 6, FootprintStage::Cells), FootprintStage::Strip);
    // Cells <-> Strip: enters at 24, held to 22.
    EXPECT_EQ(footprintLod(22.1, 6, FootprintStage::Cells), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(22.1, 6, FootprintStage::Numbers), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(21.9, 6, FootprintStage::Cells), FootprintStage::Strip);
    EXPECT_EQ(footprintLod(23.9, 6, FootprintStage::Strip), FootprintStage::Strip);
    // Numbers <-> Cells: enters at kFootprintNumbersColumn, held 2 px below.
    EXPECT_EQ(footprintLod(numbers - 1.9, 6, FootprintStage::Numbers), FootprintStage::Numbers);
    EXPECT_EQ(footprintLod(numbers - 2.1, 6, FootprintStage::Numbers), FootprintStage::Cells);
    EXPECT_EQ(footprintLod(numbers - .1, 6, FootprintStage::Cells), FootprintStage::Cells);
}
TEST(FootprintLod, BandRowsAreTheSmallestFactorReachingTheStageMinimum) {
    using chart::footprintBandRows;
    EXPECT_EQ(footprintBandRows(6, FootprintStage::Strip), 1);
    EXPECT_EQ(footprintBandRows(5.9, FootprintStage::Strip), 2);
    EXPECT_EQ(footprintBandRows(2, FootprintStage::Cells), 3);
    EXPECT_EQ(footprintBandRows(.5, FootprintStage::Cells), 12);
    EXPECT_EQ(footprintBandRows(chart::kFootprintNumbersRow, FootprintStage::Numbers), 1);
    EXPECT_EQ(footprintBandRows(chart::kFootprintNumbersRow - .1, FootprintStage::Numbers), 2);
    EXPECT_EQ(footprintBandRows(1, FootprintStage::Hidden), 1);
    EXPECT_EQ(footprintBandRows(std::numeric_limits<double>::quiet_NaN(), FootprintStage::Strip), 1);
    for (auto stage : {FootprintStage::Strip, FootprintStage::Cells, FootprintStage::Numbers}) {
        const double minimum = stage == FootprintStage::Numbers ? chart::kFootprintNumbersRow : chart::kFootprintBandMinRow;
        for (double row = .05; row < 40; row *= 1.07) {
            const auto k = footprintBandRows(row, stage);
            EXPECT_GE(k * row, minimum - 1e-9) << row;
            EXPECT_LT((k - 1) * row, minimum) << row;
        }
    }
}
TEST(FootprintLod, BandRowsHoldAcrossZoomUntilTheSmallerBandClearsTheMargin) {
    using chart::footprintBandRows;
    const auto cells = FootprintStage::Cells;
    EXPECT_EQ(footprintBandRows(5.9, cells, 1), 2) << "growing to reach the minimum is immediate";
    EXPECT_EQ(footprintBandRows(6, cells, 2), 2) << "k = 1 reaches 6 px but not 6 + 1";
    EXPECT_EQ(footprintBandRows(6.9, cells, 2), 2);
    EXPECT_EQ(footprintBandRows(7, cells, 2), 1);
    EXPECT_EQ(footprintBandRows(6, cells, 0), 1) << "no history: plain minimum";
    EXPECT_EQ(footprintBandRows(1, cells, 40), 7) << "a held k never exceeds the margin's k";
    // Zoom sweep in and out near a boundary: k changes at most once per direction.
    int64_t k = 0, changes = 0;
    for (double row : {5.8, 5.95, 6.05, 6.3, 6.05, 5.95, 6.3, 6.6, 6.4}) {
        const auto next = footprintBandRows(row, cells, k);
        changes += k && next != k; k = next;
    }
    EXPECT_EQ(changes, 0);
}
TEST(FootprintLod, BudgetBandRowsFitTheWorstCaseVisibleCells) {
    using chart::footprintBudgetBandRows;
    EXPECT_EQ(footprintBudgetBandRows(2049, 1001, 65536), 34);
    EXPECT_EQ(footprintBudgetBandRows(3, 11, 65536), 1);
    EXPECT_EQ(footprintBudgetBandRows(0, 11, 65536), 1);
    for (int64_t bars : {1, 7, 300, 2049})
        for (int64_t rows : {1, 50, 1001, 100000}) {
            const auto k = footprintBudgetBandRows(bars, rows, 65536);
            EXPECT_LE(bars * ((rows + k - 1) / k + 1), 65536) << bars << ' ' << rows;
        }
}
TEST(FootprintLod, BudgetBandRowsHoldAcrossZoomUntilTheSmallerFactorFitsTheMargin) {
    using chart::footprintBudgetBandRows;
    // 2049 bars: 30 bands per bar in 65536 cells, 28 in the 61440 the margin leaves.
    EXPECT_EQ(footprintBudgetBandRows(2049, 271, 65536, 9), 10) << "growing to fit is immediate";
    EXPECT_EQ(footprintBudgetBandRows(2049, 270, 65536, 0), 9) << "no history: plain fit";
    EXPECT_EQ(footprintBudgetBandRows(2049, 270, 65536, 10), 10) << "9 fits, but not with the margin";
    EXPECT_EQ(footprintBudgetBandRows(2049, 252, 65536, 10), 9) << "9 x 28 = 252 rows: clears the margin";
    EXPECT_EQ(footprintBudgetBandRows(2049, 252, 65536, 40), 9) << "a held k never exceeds the margin's k";
    for (int64_t bars : {1, 7, 300, 2049, 8193})
        for (int64_t rows : {1, 50, 271, 1001, 100000})
            for (int64_t previous : {0, 1, 9, 34, 481, 100000}) {
                const auto k = footprintBudgetBandRows(bars, rows, 65536, previous);
                EXPECT_LE(bars * ((rows + k - 1) / k + 1), 65536) << bars << ' ' << rows << ' ' << previous;
                EXPECT_GE(k, chart::footprintBudgetFitRows(bars, rows, 65536));
            }
}
TEST(FootprintLod, SpanCountIsConservativeAndIgnoresRoundingNoise) {
    using chart::footprintSpanCount;
    EXPECT_EQ(footprintSpanCount(2048), 2049);
    EXPECT_EQ(footprintSpanCount(2048 + 1e-12), 2049);
    EXPECT_EQ(footprintSpanCount(2048 - 1e-12), 2049);
    EXPECT_EQ(footprintSpanCount(299.5), 301);
    EXPECT_EQ(footprintSpanCount(0), 1);
    EXPECT_EQ(footprintSpanCount(std::numeric_limits<double>::infinity()), 1);
}
TEST(FootprintLod, BandsAnchorToTheAbsoluteRowGrid) {
    using chart::footprintBandStart;
    EXPECT_EQ(footprintBandStart(-1, 3), -3);
    EXPECT_EQ(footprintBandStart(5, 3), 3);
    EXPECT_EQ(footprintBandStart(6, 3), 6);
    EXPECT_EQ(footprintBandStart(7, 1), 7);
    for (int64_t k = 1; k < 8; ++k)
        for (int64_t row = -50; row < 50; ++row) {
            const auto start = footprintBandStart(row, k);
            EXPECT_EQ(start % k, 0); EXPECT_LE(start, row); EXPECT_GT(start + k, row);
            // Neighbours in one band share its start, whichever row a pan brings on screen first.
            EXPECT_EQ(footprintBandStart(start + k - 1, k), start);
        }
}
// Footprint numbers use the one chart number format (formatChartNumber); the full table
// is in test_chart_text_rule.cpp.
TEST(FootprintLod, FormatsBaseUnitsWithoutZerosOrScientificNotation) {
    struct Case { double value; const char* text; };
    for (const auto& c : {Case{.42,"0.42"}, {2.454,"2.45"}, {18.44,"18.4"}, {184.4,"184"}, {1234,"1.23k"},
                         {34567,"34.6k"}, {3.4e6,"3.4M"}, {1200,"1.2k"}, {9.999,"10"}, {999999,"1M"},
                         {1e300,">999T"}, {-2.454,"-2.45"}}) {
        char out[24]; const int length = chart::formatChartNumber(c.value, out);
        EXPECT_STREQ(out, c.text); EXPECT_EQ(length, std::strlen(out));
        EXPECT_EQ(std::strchr(out, 'e'), nullptr);
        EXPECT_LE(length, chart::kFlowMagnitudeGlyphs + (c.value < 0));
    }
    for (double value = .01; value < 1e20; value *= 1.03) {
        char out[24]; EXPECT_LE(chart::formatChartNumber(value, out), chart::kFlowMagnitudeGlyphs);
        EXPECT_EQ(std::strchr(out, 'e'), nullptr);
    }
    char out[24]; EXPECT_EQ(chart::formatChartNumber(std::numeric_limits<double>::quiet_NaN(), out), 0);
}
TEST(FootprintLod, NotionalAndStrictSingleSideHeavyTiers) {
    using chart::FlowTier; using chart::flowTier;
    EXPECT_EQ(flowTier(0, 1e6, 10, 2500), FlowTier::Empty);
    EXPECT_EQ(flowTier(.004, 1e9, 10, 2500), FlowTier::Dust);
    EXPECT_EQ(flowTier(20, 100, 30, 2500), FlowTier::Dust);
    EXPECT_EQ(flowTier(25, 100, 50, 2500), FlowTier::Normal);
    EXPECT_EQ(flowTier(55, 100, 100, 2500), FlowTier::Normal);
    EXPECT_EQ(flowTier(55.01, 100, 100, 2500), FlowTier::Heavy);
    EXPECT_DOUBLE_EQ(chart::flowTextAlpha(FlowTier::Dust), 0) << "dust cells have no number";
    EXPECT_DOUBLE_EQ(chart::flowTextAlpha(FlowTier::Normal), .6);
    EXPECT_DOUBLE_EQ(chart::flowTextAlpha(FlowTier::Heavy), 1);
}
TEST(FlowInk, FillOpacityRisesWithVolumeFromTheFloor) {
    EXPECT_DOUBLE_EQ(chart::flowFillAlpha(0, 10), chart::kFlowMinFillAlpha);
    EXPECT_DOUBLE_EQ(chart::flowFillAlpha(10, 10), 1);
    EXPECT_DOUBLE_EQ(chart::flowFillAlpha(99, 10), 1);
    EXPECT_LT(chart::flowFillAlpha(2, 10), chart::flowFillAlpha(4, 10));
    EXPECT_DOUBLE_EQ(chart::flowFillAlpha(5, 0), chart::kFlowMinFillAlpha) << "no normaliser, no volume share";
}
TEST(FlowInk, PicksTheInkWithTheHigherContrastAndLightWinsTies) {
    constexpr chart::FlowRgb green{46, 189, 133}, red{246, 70, 93}, bg{15, 17, 20}, light{240, 244, 248}, dark{11, 13, 16};
    EXPECT_FALSE(chart::flowInkIsLight(green, 1, bg, light, dark)) << "full green takes dark ink";
    EXPECT_FALSE(chart::flowInkIsLight(red, 1, bg, light, dark)) << "full red takes dark ink";
    EXPECT_TRUE(chart::flowInkIsLight(green, chart::kFlowMinFillAlpha, bg, light, dark)) << "a faint fill is near the background";
    EXPECT_TRUE(chart::flowInkIsLight(red, chart::kFlowMinFillAlpha, bg, light, dark));
    EXPECT_TRUE(chart::flowInkIsLight(green, 0, bg, light, dark)) << "no fill is the background itself";
    EXPECT_TRUE(chart::flowInkIsLight(green, 1, bg, light, light)) << "equal contrast keeps the light ink";
}
TEST(FootprintLod, SpineOnlyForCoveredBarAndMatchingFrame) {
    struct Raster {
        bool valid = true;
        double dpr = 1, colPxF = 100, rowPxF = 18, tick = 1;
        int64_t tfMs = 1000;
        double drawnMinPrice = 95, drawnMaxPrice = 105;
    } raster;
    chart::FootprintPresentation look;
    look.enabled = true;
    look.coverage.tick = 1; look.coverage.timeframeMs = 1000;
    EXPECT_FALSE(look.spine(raster));
    look.coverage.barCount = 1; look.coverage.bars[0] = {0,1000,99,103};
    EXPECT_TRUE(look.spineBar(raster, 0)); EXPECT_FALSE(look.spineBar(raster, 1000));
    raster.tick = 2; EXPECT_FALSE(look.spineBar(raster, 0)); raster.tick = 1;
    raster.colPxF = 10;
    EXPECT_EQ(look.stage(raster), FootprintStage::Strip);
    EXPECT_FALSE(look.spine(raster)) << "Strip keeps the normal full candle";
    raster.colPxF = 100;
    look.spineCandles = false; EXPECT_FALSE(look.spine(raster));
    look.numbers = false; EXPECT_EQ(look.stage(raster), FootprintStage::Cells);
    look.enabled = false; EXPECT_EQ(look.stage(raster), FootprintStage::Hidden);
}

namespace {
// Independent decimal oracle for values of at least 1: round a string to three significant
// digits by carrying through its digits, place the decimal point and suffix, drop trailing
// zeros. No floating arithmetic or production helpers.
std::string referenceFlow(std::string input) {
    const auto point = input.find('.');
    int whole = point == std::string::npos ? int(input.size()) : int(point);
    if (point != std::string::npos) input.erase(point, 1);
    const bool up = input.size() > 3 && input[3] >= '5';
    input.resize(3, '0');
    if (up) {
        int at = 2;
        while (at >= 0 && input[size_t(at)] == '9') input[size_t(at--)] = '0';
        if (at < 0) { input = "100"; ++whole; }
        else ++input[size_t(at)];
    }
    const int unit = (whole - 1) / 3;
    int decimal = whole - unit * 3;
    if (decimal < 3) input.insert(size_t(decimal), ".");
    while (input.back() == '0' && input.find('.') != std::string::npos) input.pop_back();
    if (input.back() == '.') input.pop_back();
    if (unit) input += "kMB"[unit - 1];
    return input;
}
}
TEST(ChartNumberFormat, EveryIntegerThrough999999MatchesIndependentDecimalOracle) {
    char out[24];
    for (int value = 1; value <= 999999; ++value) {
        const auto expected = referenceFlow(std::to_string(value));
        chart::formatChartNumber(value, out);
        ASSERT_EQ(out, expected) << value;
        chart::formatChartNumber(-value, out);
        ASSERT_EQ(out, "-" + expected) << -value;
    }
    for (const auto* decimal : {"2300000", "4100000000"}) {
        chart::formatChartNumber(std::stod(decimal), out);
        EXPECT_EQ(out, referenceFlow(decimal));
    }
}
TEST(ChartNumberFormat, DecimalHalfUpCarries) {
    for (const auto* decimal : {"1.005", "9.996", "99.96", "999.6", "999950",
                              "34567", "2300", "2010", "1150", "4100", "2300000", "4100000000"}) {
        char out[24]; const double value = std::stod(decimal);
        chart::formatChartNumber(value, out); EXPECT_EQ(out, referenceFlow(decimal)) << decimal;
        chart::formatChartNumber(-value, out); EXPECT_EQ(out, "-" + referenceFlow(decimal)) << decimal;
    }
}
