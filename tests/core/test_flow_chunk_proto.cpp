// Round trip of the lean flow chunk prototype (flow_chunk_proto.hpp) that
// flow_history_bench --columnar measures for docs/FLOW_CHUNKS.md.
#include "flow_chunk_proto.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCompose.hpp"
#include <gtest/gtest.h>

using namespace sentinel::flow;
namespace {
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
constexpr int64_t T0 = 1'763'596'800'000; // 2025-11-20 00:00 UTC, an hour and day start
constexpr proto::Encoding encodings[] = {proto::Encoding::Fixed, proto::Encoding::Varint, proto::Encoding::Delta};

FlowBar strip(FlowBar b) { b.notable.clear(); return b; }

// Builder bars on a $0.01 tick, rows of $1 (BTC-like), sparse minutes, price falls and rises,
// unknown-side prints and row gaps.
std::vector<FlowBar> hourBars() {
    FlowBarBuilder builder(Minute, 1, 100, 100'000'000, 100'000'000);
    int id = 1;
    const auto add = [&](int minute, int ms, double price, double size, Side side) {
        ASSERT_EQ(builder.add({std::to_string(id++), T0 + minute * Minute + ms, price, size, side}),
                  FlowBarBuilder::AddResult::Accepted);
    };
    add(0, 5, 91234.56, 0.5, Side::Buy);
    add(0, 9, 91230.01, 0.25, Side::Sell);
    add(0, 30, 91299.99, 0.001, Side::Unknown);
    add(3, 0, 91100.10, 2.0, Side::Sell);       // big drop: negative open delta
    add(3, 59'999, 91150.00, 0.1, Side::Buy);
    add(4, 1, 91500.00, 1.5, Side::Buy);        // rise
    add(59, 100, 90000.00, 0.00012345, Side::Sell);
    add(59, 200, 90012.34, 3, Side::Buy);
    auto bars = builder.bars();
    EXPECT_EQ(bars.size(), 4u);
    return bars;
}
}

using proto::Encoding;

TEST(FlowChunkProto, HourChunkRoundTripsInEveryEncoding) {
    const auto bars = hourBars();
    proto::Stats stats;
    const auto chunk = proto::fromBars(bars, Minute, T0, 60, &stats);
    EXPECT_EQ(stats.lowRowMismatch, 0u);
    EXPECT_EQ(stats.nativeTickMismatch, 0u);
    ASSERT_EQ(chunk.bars(), 4u);
    EXPECT_EQ(chunk.open.front(), 9'123'456);
    for (const auto e : encodings) {
        const auto bytes = proto::encode(chunk, e);
        const auto decoded = proto::decode(bytes);
        EXPECT_EQ(decoded, chunk) << int(e);
        const auto back = proto::toBars(decoded);
        ASSERT_EQ(back.size(), bars.size());
        for (size_t i = 0; i < bars.size(); ++i) EXPECT_EQ(back[i], strip(bars[i])) << int(e) << ' ' << i;
    }
}

TEST(FlowChunkProto, SlotsAreBarPositions) {
    const auto chunk = proto::fromBars(hourBars(), Minute, T0, 60);
    std::vector<uint16_t> expected;
    for (const auto& b : hourBars()) expected.push_back(uint16_t((b.startMs - T0) / Minute));
    EXPECT_EQ(chunk.slot, expected);
    EXPECT_EQ(expected, (std::vector<uint16_t>{0, 3, 4, 59}));
}

TEST(FlowChunkProto, SummaryOnlyIsAPrefixOfTheFullChunk) {
    const auto chunk = proto::fromBars(hourBars(), Minute, T0, 60);
    for (const auto e : encodings) {
        const auto full = proto::encode(chunk, e);
        const auto summary = proto::encode(chunk, e, true);
        ASSERT_LT(summary.size(), full.size());
        EXPECT_TRUE(std::equal(summary.begin(), summary.end(), full.begin())) << int(e);
    }
}

TEST(FlowChunkProto, DeltaIsSmallestAndFixedIsTheInMemoryWidth) {
    const auto chunk = proto::fromBars(hourBars(), Minute, T0, 60);
    const auto fixed = proto::encode(chunk, Encoding::Fixed, false, false);
    const auto varint = proto::encode(chunk, Encoding::Varint, false, false);
    const auto delta = proto::encode(chunk, Encoding::Delta, false, false);
    // Fixed: u16 count; per bar u16 slot, 8 x i64, u32 count, u16 row count; per row i16 offset + 24 bytes.
    EXPECT_EQ(fixed.size(), 2 + chunk.bars() * (2 + 64 + 4 + 2) + chunk.rows.size() * (2 + 24));
    EXPECT_LT(varint.size(), fixed.size());
    EXPECT_LT(delta.size(), varint.size());
}

TEST(FlowChunkProto, WidestInt16OffsetRoundTrips) {
    FlowBar b;
    b.startMs = T0; b.timeframeMs = Minute; b.nativeTick = 1; b.priceScale = 100;
    b.baseScale = 100'000'000; b.quoteScale = 100'000'000;
    b.open = b.low = 100; b.high = b.close = 100 + INT16_MAX;
    b.lowRow = 100;
    b.rows = {{0, 5, 0, 1, 0}, {INT16_MAX, 0, 7, 0, 2}};
    b.baseVolume = 12; b.sellVol = 5; b.buyVol = 7; b.delta = 2; b.maxRowVol = 7; b.count = 3;
    const auto chunk = proto::fromBars(std::span(&b, 1), Minute, T0, 60);
    for (const auto e : encodings) EXPECT_EQ(proto::toBars(proto::decode(proto::encode(chunk, e))).front(), b) << int(e);
}

TEST(FlowChunkProto, DayChunkKeepsEachBarsGrid) {
    auto minute = hourBars();
    auto day1 = composeFlowBars(minute, Day);
    auto day2 = day1;
    day2.startMs += Day;
    day2.nativeTick = 5; // a grid change between days: rows are 5-dollar rows on day 2
    day2.lowRow = recording::floorDiv(std::llround(day2.low * 100), 500);
    const std::vector<FlowBar> days{day1, day2};
    const auto chunk = proto::fromBars(days, Day, T0, 16);
    ASSERT_EQ(chunk.grids.size(), 2u);
    EXPECT_EQ(chunk.grid, (std::vector<uint8_t>{0, 1}));
    for (const auto e : encodings) {
        const auto back = proto::toBars(proto::decode(proto::encode(chunk, e)));
        ASSERT_EQ(back.size(), 2u);
        EXPECT_EQ(back[0], strip(day1)) << int(e);
        EXPECT_EQ(back[1], strip(day2)) << int(e);
    }
}

TEST(FlowChunkProto, EmptyChunkAndMalformedInput) {
    const auto empty = proto::fromBars({}, Minute, T0, 60);
    for (const auto e : encodings) EXPECT_EQ(proto::decode(proto::encode(empty, e)), empty);
    const auto bytes = proto::encode(proto::fromBars(hourBars(), Minute, T0, 60), Encoding::Delta);
    for (size_t cut : {size_t(3), size_t(30), bytes.size() / 2, bytes.size() - 1})
        EXPECT_ANY_THROW(proto::decode(std::span(bytes).first(cut))) << cut;
    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_ANY_THROW(proto::decode(trailing));
    // A bar outside the chunk span is refused at build time.
    auto bars = hourBars();
    EXPECT_ANY_THROW(proto::fromBars(bars, Minute, T0 + Minute, 60));
}
