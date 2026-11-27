#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include "flow/FlowChunkCodec.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
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

TEST(FlowBar, KeepsTradeWhenPositiveQuoteRoundsToZero) {
    FlowBarBuilder b(60'000, 1, 100, 1000, 1);
    accepted(b, {"tiny-buy", 0, 1, .001, Side::Buy});
    accepted(b, {"tiny-sell", 1, 1, .001, Side::Sell});
    const auto bars = b.bars();
    ASSERT_EQ(bars.size(), 1);
    const auto& bar = bars.front();
    EXPECT_EQ(bar.count, 2);
    EXPECT_EQ(bar.baseVolume, 2);
    EXPECT_EQ(bar.quoteVolume, 0);
    EXPECT_EQ(bar.buyVol, 1);
    EXPECT_EQ(bar.sellVol, 1);
    ASSERT_EQ(bar.rows.size(), 1);
    EXPECT_EQ(bar.rows.front(), (Row{0, 1, 1, 1, 1}));
    EXPECT_TRUE(bar.notable.empty());
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(bar)), bar);
}

TEST(FlowBar, RecorderRowFloorAndRejectedInputs) {
    auto b = builder();
    accepted(b, {"1", -1, 100123.6, 1, Side::Buy});
    EXPECT_EQ(b.bars().front().lowRow, 100123);
    EXPECT_EQ(b.bars().front().startMs, -60'000);
    EXPECT_EQ(b.add({"bad", 0, std::numeric_limits<double>::quiet_NaN(), 1, Side::Buy}),
              FlowBarBuilder::AddResult::Rejected);
    accepted(b, {"far", -2, 140000, 1, Side::Buy});
    EXPECT_EQ(b.add({"side", 0, 100, 1, static_cast<Side>(3)}),
              FlowBarBuilder::AddResult::Rejected);
    EXPECT_EQ(b.rejectedCount(), 2);
    EXPECT_EQ(b.bars().front().count, 2);
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

TEST(FlowBar, CodecRejectsRowVolumesThatDisagreeWithSideTotals) {
    FlowBarBuilder b(60'000, 1, 100, 1, 1);
    accepted(b, {"buy", 0, 100, 1, Side::Buy});
    accepted(b, {"sell", 1, 101, 2, Side::Sell});
    accepted(b, {"unknown", 2, 102, 3, Side::Unknown});
    const auto original = b.bars().front();
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(original)), original);
    for (size_t offset : {size_t(153), size_t(158)}) {
        auto frame = encodeFlowBar(original);
        // FLB1's fixed header is 150 bytes; each of these small rows has a two-byte
        // offset and four one-byte varints. Change only one side's row volume.
        ++frame[offset];
        EXPECT_THROW(decodeFlowBar(frame), std::invalid_argument);
        frame[offset] -= 2;
        EXPECT_THROW(decodeFlowBar(frame), std::invalid_argument);
    }
}

TEST(FlowBar, CodecAcceptsSideVolumesEqualToBaseAndRejectsExcess) {
    FlowBarBuilder b(60'000, 1, 100, 1, 1);
    accepted(b, {"buy", 0, 100, 1, Side::Buy});
    accepted(b, {"sell", 1, 101, 2, Side::Sell});
    accepted(b, {"unknown", 2, 102, 3, Side::Unknown});
    auto bar = b.bars().front();
    bar.baseVolume = bar.buyVol + bar.sellVol;
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(bar)), bar);
    --bar.baseVolume;
    EXPECT_THROW(encodeFlowBar(bar), std::invalid_argument);
}

TEST(FlowBar, CodecAcceptsKnownRowCountEqualToBarCountAndRejectsExcess) {
    FlowBarBuilder b(60'000, 1, 100, 1, 1);
    accepted(b, {"buy", 0, 100, 1, Side::Buy});
    accepted(b, {"sell", 1, 101, 2, Side::Sell});
    auto bar = b.bars().front();
    ASSERT_EQ(bar.count, 2u);
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(bar)), bar);
    --bar.count;
    EXPECT_THROW(encodeFlowBar(bar), std::invalid_argument);
}

