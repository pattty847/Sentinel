// The chart text rule (ChartTextRule.hpp) and the one chart number format
// (ChartNumberFormat.hpp). Qt-free; the layers' use of them is tested in
// test_heatmap_label_layout.cpp (heatmap labels), test_ChartLayers.cpp (footprint
// numbers) and the TPO renderer tests.
#include "chart/core/ChartNumberFormat.hpp"
#include "chart/core/ChartTextRule.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <vector>
#include <limits>
#include <ostream>
#include <set>
#include <string>

namespace {
using namespace chart;

std::string text(double value, NumberStyle style = {}) {
    char out[24];
    const int length = formatChartNumber(value, out, style);
    EXPECT_EQ(length, int(std::strlen(out)));
    return out;
}

TEST(ChartNumberFormat, TableOfForms) {
    struct Case { double value; const char* full; const char* compact; };
    const Case cases[] = {
        {0, "0", "0"},
        {0.0004, "<0.001", "<0.001"},
        {0.0005, "0.001", "0.001"},
        {0.000999, "0.001", "0.001"},
        {0.0642, "0.064", "0.064"},
        {0.0995, "0.1", "0.1"},
        {0.358, "0.358", "0.36"},
        {0.5, "0.5", "0.5"},
        {0.995, "0.995", "1"},
        {1.005, "1.01", "1"},
        {3.6, "3.6", "3.6"},
        {9.996, "10", "10"},
        {12.5, "12.5", "13"},
        {99.96, "100", "100"},
        {999, "999", "999"},
        {999.6, "1k", "1k"},
        {1'236, "1.24k", "1.2k"},
        {11'000, "11k", "11k"},
        {12'345, "12.3k", "12k"},
        {123'456, "123k", "123k"},
        {847'000, "847k", "847k"},
        {999'900, "1M", "1M"},
        {1'000'000, "1M", "1M"},
        {1'200'000, "1.2M", "1.2M"},
        {1'240'000, "1.24M", "1.2M"},
        {2.1e9, "2.1B", "2.1B"},
        {2.5e12, "2.5T", "2.5T"},
        {999.4e12, "999T", "999T"},
        {999.6e12, ">999T", ">999T"},
        {1e20, ">999T", ">999T"},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(text(c.value), c.full) << c.value;
        EXPECT_EQ(text(c.value, {NumberForm::Compact}), c.compact) << c.value;
        if (c.value != 0) EXPECT_EQ(text(-c.value), std::string("-") + c.full) << c.value;
    }
    EXPECT_EQ(text(std::numeric_limits<double>::quiet_NaN()), "");
    EXPECT_EQ(text(std::numeric_limits<double>::infinity()), "");
    EXPECT_EQ(text(-0.0), "0");
}

TEST(ChartNumberFormat, SignAndCurrency) {
    EXPECT_EQ(text(1250, {NumberForm::Full, true}), "+1.25k");
    EXPECT_EQ(text(-1250, {NumberForm::Full, true}), "-1.25k");
    EXPECT_EQ(text(0, {NumberForm::Full, true}), "0");
    EXPECT_EQ(text(0.004, {NumberForm::Full, true}), "+0.004");
    EXPECT_EQ(text(1'070'000, {NumberForm::Full, false, true}), "$1.07M");
    EXPECT_EQ(text(-61.26, {NumberForm::Full, false, true}), "-$61.3");
    EXPECT_EQ(text(0.0001, {NumberForm::Full, false, true}), "<$0.001");
    EXPECT_EQ(text(5e15, {NumberForm::Full, false, true}), ">$999T");
    EXPECT_EQ(text(0, {NumberForm::Full, false, true}), "$0");
}

// Principle 8: never an exponent, never a nonzero value shown as zero, and every magnitude
// of at least 0.001 within kFlowMagnitudeGlyphs characters, over the whole double range.
TEST(ChartNumberFormat, NoExponentNoNonzeroZeroBoundedWidth) {
    for (double value = 1e-12; value < 1e20; value *= 1.0137) {
        for (auto form : {NumberForm::Full, NumberForm::Compact}) {
            const auto s = text(value, {form});
            ASSERT_EQ(s.find('e'), std::string::npos) << value;
            ASSERT_NE(s, "0") << value;
            ASSERT_NE(s.find_first_of("123456789"), std::string::npos) << value;
            if (value >= 0.001) ASSERT_LE(int(s.size()), kFlowMagnitudeGlyphs) << value << " " << s;
            if (form == NumberForm::Compact)
                ASSERT_LE(s.size(), text(value).size()) << "Compact is never wider than Full: " << value;
        }
    }
}

// Principle 8 at the ends of the double range.
TEST(ChartNumberFormat, ExtremesOfTheDoubleRange) {
    EXPECT_EQ(text(5e-324), "<0.001");
    EXPECT_EQ(text(-5e-324), "-<0.001");
    EXPECT_EQ(text(DBL_MAX), ">999T");
    EXPECT_EQ(text(-DBL_MAX, {NumberForm::Compact}), "->999T");
    EXPECT_EQ(text(DBL_MIN, {NumberForm::Full, false, true}), "<$0.001");
}

// Same value and same form give the same text everywhere; mixed magnitudes print each value
// per its own form (no shared unit or decimals).
TEST(ChartNumberFormat, MixedMagnitudesPrintPerValue) {
    EXPECT_EQ(text(12'345), "12.3k");
    EXPECT_EQ(text(1.234), "1.23");
    EXPECT_EQ(text(0.05), "0.05");
    EXPECT_EQ(text(12'345, {NumberForm::Compact}), "12k");
}

TEST(ChartTextRule, SettingsClampToWholePixelsInRange) {
    EXPECT_EQ(ChartTextRule{}, ChartTextRule::clamped(13, 18, 2));
    const auto low = ChartTextRule::clamped(2, 3, -4);
    EXPECT_EQ(low.minPx, 10); EXPECT_EQ(low.maxPx, 10); EXPECT_EQ(low.padPx, 0);
    const auto high = ChartTextRule::clamped(99, 99, 99);
    EXPECT_EQ(high.minPx, 24); EXPECT_EQ(high.maxPx, 32); EXPECT_EQ(high.padPx, 8);
    const auto inverted = ChartTextRule::clamped(20, 14, 2.4);
    EXPECT_EQ(inverted.minPx, 20); EXPECT_EQ(inverted.maxPx, 20) << "the largest size is never below the smallest";
    EXPECT_EQ(inverted.padPx, 2);
    const auto fractional = ChartTextRule::clamped(13.6, 17.4, 1.5);
    EXPECT_EQ(fractional.minPx, 14); EXPECT_EQ(fractional.maxPx, 17); EXPECT_EQ(fractional.padPx, 2);
    const auto nan = ChartTextRule::clamped(std::nan(""), std::nan(""), std::nan(""));
    EXPECT_EQ(nan, ChartTextRule{});
}

// Text grows with its cell from 13 to 18 px in whole logical pixels: a continuous zoom
// steps the size, so glyphs never resample between frames at 1x.
TEST(ChartTextRule, SizeGrowsWithTheCellInWholePixels) {
    const ChartTextRule rule;
    EXPECT_EQ(rule.fitSize(500, 16.9, 0.6), 0) << "13 px plus 2 px above and below needs 17 px";
    EXPECT_EQ(rule.fitSize(500, 17, 0.6), 13);
    EXPECT_EQ(rule.fitSize(500, 100, 0.6), 18) << "capped at the largest size";
    double previous = 0;
    for (double height = 17; height < 30; height += 0.0625) {
        const double size = rule.fitSize(500, height, 0.6);
        ASSERT_EQ(size, std::floor(size)) << height;
        ASSERT_GE(size, previous);
        ASSERT_LE(size + 2 * rule.padPx, height + 1e-9);
        previous = size;
    }
    // Width: ink plus 2 px on each side.
    EXPECT_EQ(rule.fitSize(4 + 13 * 3, 100, 3), 13);
    EXPECT_EQ(rule.fitSize(4 + 13 * 3 - 0.01, 100, 3), 0);
}

// The view and demand of a test layer: monospace advance 0.6 em.
constexpr double kAdvance = 0.6;
ChartTextView view(double w, double h, ChartTextRule rule = {}, double dpr = 1) { return {w, h, dpr, rule}; }
ChartTextDemand demand(std::initializer_list<int> glyphs) {
    ChartTextDemand d;
    d.forms = int(glyphs.size());
    int f = 0;
    for (int g : glyphs) d.glyphs[size_t(f++)] = g;
    return d;
}
// A label of `glyphs` glyphs at `px` with the rule's padding needs this cell width.
double needs(int glyphs, double px = 13) { return 4 + px * glyphs * kAdvance; }

// All or none: the longest label of the frame decides for every label; the compact form is
// tried before the layer hides its text.
TEST(ChartTextRule, CompactBeforeHideThenNothing) {
    const auto d = demand({5, 4}); // e.g. "1.24M" and "1.2M"
    auto fit = decideText(view(needs(5) + ChartTextRule::kHysteresisPx, 20), kAdvance, d, {});
    EXPECT_TRUE(fit.on); EXPECT_EQ(fit.form, 0); EXPECT_EQ(fit.sizePx, 13);
    fit = decideText(view(needs(4) + ChartTextRule::kHysteresisPx, 20), kAdvance, d, {});
    EXPECT_TRUE(fit.on); EXPECT_EQ(fit.form, 1); EXPECT_EQ(fit.sizePx, 13);
    EXPECT_FALSE(decideText(view(needs(4) - 0.5, 20), kAdvance, d, {}).on);
    EXPECT_FALSE(decideText(view(1000, 16), kAdvance, d, {}).on) << "too short, whatever the width";
}

// The glyph budget: a form whose wanted labels need more glyphs than the layer may draw is
// not used; with no form left the layer draws none, never a subset.
TEST(ChartTextRule, AFormOverTheGlyphBudgetIsNotUsed) {
    auto d = demand({5, 4});
    d.withinBudget = 0b10;
    auto fit = decideText(view(500, 40), kAdvance, d, {});
    EXPECT_TRUE(fit.on); EXPECT_EQ(fit.form, 1);
    d.withinBudget = 0;
    EXPECT_FALSE(decideText(view(500, 40), kAdvance, d, {}).on);
}

// A decision made under another rule or dpr is no decision: {on, Full, 13 px} under a
// new 18..18 rule at 58 x 22 gives 18 px, never 13 (and TPO never clamps an inverted range).
TEST(ChartTextRule, AnOldDecisionDoesNotHoldUnderANewRule) {
    const auto d = demand({5});
    const ChartTextFit at13 = decideText(view(58, 18), kAdvance, d, {});
    ASSERT_TRUE(at13.on); ASSERT_EQ(at13.form, 0); ASSERT_EQ(at13.sizePx, 13);
    const auto rule = ChartTextRule::clamped(18, 18, 2);
    for (const double height : {22.0, 23.0}) {
        const auto fit = decideText(view(58, height, rule), kAdvance, d, at13);
        EXPECT_TRUE(!fit.on || fit.sizePx == 18) << height << ": " << fit.sizePx;
    }
    EXPECT_EQ(decideText(view(61, 23, rule), kAdvance, d, at13).sizePx, 18);
    // Another dpr also decides afresh: off with no spare width at dpr 1 holds, at dpr 2 it is new.
    const auto two = demand({2});
    const auto on = decideText(view(needs(2), 20), kAdvance, two, decideText(view(needs(2) + 3, 20), kAdvance, two, {}));
    ASSERT_TRUE(on.on);
    EXPECT_FALSE(decideText(view(needs(2), 20, {}, 2), kAdvance, two, on).on);
    // A hand-made decision outside the rule's sizes is no decision either.
    ChartTextFit stale = on;
    stale.sizePx = 40;
    EXPECT_EQ(decideText(view(needs(2), 20), kAdvance, two, stale).on, false);
}

// Hysteresis: no flip inside the band while zooming; it turns on, grows and returns to the
// full form only with spare width.
TEST(ChartTextRule, NoFlipInsideTheHysteresisBand) {
    const auto d = demand({5});
    const double exact = needs(5);
    ChartTextFit fit;
    for (double w = exact; w < exact + ChartTextRule::kHysteresisPx; w += 0.125) {
        fit = decideText(view(w, 20), kAdvance, d, fit);
        ASSERT_FALSE(fit.on) << w;
    }
    fit = decideText(view(exact + ChartTextRule::kHysteresisPx, 20), kAdvance, d, fit);
    ASSERT_TRUE(fit.on);
    for (double w = exact + ChartTextRule::kHysteresisPx; w >= exact; w -= 0.125) {
        fit = decideText(view(w, 20), kAdvance, d, fit);
        ASSERT_TRUE(fit.on) << w;
    }
    EXPECT_FALSE(decideText(view(exact - 0.01, 20), kAdvance, d, fit).on);
    // Form: back from Compact to Full only with the spare width.
    const auto forms = demand({5, 3});
    const auto compact = decideText(view(needs(3) + 3, 20), kAdvance, forms, {});
    ASSERT_EQ(compact.form, 1);
    EXPECT_EQ(decideText(view(exact + 1, 20), kAdvance, forms, compact).form, 1);
    EXPECT_EQ(decideText(view(exact + ChartTextRule::kHysteresisPx, 20), kAdvance, forms, compact).form, 0);
    // Size: one pixel larger only with the spare width; smaller at once.
    const auto at13 = decideText(view(needs(5, 13) + 3, 40), kAdvance, d, {});
    ASSERT_EQ(at13.sizePx, 13);
    EXPECT_EQ(decideText(view(needs(5, 14) + 1, 40), kAdvance, d, at13).sizePx, 13);
    EXPECT_EQ(decideText(view(needs(5, 14) + ChartTextRule::kHysteresisPx, 40), kAdvance, d, at13).sizePx, 14);
    const auto at16 = decideText(view(needs(5, 16) + 3, 40), kAdvance, d, {});
    ASSERT_EQ(at16.sizePx, 16);
    EXPECT_EQ(decideText(view(needs(5, 14) + 1, 40), kAdvance, d, at16).sizePx, 14);
}

// The accepted limitation (owner 2026-11-18, ChartTextRule.hpp): no memory of past label
// widths, so at an unchanged view whose cells sit at the size limit a live value that changes
// length switches the text: "1k" clears the turn-on margin, "999" fails even Compact.
// Do not "fix" this without the owner.
TEST(ChartTextRule, ALengthChangeAtTheLimitTogglesText) {
    char a[24], b[24];
    const int longer = formatChartNumber(999.49, a), shorter = formatChartNumber(999.51, b);
    ASSERT_EQ(std::string(a), "999"); ASSERT_EQ(std::string(b), "1k");
    ASSERT_EQ(formatChartNumber(999.49, a, {NumberForm::Compact}), longer) << "compact is no shorter";
    const auto v = view(needs(shorter) + ChartTextRule::kHysteresisPx + 0.5, 20);
    ASSERT_LT(v.cellWidth, needs(longer));
    ChartTextFit fit;
    for (int i = 0; i < 6; ++i) {
        const int glyphs = i % 2 ? longer : shorter;
        fit = decideText(v, kAdvance, demand({glyphs, glyphs}), fit);
        EXPECT_EQ(fit.on, glyphs == shorter) << i;
    }
}

// The decision is a pure function of the frame and the validated previous decision: the same
// inputs give the same output, and two histories ending in the same decision agree.
TEST(ChartTextRule, TheDecisionIsAPureFunction) {
    const auto d = demand({5, 4});
    const auto frame = view(needs(5) + 1, 20);
    ChartTextFit a, b;
    for (double w : {60.0, 50.0, needs(5) + 3}) a = decideText(view(w, 20), kAdvance, d, a);
    for (double w : {needs(5) + 10, needs(5) + 3}) b = decideText(view(w, 20), kAdvance, demand({2, 2}), b);
    b = decideText(view(needs(5) + 3, 20), kAdvance, d, b);
    ASSERT_EQ(a, b);
    EXPECT_EQ(decideText(frame, kAdvance, d, a), decideText(frame, kAdvance, d, b));
    EXPECT_EQ(decideText(frame, kAdvance, d, a), decideText(frame, kAdvance, d, a));
}

// Height band: turning text on needs one spare pixel of height, so 16.99/17.01 px rows do
// not toggle every frame; a drawn text stays while it fits exactly.
TEST(ChartTextRule, AHeightAtTheEdgeNeverToggles) {
    const auto d = demand({3});
    ChartTextFit fit;
    std::set<bool> seen;
    for (int i = 0; i < 50; ++i) {
        fit = decideText(view(200, i % 2 ? 17.01 : 16.99), kAdvance, d, fit);
        seen.insert(fit.on);
    }
    EXPECT_EQ(seen, std::set<bool>{false});
    fit = decideText(view(200, 18), kAdvance, d, fit);
    ASSERT_TRUE(fit.on);
    EXPECT_EQ(fit.sizePx, 13);
    EXPECT_TRUE(decideText(view(200, 17), kAdvance, d, fit).on) << "drawn text stays at an exact fit";
    // Growing one pixel also needs the spare height.
    EXPECT_EQ(decideText(view(200, 18.5), kAdvance, d, fit).sizePx, 13);
    EXPECT_EQ(decideText(view(200, 19), kAdvance, d, fit).sizePx, 14);
}

// TPO letters: the TPO style's letter width stays TPO's own largest size, floored to whole
// pixels, never below the rule's smallest.
TEST(ChartTextRule, TpoLetterSize) {
    const ChartTextRule rule;
    ChartTextFit fit;
    fit.on = true;
    fit.sizePx = 16;
    EXPECT_EQ(tpoLetterPx(fit, rule, 13), 13);
    EXPECT_EQ(tpoLetterPx(fit, rule, 14.7), 14) << "whole pixels";
    EXPECT_EQ(tpoLetterPx(fit, rule, 30), 16) << "the cell's fit caps it";
    EXPECT_EQ(tpoLetterPx(fit, rule, 9), 13) << "never below the rule's smallest";
    EXPECT_EQ(tpoLetterPx(fit, ChartTextRule::clamped(18, 18, 2), 13), 18) << "never an inverted clamp";
    fit.on = false;
    EXPECT_EQ(tpoLetterPx(fit, rule, 13), 0);
}
} // namespace
