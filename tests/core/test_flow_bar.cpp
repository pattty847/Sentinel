#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include <gtest/gtest.h>

using namespace sentinel::flow;

TEST(FlowBar, ScriptedTotalsRowsSweepAndDedup) {
    FlowBarBuilder builder(60'000, 1.0);
    EXPECT_TRUE(builder.add({"100", 1000, 100, 2, Side::Buy}));
    EXPECT_TRUE(builder.add({"101", 1000, 101, 3, Side::Buy}));
    EXPECT_FALSE(builder.add({"101", 1000, 101, 3, Side::Buy}));
    EXPECT_TRUE(builder.add({"102", 2000, 99, 4, Side::Sell}));
    EXPECT_TRUE(builder.add({"103", 3000, 100, 5, Side::Unknown}));
    const auto bars = builder.bars();
    ASSERT_EQ(bars.size(), 1);
    const auto& b = bars.front();
    EXPECT_EQ(b.startMs, 0);
    EXPECT_EQ(b.open, 100); EXPECT_EQ(b.high, 101);
    EXPECT_EQ(b.low, 99); EXPECT_EQ(b.close, 100);
    EXPECT_EQ(b.baseVolume, 14); EXPECT_EQ(b.quoteVolume, 1399);
    EXPECT_EQ(b.count, 4); EXPECT_EQ(b.buyVol, 5);
    EXPECT_EQ(b.sellVol, 4); EXPECT_EQ(b.delta, 1);
    EXPECT_EQ(b.lowRow, 99); EXPECT_EQ(b.maxRowVol, 4);
    ASSERT_EQ(b.rows.size(), 3);
    EXPECT_EQ(b.rows[0], (Row{0, 4, 0, 1, 0}));
    EXPECT_EQ(b.rows[1], (Row{1, 0, 2, 0, 1}));
    EXPECT_EQ(b.rows[2], (Row{2, 0, 3, 0, 1}));
    ASSERT_EQ(b.notable.size(), 3);
    EXPECT_EQ(b.notable[0].tradeId, "100");
    EXPECT_EQ(b.notable[0].size, 5);
    EXPECT_EQ(b.notable[0].sweepLevels, 2);
    EXPECT_EQ(b.notable[0].minPrice, 100);
    EXPECT_EQ(b.notable[0].maxPrice, 101);
}

TEST(FlowBar, ComposeEqualsDirectBuildAndCodec) {
    FlowBarBuilder minute(60'000, 1), five(300'000, 1);
    for (int i = 0; i < 5; ++i) {
        Trade t{std::to_string(i + 10), i * 60'000 + 1000, 100.0 + i, double(i + 1),
                i % 2 ? Side::Sell : Side::Buy};
        minute.add(t); five.add(t);
    }
    const auto smaller = minute.bars();
    ASSERT_EQ(smaller.size(), 5);
    const auto composed = composeFlowBars(smaller, 300'000);
    const auto direct = five.bars().front();
    EXPECT_EQ(encodeFlowBar(composed), encodeFlowBar(direct));
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(direct)), direct);
    DayFile day;
    day.product = "BTC-USD"; day.dayStartMs = 0; day.timeframeMs = 60'000;
    day.nativeTick = 1; day.sourceFingerprint.fill(0x5a); day.bars = smaller;
    EXPECT_EQ(decodeFlowDay(encodeFlowDay(day)), day);
    auto corrupt = encodeFlowDay(day);
    corrupt[corrupt.size() - 1] ^= 1;
    EXPECT_NE(decodeFlowDay(corrupt), day);
}

TEST(FlowBar, HalfOpenUtcAndRowOverflow) {
    FlowBarBuilder b(60'000, 1);
    b.add({"a", -1, 100, 1, Side::Unknown});
    b.add({"b", 0, 100, 1, Side::Unknown});
    b.add({"c", 60'000, 100, 1, Side::Unknown});
    const auto bars = b.bars();
    ASSERT_EQ(bars.size(), 3);
    EXPECT_EQ(bars[0].startMs, -60'000);
    EXPECT_EQ(bars[1].startMs, 0);
    EXPECT_EQ(bars[2].startMs, 60'000);
    EXPECT_EQ(bars[1].baseVolume, 1);
    EXPECT_EQ(bars[1].buyVol, 0);
    EXPECT_EQ(bars[1].sellVol, 0);
    EXPECT_THROW(b.add({"d", 1, 40'000, 1, Side::Buy}), std::out_of_range);
    b.add({"late", 500, 101, 1, Side::Buy});
    EXPECT_EQ(b.bars()[1].revision, 1);
}
