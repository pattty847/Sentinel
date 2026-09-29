#include "models/PriceAxisModel.hpp"
#include "render/GridViewState.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

std::vector<AxisModel::TickSnapshot> axisTicks(double priceMin, double priceRange,
                                               double cellTick) {
    GridViewState view;
    PriceAxisModel axis;
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
