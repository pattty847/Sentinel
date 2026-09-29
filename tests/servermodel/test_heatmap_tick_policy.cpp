// Slice T: heatmap tick policy (interaction spec rules 1, 2 and 4).
// Auto hysteresis never twitches, presets respect commonTick(), Manual presets and
// per-(symbol, timeframe) memory, and the one column / one row per pixel clamps.
#include "heatmap/HeatmapResolution.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

namespace {
using namespace heatmap;
constexpr double kScale = 100; // BTC-USD: price units are cents
constexpr int64_t kDollar = 100;

// Price units per physical pixel so that one row of `units` is `rowPx` tall.
double uppFor(int64_t units, double rowPx) { return double(units) / rowPx; }

TEST(HeatmapTickPolicy, LadderIsOneTwoTwoPointFiveFiveInWholeUnits) {
    const auto &ladder = presetLadderUnits();
    const std::vector<int64_t> head{1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
    ASSERT_GE(ladder.size(), head.size());
    for (size_t i = 0; i < head.size(); ++i) EXPECT_EQ(ladder[i], head[i]) << i;
    EXPECT_TRUE(std::is_sorted(ladder.begin(), ladder.end()));
    EXPECT_TRUE(isPresetUnits(250));   // $2.50
    EXPECT_FALSE(isPresetUnits(300));  // $3 is not a preset
    EXPECT_FALSE(isPresetUnits(750));  // $7.50 either
    EXPECT_EQ(toUnits(2.5, kScale), 250);
    EXPECT_EQ(toUnits(-1, kScale), 0);
    EXPECT_EQ(toUnits(NAN, kScale), 0);
    EXPECT_DOUBLE_EQ(fromUnits(2500, kScale), 25.0);
}

TEST(HeatmapTickPolicy, PresetsAreMultiplesOfTheCommonTick) {
    EXPECT_EQ(presetAtLeast(150, kDollar), 200);
    EXPECT_EQ(presetAtLeast(201, kDollar), 500) << "$2.50 is not a multiple of $1";
    EXPECT_EQ(presetAtLeast(1, 10 * kDollar), 1000) << "$10 history offers $10 at the finest";
    EXPECT_EQ(presetAtLeast(2001, 10 * kDollar), 5000) << "$25 is not a multiple of $10";
    EXPECT_EQ(presetAtLeast(2001, 5 * kDollar), 2500) << "$25 is a multiple of $5";
    EXPECT_EQ(presetAtLeast(1, 0), 0);
    EXPECT_EQ(presetAtLeast(1, 3), 0) << "no ladder tick is a multiple of 3 units";
    // Auto output over a wide zoom sweep is always a preset built by the common tick.
    for (const int64_t common : {kDollar, 5 * kDollar, 10 * kDollar, int64_t(1)})
        for (double upp = 0.01; upp < 1e6; upp *= 1.07) {
            const int64_t t = autoTickUnits(0, common, upp, {2, 0.25});
            ASSERT_GT(t, 0);
            EXPECT_TRUE(isPresetUnits(t));
            EXPECT_TRUE(buildsOn(t, common)) << t << " on " << common;
            EXPECT_GE(double(t) / upp, 2.0 - 1e-9) << "fresh Auto rows are at least minRowPx";
        }
}

TEST(HeatmapTickPolicy, ZeroHysteresisIsTheIdealTick) {
    for (double span = 50; span < 200'000; span *= 1.13) {
        const double heightPx = 880;
        const double ideal = idealTick(80'000, 80'000 + span, heightPx, 2, 1, kScale);
        const double upp = span * kScale / heightPx;
        for (const int64_t current : {int64_t(0), kDollar, 10 * kDollar, 500 * kDollar})
            EXPECT_EQ(autoTickUnits(current, kDollar, upp, {2, 0}), toUnits(ideal, kScale)) << span;
    }
}

TEST(HeatmapTickPolicy, HysteresisThresholdsAreExact) {
    const AutoTickParams p{2, 0.25};
    // Coarser only below 2 * 0.75 = 1.5 px, then to the plain target.
    EXPECT_EQ(autoTickUnits(kDollar, kDollar, uppFor(kDollar, 1.51), p), kDollar);
    EXPECT_EQ(autoTickUnits(kDollar, kDollar, uppFor(kDollar, 1.49), p), 2 * kDollar);
    // Finer only when the finer preset reaches 2 * 1.25 = 2.5 px.
    EXPECT_EQ(autoTickUnits(2 * kDollar, kDollar, uppFor(kDollar, 2.49), p), 2 * kDollar);
    EXPECT_EQ(autoTickUnits(2 * kDollar, kDollar, uppFor(kDollar, 2.51), p), kDollar);
    // A big zoom-in jump goes straight to the finest preset that qualifies.
    EXPECT_EQ(autoTickUnits(1000 * kDollar, kDollar, uppFor(kDollar, 3), p), kDollar);
    // h is clamped to [0, 0.9]: h = 5 behaves as 0.9, not as "never coarsen".
    EXPECT_EQ(autoTickUnits(kDollar, kDollar, uppFor(kDollar, 0.19), {2, 5}), 20 * kDollar);
}

TEST(HeatmapTickPolicy, AutoIsAFixedPointAndSkipsTicksTheViewCannotBuild) {
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> logUpp(-2, 6);
    std::uniform_int_distribution<size_t> pick(0, 30);
    for (const double h : {0.0, 0.15, 0.25, 0.4})
        for (int i = 0; i < 20'000; ++i) {
            const double upp = std::pow(10.0, logUpp(rng));
            const int64_t current = presetLadderUnits()[pick(rng)];
            const int64_t next = autoTickUnits(current, kDollar, upp, {2, h});
            ASSERT_GT(next, 0);
            EXPECT_EQ(autoTickUnits(next, kDollar, upp, {2, h}), next) << "h=" << h << " upp=" << upp;
        }
    // $1 current tick after panning into $10-grid history: replaced at once.
    const double upp = uppFor(kDollar, 40); // $1 rows 40 px
    EXPECT_EQ(autoTickUnits(kDollar, 10 * kDollar, upp, {2, 0.25}), 10 * kDollar);
    EXPECT_EQ(autoTickUnits(250, kDollar, uppFor(kDollar, 1.8), {2, 0.25}), 2 * kDollar)
        << "$2.50 on a $1 grid is not buildable: evaluate fresh";
}

// Zoom through many thresholds with small wheel steps, then jitter one step
// around every point where the tick changed. With h > 0 a jitter smaller than
// the band never flips the tick; with h = 0 it flips every time (the twitch
// the hysteresis exists to remove).
struct Sweep { int inChanges = 0, outChanges = 0, jitterFlips = 0; bool monotonic = true; };
Sweep sweep(double h, double step) {
    const AutoTickParams p{2, h};
    Sweep s;
    int64_t tick = 0;
    std::vector<double> changePoints;
    // $1 rows from 0.001 px ($2500 ticks) to 100 px, then back out past the start
    // (the hysteresis delays the last coarser steps by up to (1 + h) / (1 - h)).
    double upp = uppFor(kDollar, 0.001);
    tick = autoTickUnits(0, kDollar, upp, p);
    for (; upp > uppFor(kDollar, 100); upp /= step) { // zoom in: rows grow
        const int64_t next = autoTickUnits(tick, kDollar, upp, p);
        if (next != tick) { ++s.inChanges; s.monotonic &= next < tick; changePoints.push_back(upp); tick = next; }
    }
    for (; upp < uppFor(kDollar, 0.0003); upp *= step) { // zoom out
        const int64_t next = autoTickUnits(tick, kDollar, upp, p);
        if (next != tick) { ++s.outChanges; s.monotonic &= next > tick; tick = next; }
    }
    for (const double point : changePoints) {
        int64_t t = autoTickUnits(0, kDollar, point, p);
        for (int i = 0; i < 10; ++i)
            for (const double u : {point * step, point, point / step, point}) {
                const int64_t next = autoTickUnits(t, kDollar, u, p);
                s.jitterFlips += next != t;
                t = next;
            }
    }
    return s;
}

TEST(HeatmapTickPolicy, HysteresisNeverTwitchesUnderSmallZoomJitter) {
    const double step = 1.02; // 2 % per wheel/trackpad step
    for (const double h : {0.15, 0.25, 0.4}) {
        const Sweep s = sweep(h, step);
        EXPECT_TRUE(s.monotonic) << "h=" << h;
        EXPECT_GE(s.inChanges, 10) << "sweep crosses many thresholds";
        EXPECT_GE(s.outChanges, 10) << "h=" << h;
        EXPECT_EQ(s.jitterFlips, 0) << "h=" << h;
    }
    const Sweep none = sweep(0, step);
    EXPECT_TRUE(none.monotonic);
    EXPECT_GT(none.jitterFlips, 0) << "without hysteresis a jitter at a threshold flips the tick";
}

TEST(HeatmapTickPolicy, ManualOffersPresetsForAnyPartOfTheLoadedData) {
    // Deep history: older $10 grid, recent $5 grid. $5 and $25 build only on the
    // recent part; they are still offered (the older part draws the veil).
    const auto deep = manualPresetUnits({5 * kDollar, 10 * kDollar}, 1000 * kDollar);
    const std::vector<int64_t> want{500, 1000, 2000, 2500, 5000, 10'000, 20'000, 25'000, 50'000, 100'000};
    EXPECT_EQ(deep, want);
    // Near on $1 plus a $10 part: $1 is offered although the $10 part cannot build it.
    const auto mixed = manualPresetUnits({kDollar, 10 * kDollar}, 10 * kDollar);
    EXPECT_EQ(mixed, (std::vector<int64_t>{100, 200, 500, 1000}));
    EXPECT_FALSE(buildsOn(kDollar, 10 * kDollar)) << "$1 over $10 history is the veil case";
    EXPECT_TRUE(manualPresetUnits({}, 1000).empty());
}

TEST(HeatmapTickPolicy, ManualTickIsRememberedPerSymbolAndTimeframe) {
    ManualTickMemory memory;
    const int64_t minute = kMinuteMs, hour = kHourMs;
    EXPECT_FALSE(memory.get("BTC-USD", minute));
    EXPECT_TRUE(memory.set("BTC-USD", minute, kDollar));
    EXPECT_TRUE(memory.set("BTC-USD", hour, 25 * kDollar));
    EXPECT_TRUE(memory.set("ETH-USD", minute, 10));
    EXPECT_FALSE(memory.set("BTC-USD", 5 * minute, 3 * kDollar)) << "$3 is not a preset";
    EXPECT_FALSE(memory.set("", minute, kDollar));
    EXPECT_EQ(memory.get("BTC-USD", minute), kDollar);
    EXPECT_EQ(memory.get("BTC-USD", hour), 25 * kDollar);
    EXPECT_EQ(memory.get("ETH-USD", minute), 10);
    EXPECT_FALSE(memory.get("BTC-USD", 5 * minute));
    EXPECT_TRUE(memory.set("BTC-USD", minute, 10 * kDollar));
    EXPECT_EQ(memory.get("BTC-USD", minute), 10 * kDollar);
    EXPECT_EQ(memory.get("BTC-USD", hour), 25 * kDollar) << "other timeframes keep their tick";
    memory.erase("BTC-USD", hour);
    EXPECT_FALSE(memory.get("BTC-USD", hour));
    EXPECT_EQ(memory.entries().size(), 2u);
}

TEST(HeatmapTickPolicy, OneColumnAndOneRowPerPixelClamps) {
    EXPECT_DOUBLE_EQ(maxTimeSpanMs(1500, kMinuteMs), 1500.0 * kMinuteMs);
    EXPECT_DOUBLE_EQ(maxTimeSpanMs(1500, kHourMs), 1500.0 * kHourMs);
    EXPECT_EQ(maxTimeSpanMs(0, kMinuteMs), 0);
    EXPECT_DOUBLE_EQ(maxManualPriceSpan(880, 1), 880);
    EXPECT_DOUBLE_EQ(maxManualPriceSpan(1760, 25), 44'000);
    EXPECT_EQ(maxManualPriceSpan(880, 0), 0);
    double lo = 0, hi = 3000;
    clampSpan(lo, hi, 1000);
    EXPECT_DOUBLE_EQ(lo, 1000);
    EXPECT_DOUBLE_EQ(hi, 2000);
    clampSpan(lo, hi, 5000); // already inside
    EXPECT_DOUBLE_EQ(hi - lo, 1000);
    clampSpan(lo, hi, 0);    // no limit
    EXPECT_DOUBLE_EQ(hi - lo, 1000);
}
} // namespace
