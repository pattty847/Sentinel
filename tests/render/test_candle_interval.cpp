#include "chart/core/CandleInterval.hpp"
#include <gtest/gtest.h>

TEST(CandleInterval, ConvertsOnlyPositiveWholeSeconds) {
    EXPECT_EQ(chart::candleTimeframeSeconds(1000), 1);
    EXPECT_EQ(chart::candleTimeframeSeconds(60000), 60);
    for (int64_t ms : {100, 1500, 999, 0, -1000})
        EXPECT_FALSE(chart::candleTimeframeSeconds(ms)) << ms;
}

TEST(CandleInterval, DecodesWireWithoutOverflow) {
    EXPECT_EQ(chart::candleTimeframeMilliseconds(1), 1000);
    EXPECT_EQ(chart::candleTimeframeMilliseconds(60), 60000);
    EXPECT_FALSE(chart::candleTimeframeMilliseconds(0));
    EXPECT_FALSE(chart::candleTimeframeMilliseconds(-1));
    EXPECT_FALSE(chart::candleTimeframeMilliseconds(std::numeric_limits<int64_t>::max()));
}
