#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

using namespace sentinel::flow;
namespace {
constexpr int64_t Base = 100'000'000;
constexpr int64_t Quote = 100'000'000;
FlowBarBuilder builder(int64_t tf = 60'000) { return {tf, 1, 100, Base, Quote}; }
void accepted(FlowBarBuilder& b, const Trade& t) {
    ASSERT_EQ(b.add(t), FlowBarBuilder::AddResult::Accepted);
}
}

TEST(FlowBar, ScriptedTotalsRowsSweepAndDedup) {
    auto b = builder();
    accepted(b, {"100", 1000, 100, 2, Side::Buy});
    accepted(b, {"101", 1000, 101, 3, Side::Buy});
    EXPECT_EQ(b.add({"101", 1000, 101, 3, Side::Buy}), FlowBarBuilder::AddResult::Duplicate);
    accepted(b, {"102", 2000, 99, 4, Side::Sell});
    accepted(b, {"103", 3000, 100, 5, Side::Unknown});
    const auto bars = b.bars();
    ASSERT_EQ(bars.size(), 1);
    const auto& bar = bars.front();
    EXPECT_EQ(bar.startMs, 0);
    EXPECT_EQ(bar.open, 100); EXPECT_EQ(bar.high, 101);
    EXPECT_EQ(bar.low, 99); EXPECT_EQ(bar.close, 100);
    EXPECT_EQ(bar.baseVolume, 14 * Base); EXPECT_EQ(bar.quoteVolume, 1399 * Quote);
    EXPECT_EQ(bar.count, 4); EXPECT_EQ(bar.buyVol, 5 * Base);
    EXPECT_EQ(bar.sellVol, 4 * Base); EXPECT_EQ(bar.delta, Base);
    EXPECT_EQ(bar.lowRow, 99); EXPECT_EQ(bar.maxRowVol, 4 * Base);
    ASSERT_EQ(bar.rows.size(), 3);
    EXPECT_EQ(bar.rows[0], (Row{0, 4 * Base, 0, 1, 0}));
    EXPECT_EQ(bar.rows[1], (Row{1, 0, 2 * Base, 0, 1}));
    EXPECT_EQ(bar.rows[2], (Row{2, 0, 3 * Base, 0, 1}));
    ASSERT_EQ(bar.notable.size(), 3);
    EXPECT_EQ(bar.notable[0].tradeId, "100");
    EXPECT_EQ(bar.notable[0].size, 5 * Base);
    EXPECT_EQ(bar.notable[0].notional, 503 * Quote);
    EXPECT_EQ(bar.notable[0].sweepLevels, 2);
    EXPECT_EQ(bar.notable[0].minPrice, 100);
    EXPECT_EQ(bar.notable[0].maxPrice, 101);
}

TEST(FlowBar, NonDyadicComposeIsByteEqualAndProvisionalReadsDoNotRevise) {
    auto minute = builder(), direct = builder(300'000);
    const double sizes[] = {0.1, 0.1, 0.1, 0.1, 0.3, 0.1, 0.1, 0.00012345};
    const int64_t times[] = {0, 1, 2, 60'000, 60'001, 120'000, 180'000, 240'000};
    for (int i = 0; i < 8; ++i) {
        Trade t{std::to_string(i + 10), times[i],
                100.01 + (i % 3) * 0.01, sizes[i], i % 2 ? Side::Sell : Side::Buy};
        accepted(minute, t); accepted(direct, t);
        minute.bars();
    }
    auto smaller = minute.bars();
    const auto composed = composeFlowBars(smaller, 300'000);
    const auto built = direct.bars().front();
    EXPECT_EQ(minute.revisionAt(0), 0);
    EXPECT_EQ(encodeFlowBar(composed), encodeFlowBar(built));
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(built)), built);
    DayFile day;
    day.product = "BTC-USD"; day.dayStartMs = 0; day.timeframeMs = 60'000;
    day.nativeTick = 1; day.priceScale = 100; day.baseScale = Base;
    day.quoteScale = Quote; day.sourceFingerprint.fill(0x5a); day.bars = smaller;
    day.revisions.resize(smaller.size(), 0);
    EXPECT_EQ(decodeFlowDay(encodeFlowDay(day)), day);
}

TEST(FlowBar, ComposeFloorsStartAndRejectsNextBucketOrWrongInputTimeframe) {
    auto minute = builder();
    accepted(minute, {"1", 60'000, 100, 1, Side::Buy});
    accepted(minute, {"2", 300'000, 101, 1, Side::Buy});
    const auto bars = minute.bars();
    ASSERT_EQ(bars.size(), 2);
    EXPECT_EQ(composeFlowBars(std::span<const FlowBar>(bars.data(), 1), 300'000).startMs, 0);
    EXPECT_THROW(composeFlowBars(bars, 300'000), std::invalid_argument);
    EXPECT_THROW(composeFlowBars(bars, 100'000), std::invalid_argument);
}

TEST(FlowBar, ExplicitSealRevisionAndDrop) {
    auto live = builder(), history = builder();
    Trade first{"1", 1000, 100, 0.1, Side::Buy};
    Trade late{"2", 500, 101, 0.2, Side::Sell};
    accepted(live, first); live.bars();
    accepted(live, late);
    accepted(history, first); accepted(history, late);
    EXPECT_EQ(encodeFlowBar(live.bars().front()), encodeFlowBar(history.bars().front()));
    live.sealThrough(60'000);
    accepted(live, {"3", 2000, 102, 0.1, Side::Buy});
    EXPECT_EQ(live.revisionAt(0), 1);
    live.dropBefore(60'000);
    EXPECT_TRUE(live.bars().empty());
}

TEST(FlowBar, DroppedBucketRejectsLateTradeAndDropClampsToSeal) {
    auto live = builder();
    accepted(live, {"1", 1000, 100, 1, Side::Buy});
    EXPECT_THROW(live.sealThrough(30'000), std::invalid_argument);
    live.sealThrough(60'000);
    live.dropBefore(120'000);
    EXPECT_TRUE(live.bars().empty());
    EXPECT_EQ(live.add({"2", 2000, 101, 1, Side::Sell}),
              FlowBarBuilder::AddResult::Rejected);
    EXPECT_EQ(live.droppedRejectCount(), 1);
    EXPECT_EQ(live.rejectedCount(), 1);
    EXPECT_TRUE(live.bars().empty());
    accepted(live, {"3", 60'000, 102, 1, Side::Buy});
    ASSERT_EQ(live.bars().size(), 1);
    EXPECT_EQ(live.bars().front().startMs, 60'000);
}

TEST(FlowBar, EmptySealedMinuteRevisesButContentMatchesHistory) {
    auto live = builder(), history = builder();
    live.sealThrough(60'000);
    Trade first{"1", 1000, 100, 0.1, Side::Buy};
    Trade second{"2", 2000, 101, 0.2, Side::Sell};
    accepted(live, first);
    EXPECT_EQ(live.revisionAt(0), 1);
    accepted(live, second);
    EXPECT_EQ(live.revisionAt(0), 2);
    accepted(history, first); accepted(history, second);
    EXPECT_EQ(encodeFlowBar(live.bars().front()), encodeFlowBar(history.bars().front()));
    DayFile day;
    day.product = "BTC-USD"; day.dayStartMs = 0; day.timeframeMs = 60'000;
    day.nativeTick = 1; day.priceScale = 100; day.baseScale = Base;
    day.quoteScale = Quote; day.bars = live.bars(); day.revisions = {*live.revisionAt(0)};
    EXPECT_EQ(decodeFlowDay(encodeFlowDay(day)), day);
}

TEST(FlowBar, QuoteRoundHalfUpUsesExactUnits) {
    FlowBarBuilder b(60'000, 0.5, 2, 1, 1);
    accepted(b, {"tie", 0, 1.5, 3002399751580331.0, Side::Buy});
    EXPECT_EQ(b.bars().front().quoteVolume, 4503599627370497LL);
}

TEST(FlowBar, RecorderRowFloorAndRejectedInputs) {
    auto b = builder();
    accepted(b, {"1", -1, 100123.6, 1, Side::Buy});
    EXPECT_EQ(b.bars().front().lowRow, 100123);
    EXPECT_EQ(b.bars().front().startMs, -60'000);
    EXPECT_EQ(b.add({"bad", 0, std::numeric_limits<double>::quiet_NaN(), 1, Side::Buy}),
              FlowBarBuilder::AddResult::Rejected);
    EXPECT_EQ(b.add({"far", -2, 140000, 1, Side::Buy}), FlowBarBuilder::AddResult::Rejected);
    EXPECT_EQ(b.add({"side", 0, 100, 1, static_cast<Side>(3)}),
              FlowBarBuilder::AddResult::Rejected);
    EXPECT_EQ(b.rejectedCount(), 3);
    EXPECT_EQ(b.bars().front().count, 1);
}

TEST(FlowBar, MixedIdsHaveDeterministicTimeTieOrder) {
    auto b = builder();
    accepted(b, {"15x", 0, 103, 1, Side::Buy});
    accepted(b, {"10", 0, 102, 1, Side::Buy});
    accepted(b, {"2", 0, 101, 1, Side::Buy});
    EXPECT_EQ(b.bars().front().open, 101);
    EXPECT_EQ(b.bars().front().close, 103);
}

TEST(FlowBar, CodecRejectsNonfiniteAndInvalidNotable) {
    auto b = builder();
    accepted(b, {"1", 0, 100, 1, Side::Buy});
    accepted(b, {"2", 1, 101, 2, Side::Sell});
    auto bar = b.bars().front();
    auto frame = encodeFlowBar(bar);
    frame[124] ^= 1; // Delta's fixed offset in FLB1.
    EXPECT_THROW(decodeFlowBar(frame), std::invalid_argument);
    frame = encodeFlowBar(bar);
    std::fill(frame.begin() + 140, frame.begin() + 148, 0);
    EXPECT_THROW(decodeFlowBar(frame), std::invalid_argument);
    frame = encodeFlowBar(bar);
    frame[frame.size() - 2] = 0; frame[frame.size() - 1] = 0;
    EXPECT_THROW(decodeFlowBar(frame), std::invalid_argument);
    bar.open = std::numeric_limits<double>::infinity();
    EXPECT_THROW(encodeFlowBar(bar), std::invalid_argument);
    bar.open = 100;
    std::reverse(bar.notable.begin(), bar.notable.end());
    EXPECT_THROW(encodeFlowBar(bar), std::invalid_argument);
}
