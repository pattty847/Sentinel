#include "chart/core/CandleInterval.hpp"
#include <gtest/gtest.h>

TEST(CandleInterval, MemoryHistoryIsOnlyHundredMillisecondsAndOneSecond) {
    for (int64_t tf : {100, 1000}) EXPECT_TRUE(chart::candleHistoryFromMemory(tf));
    for (int64_t tf : {-1, 0, 250, 1500, 60000}) EXPECT_FALSE(chart::candleHistoryFromMemory(tf));
}
TEST(CandleInterval, QuietBucketsStartAtOneSecond) {
    for (int64_t tf : {-1, 0, 100, 999}) {
        EXPECT_FALSE(chart::drawsTradeFreeBuckets(tf));
        EXPECT_FALSE(chart::synthesizesQuietBars(tf));
    }
    for (int64_t tf : {1000, 60000, 86400000}) {
        EXPECT_TRUE(chart::drawsTradeFreeBuckets(tf));
        EXPECT_TRUE(chart::synthesizesQuietBars(tf));
    }
}
TEST(CandleInterval, TradeFreeBarsAreDrawnOnlyFromOneSecond) {
    EXPECT_TRUE(chart::drawsCandle(100, 0.5));
    EXPECT_FALSE(chart::drawsCandle(100, 0));
    EXPECT_FALSE(chart::drawsCandle(100, -1));
    for (int64_t tf : {1000, 60000}) {
        EXPECT_TRUE(chart::drawsCandle(tf, 0));
        EXPECT_TRUE(chart::drawsCandle(tf, 2));
    }
}