// The default decode keeps the writer's whole-day check; BarsOnly skips only that
// re-encode and still checks every bar and the index.
TEST(FlowBar, DayDecodeBarsOnlySkipsOnlyTheWholeDayReEncode) {
    auto b = builder();
    accepted(b, {"1", 1'000, 100, 2, Side::Buy});
    accepted(b, {"2", 61'000, 101, 3, Side::Sell});
    DayFile day;
    day.product = "BTC-USD"; day.dayStartMs = 0; day.timeframeMs = 60'000;
    day.nativeTick = 1; day.priceScale = 100; day.baseScale = Base; day.quoteScale = Quote;
    day.bars = b.bars(); day.revisions.assign(day.bars.size(), 1);
    ASSERT_EQ(day.bars.size(), 2u);
    const auto bytes = encodeFlowDay(day);
    EXPECT_EQ(decodeFlowDay(bytes, DayCheck::BarsOnly), decodeFlowDay(bytes));
    // dayStartMs follows the magic and the length-prefixed product. A start that is
    // not on a UTC day passes every per-bar check and fails only the day re-encode.
    auto shifted = bytes;
    const size_t dayStartAt = 4 + 2 + day.product.size();
    ++shifted[dayStartAt];
    EXPECT_ANY_THROW(decodeFlowDay(shifted));
    EXPECT_EQ(decodeFlowDay(shifted, DayCheck::BarsOnly).dayStartMs, 1);
    auto truncated = bytes;
    truncated.pop_back();
    EXPECT_ANY_THROW(decodeFlowDay(truncated, DayCheck::BarsOnly));
}

TEST(FlowBar, BuilderAcceptsOffsetAbove32767AndFlb1RoundTrips) {
    auto b = builder();
    accepted(b, {"lo", 0, 100, 1, Side::Buy});
    accepted(b, {"hi", 1, 40100, 1, Side::Sell});
    const auto bar = b.bars().front();
    EXPECT_EQ(bar.rows.back().offset, 40000); EXPECT_EQ(bar.nativeTick, 1);
    EXPECT_EQ(bar.count, 2); EXPECT_EQ(b.rejectedCount(), 0u);
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(bar)), bar);
}
TEST(FlowBar, BuilderCoarsensWideBarToStepTwo) {
    auto b = builder();
    accepted(b, {"lo", 0, 101, 1, Side::Buy});
    accepted(b, {"same-bin", 1, 100, 2, Side::Sell});
    accepted(b, {"hi", 2, 65635, 3, Side::Buy});
    const auto bar = b.bars().front();
    EXPECT_EQ(bar.nativeTick, 2); EXPECT_EQ(bar.lowRow, 50);
    ASSERT_EQ(bar.rows.size(), 2u);
    EXPECT_EQ(bar.rows.back().offset, 32767);
    EXPECT_EQ(bar.rows.front().bidVol, 2 * Base); EXPECT_EQ(bar.rows.front().askVol, Base);
    EXPECT_EQ(bar.count, 3); EXPECT_EQ(decodeFlowBar(encodeFlowBar(bar)), bar);
}
TEST(FlowBar, DayFileRoundTripsCoarseBarAndComposesHistoryPage) {
    auto b = builder();
    accepted(b, {"fine", 0, 100, 1, Side::Buy});
    accepted(b, {"low", 60000, 100, 1, Side::Buy});
    accepted(b, {"high", 60001, 65635, 1, Side::Sell});
    DayFile day;
    day.product = "BTC-USD"; day.dayStartMs = 0; day.timeframeMs = 60000;
    day.nativeTick = 1; day.priceScale = 100; day.baseScale = Base; day.quoteScale = Quote;
    day.bars = b.bars(); day.revisions.assign(day.bars.size(), 0);
    ASSERT_EQ(day.bars.size(), 2u);
    ASSERT_EQ(day.bars.back().nativeTick, 2);
    const auto bytes = encodeFlowDay(day);
    const auto decoded = decodeFlowDay(bytes, DayCheck::ReEncode);
    EXPECT_EQ(decoded, day);
    EXPECT_EQ(decodeFlowDay(bytes, DayCheck::BarsOnly), day);
    const auto page = composeFlowBars(decoded.bars, 120000);
    EXPECT_EQ(page.nativeTick, 2);
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(page)), page);
    auto mixed = day;
    mixed.bars.back().nativeTick *= 3;
    EXPECT_THROW(encodeFlowDay(mixed), std::invalid_argument);
}
TEST(FlowBar, ComposeStepOnePreservesNativeTickBits) {
    FlowBarBuilder b(60000, 4.9999999999999996e-06, 1'000'000, Base, Quote);
    accepted(b, {"a", 0, 1, 1, Side::Buy});
    accepted(b, {"b", 60000, 1.000005, 1, Side::Sell});
    const auto bars = b.bars();
    ASSERT_EQ(bars.size(), 2u);
    const auto composed = composeFlowBars(bars, 120000);
    EXPECT_EQ(std::bit_cast<uint64_t>(composed.nativeTick), std::bit_cast<uint64_t>(bars.front().nativeTick));
}
TEST(FlowBar, LargePowerOfTwoTicksRemainRepresentable) {
    FlowBarBuilder b(60000, 0.07, 100, Base, Quote);
    accepted(b, {"low", 0, 0.07, 0.000001, Side::Buy});
    accepted(b, {"high", 1, 4e13, 0.000001, Side::Sell});
    auto coarse = b.bars().front();
    EXPECT_EQ(coarse.nativeTick, 0.07 * double(uint64_t(1) << 34));
    EXPECT_NO_THROW(composeFlowBars(std::span(&coarse, 1), 120000));
    const chunks::FlowGrid grid{7, 100, Base, Quote};
    EXPECT_NO_THROW(chunks::fromBars(std::span(&coarse, 1), 60000, 0, 60, nullptr, std::span(&grid, 1)));

    FlowBarBuilder small(60000, 0.07, 100, Base, Quote);
    accepted(small, {"a", 0, 0.07, 1, Side::Buy});
    accepted(small, {"b", 60000, 0.07, 1, Side::Sell});
    auto bars = small.bars();
    bars.back().nativeTick *= double(uint64_t(1) << 48);
    bars.back().lowRow = 0;
    EXPECT_NO_THROW(composeFlowBars(bars, 120000));
    const std::array<chunks::FlowGrid, 2> grids{grid, grid};
    EXPECT_NO_THROW(chunks::fromBars(bars, 60000, 0, 60, nullptr, grids));
}
TEST(FlowBar, ComposeStepsOneAndFourUsesFour) {
    FlowBarBuilder a(60000, 1, 100, Base, Quote), b(60000, 4, 100, Base, Quote);
    accepted(a, {"a", 0, 101, 1, Side::Buy});
    accepted(b, {"b", 60000, 107, 2, Side::Sell});
    std::vector<FlowBar> bars{a.bars().front(), b.bars().front()};
    const auto composed = composeFlowBars(bars, 120000);
    EXPECT_EQ(composed.nativeTick, 4); EXPECT_EQ(composed.lowRow, 25);
    ASSERT_EQ(composed.rows.size(), 2u); EXPECT_EQ(composed.rows.back().offset, 1);
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(composed)), composed);
    bars[1].baseScale *= 2;
    EXPECT_THROW(composeFlowBars(bars, 120000), std::invalid_argument);
}
TEST(FlowBar, ComposeWideRangeChoosesSmallestFittingStep) {
    auto b = builder();
    accepted(b, {"lo", 0, 100, 1, Side::Buy});
    accepted(b, {"hi", 60000, 200100, 1, Side::Sell});
    const auto composed = composeFlowBars(b.bars(), 120000);
    EXPECT_EQ(composed.nativeTick, 4); EXPECT_EQ(composed.rows.back().offset, 50000);
    EXPECT_EQ(decodeFlowBar(encodeFlowBar(composed)), composed);
}
TEST(FlowBar, ComposeStillRefusesMixedGrids) {
    auto b = builder();
    accepted(b, {"a", 0, 100, 1, Side::Buy}); accepted(b, {"b", 60000, 101, 1, Side::Sell});
    const auto original = b.bars();
    for (int field = 0; field < 5; ++field) {
        auto bars = original;
        if (field == 0) bars[1].priceScale *= 2;
        if (field == 1) bars[1].baseScale *= 2;
        if (field == 2) bars[1].quoteScale *= 2;
        if (field == 3) bars[1].nativeTick *= 3;
        if (field == 4) bars[1].nativeTick += 0.001;
        EXPECT_THROW(composeFlowBars(bars, 120000), std::invalid_argument) << field;
    }
}
