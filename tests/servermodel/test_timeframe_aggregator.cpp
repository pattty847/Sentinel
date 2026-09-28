#include <gtest/gtest.h>
#include "servermodel/TimeframeAggregator.hpp"

#include <chrono>

namespace {
Trade tradeAt(int64_t timestampMs, double price, double size) {
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
    trade.price = price;
    trade.size = size;
    return trade;
}
}

TEST(TimeframeAggregatorRollup, UtcBoundariesAndOhlcv) {
    constexpr int64_t minute = 60'000;
    constexpr int64_t day = 86'400'000;
    // Four observed minutes straddle UTC midnight; two are in the last
    // 5m/15m/4h/day bucket and two in the next one.
    const std::vector<OHLCVBar> minutes = {
        {day - 2 * minute, 10, 14, 9, 12, 2, 3, true},
        {day - minute, 12, 15, 11, 13, 4, 5, true},
        {day, 13, 16, 8, 11, 6, 7, true},
        {day + minute, 11, 17, 10, 16, 8, 9, true},
    };
    for (const int64_t timeframe : {5 * minute, 15 * minute, 4 * 60 * minute, day}) {
        const auto rolled = TimeframeAggregator::rollupMinutes(minutes, timeframe);
        ASSERT_EQ(rolled.size(), 2u) << timeframe;
        EXPECT_EQ(rolled[0].timestamp_ms, day - timeframe);
        EXPECT_EQ(rolled[1].timestamp_ms, day);
        EXPECT_DOUBLE_EQ(rolled[0].open, 10);
        EXPECT_DOUBLE_EQ(rolled[0].high, 15);
        EXPECT_DOUBLE_EQ(rolled[0].low, 9);
        EXPECT_DOUBLE_EQ(rolled[0].close, 13);
        EXPECT_DOUBLE_EQ(rolled[0].volume, 6);
        EXPECT_EQ(rolled[0].count, 8u);
        EXPECT_DOUBLE_EQ(rolled[1].open, 13);
        EXPECT_DOUBLE_EQ(rolled[1].high, 17);
        EXPECT_DOUBLE_EQ(rolled[1].low, 8);
        EXPECT_DOUBLE_EQ(rolled[1].close, 16);
        EXPECT_DOUBLE_EQ(rolled[1].volume, 14);
        EXPECT_EQ(rolled[1].count, 16u);
    }
}

TEST(TimeframeAggregatorRollup, LiveFiveMinuteClosesFromMinuteAnchor) {
    TimeframeAggregator aggregator({1'000, 60'000, 300'000, 900'000,
                                   3'600'000, 14'400'000, 86'400'000});
    aggregator.onTrade(tradeAt(60'000, 10, 2));
    aggregator.onTrade(tradeAt(119'000, 12, 3));
    aggregator.onTrade(tradeAt(120'000, 11, 4));
    aggregator.tick(300'000);
    const auto minutes = aggregator.getHistory("BTC-USD", 60'000);
    const auto five = aggregator.getHistory("BTC-USD", 300'000);
    ASSERT_EQ(five.size(), 1u);
    const auto expected = TimeframeAggregator::rollupMinutes(minutes, 300'000);
    ASSERT_EQ(expected.size(), 1u);
    EXPECT_DOUBLE_EQ(five[0].open, expected[0].open);
    EXPECT_DOUBLE_EQ(five[0].high, expected[0].high);
    EXPECT_DOUBLE_EQ(five[0].low, expected[0].low);
    EXPECT_DOUBLE_EQ(five[0].close, expected[0].close);
    EXPECT_DOUBLE_EQ(five[0].volume, expected[0].volume);
    EXPECT_EQ(five[0].count, expected[0].count);
}
