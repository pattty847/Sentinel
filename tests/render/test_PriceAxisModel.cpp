#include "models/PriceAxisModel.hpp"
#include "models/PriceUnits.hpp"
#include "render/GridViewState.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

std::vector<AxisModel::TickSnapshot> axisTicks(double priceMin, double priceRange,
                                               double cellTick, const QString& symbol = {}) {
    GridViewState view;
    PriceAxisModel axis;
    axis.setProperty("symbol", symbol);
    axis.setViewportSize(1200.0, 900.0);
    axis.setGridViewState(&view);
    axis.setProperty("tickSize", cellTick);
    view.setViewport(1, 2, priceMin, priceMin + priceRange);
    std::vector<AxisModel::TickSnapshot> ticks;
    axis.copyTicks(ticks);
    return ticks;
}

void expectGrid(const std::vector<AxisModel::TickSnapshot>& ticks, double step) {
    ASSERT_GE(ticks.size(), 2u);
    for (size_t i = 0; i < ticks.size(); ++i) {
        EXPECT_NEAR(ticks[i].value / step, std::round(ticks[i].value / step), 1e-8);
        if (i > 0) {
            EXPECT_NEAR(ticks[i].value - ticks[i - 1].value, step, 1e-8);
        }
    }
}

} // namespace

TEST(PriceAxisModel, UsesSharedLadderAcrossViewportRanges) {
    for (const auto [range, step] : {std::pair{100.0, 10.0},
                                     {2000.0, 200.0},
                                     {20000.0, 2000.0},
                                     {200000.0, 20000.0}}) {
        SCOPED_TRACE(range);
        expectGrid(axisTicks(100003.0, range, 2.0), step);
    }
}

TEST(PriceAxisModel, HonorsLadderCellTickWithoutOddSteps) {
    expectGrid(axisTicks(101.0, 60.0, 2.0), 10.0); // A $5 ladder step cannot align to $2 cells.
    expectGrid(axisTicks(101.0, 60.0, 5.0), 5.0);
    expectGrid(axisTicks(101.0, 60.0, 2.5), 5.0);
    expectGrid(axisTicks(101.0, 2000.0, 32.0), 200.0); // Not $160.
}

TEST(PriceAxisModel, IgnoresNonLadderCellAlignment) {
    expectGrid(axisTicks(101.0, 60.0, 3.0), 5.0);  // Not $6.
    expectGrid(axisTicks(101.0, 100.0, 4.0), 10.0); // Not $12.
    expectGrid(axisTicks(101.0, 0.6, 0.03), 0.05);
}

TEST(PriceAxisModel, StartsOnStepGridAfterPan) {
    const auto ticks = axisTicks(103.0, 60.0, 2.0);
    expectGrid(ticks, 10.0);
    EXPECT_DOUBLE_EQ(ticks.front().value, 110.0);
}

// PEPE trades near 0.000005 on a 1e-8 tick. The axis stepped by $1 there (the tick
// counted as 0 price units), so no label fell inside the view: an empty axis.
TEST(PriceAxisModel, LabelsATinyTickView) {
    PriceUnits::instance().clearForTest();
    const auto ticks = axisTicks(0.000005, 0.00000006, 1e-8);
    ASSERT_GE(ticks.size(), 3u);
    for (size_t i = 0; i < ticks.size(); ++i) {
        EXPECT_GE(ticks[i].value, 0.000005 - 1e-8);
        EXPECT_LE(ticks[i].value, 0.00000506 + 1e-8);
        EXPECT_NEAR(ticks[i].value / 1e-8, std::round(ticks[i].value / 1e-8), 1e-6);
        if (i > 0) EXPECT_NEAR(ticks[i].value - ticks[i - 1].value, 1e-8, 1e-12);
    }
}

TEST(PriceAxisModel, ScaledUnitLabelsAndBtcUnchanged) {
    auto& units = PriceUnits::instance();
    units.clearForTest();
    units.observe("PEPE-USD", 0.00000504);
    units.observe("BTC-USD", 101234.5);
    const auto pepe = axisTicks(0.000005, 0.00000006, 1e-8, "PEPE-USD");
    ASSERT_GE(pepe.size(), 3u);
    const std::vector<QString> expected{"5.00", "5.01", "5.02", "5.03", "5.04", "5.05", "5.06"};
    for (const auto& tick : pepe) {
        const auto index = size_t(std::llround((tick.value - 0.000005) / 1e-8));
        ASSERT_LT(index, expected.size());
        EXPECT_EQ(tick.label, expected[index]);
    }
    // BTC keeps its dollar labels; the same view without a symbol labels alike.
    const auto btc = axisTicks(100003.0, 2000.0, 2.0, "BTC-USD");
    const auto plain = axisTicks(100003.0, 2000.0, 2.0);
    ASSERT_EQ(btc.size(), plain.size());
    for (size_t i = 0; i < btc.size(); ++i) EXPECT_EQ(btc[i].label, plain[i].label);
    EXPECT_EQ(btc.front().label, QStringLiteral("$100200"));
    units.clearForTest();
}
