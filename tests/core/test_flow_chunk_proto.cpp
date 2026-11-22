// The lean flow chunk prototype (flow_chunk_proto.hpp) that flow_history_bench
// --columnar measures for docs/FLOW_CHUNKS.md: payload round trip, golden
// bytes, decoder checks, day file and 16-day assembly.
#include "flow_chunk_proto.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCompose.hpp"
#include <gtest/gtest.h>

using namespace sentinel::flow;
namespace {
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
constexpr int64_t T0 = 1'763'596'800'000; // 2025-11-20 00:00 UTC, an hour and day start
constexpr int64_t Base = 100'000'000, Quote = 100'000'000;
constexpr proto::Encoding encodings[] = {proto::Encoding::Spec, proto::Encoding::Varint, proto::Encoding::Fixed};

FlowBar strip(FlowBar b) { b.notable.clear(); return b; }
std::vector<uint8_t> bytesOf(std::initializer_list<int> list) { return {list.begin(), list.end()}; }

// Builder bars on a $0.01 tick with $1 rows (BTC-like): sparse minutes, a price
// drop (negative open delta), unknown-side prints and row gaps.
std::vector<FlowBar> hourBars() {
    FlowBarBuilder builder(Minute, 1, 100, Base, Quote);
    int id = 1;
    const auto add = [&](int minute, int ms, double price, double size, Side side) {
        ASSERT_EQ(builder.add({std::to_string(id++), T0 + minute * Minute + ms, price, size, side}),
                  FlowBarBuilder::AddResult::Accepted);
    };
    add(0, 5, 91234.56, 0.5, Side::Buy);
    add(0, 9, 91230.01, 0.25, Side::Sell);
    add(0, 30, 91299.99, 0.001, Side::Unknown);
    add(3, 0, 91100.10, 2.0, Side::Sell);
    add(3, 59'999, 91150.00, 0.1, Side::Buy);
    add(4, 1, 91500.00, 1.5, Side::Buy);
    add(59, 100, 90000.00, 0.00012345, Side::Sell);
    add(59, 200, 90012.34, 3, Side::Buy);
    auto bars = builder.bars();
    EXPECT_EQ(bars.size(), 4u);
    return bars;
}
// One day bar built by the builder on its own grid: rows of `tick` dollars.
FlowBar dayBar(int64_t dayStart, double tick, double price) {
    FlowBarBuilder builder(Day, tick, 100, Base, Quote);
    int id = 1;
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(builder.add({std::to_string(id++), dayStart + i * Hour, price + i * 7.31, 0.5 + i,
                               i % 2 ? Side::Sell : Side::Buy}), FlowBarBuilder::AddResult::Accepted);
    return builder.bars().front();
}
proto::FlowChunk decodeSpec(std::span<const uint8_t> b, int64_t level, int64_t start, uint32_t slots,
                            bool summary = false) {
    return proto::decode(b, proto::Encoding::Spec, level, start, slots, summary);
}
}

TEST(FlowChunkProto, HourChunkRoundTripsInEveryEncoding) {
    const auto bars = hourBars();
    proto::Stats stats;
    const auto chunk = proto::fromBars(bars, Minute, T0, 60, &stats);
    EXPECT_EQ(stats.lowRowMismatch, 0u);
    EXPECT_EQ(stats.nativeTickMismatch, 0u);
    EXPECT_EQ(chunk.slot, (std::vector<uint16_t>{0, 3, 4, 59}));
    EXPECT_EQ(chunk.open.front(), 9'123'456);
    for (const auto e : encodings) {
        const auto decoded = proto::decode(proto::encode(chunk, e), e, Minute, T0, 60);
        EXPECT_EQ(decoded, chunk) << int(e);
        const auto back = proto::toBars(decoded);
        ASSERT_EQ(back.size(), bars.size());
        for (size_t i = 0; i < bars.size(); ++i) EXPECT_EQ(back[i], strip(bars[i])) << int(e) << ' ' << i;
    }
}

// Section 4 bytes written out by hand: one bar in slot 2 of a 1m hour chunk.
TEST(FlowChunkProto, GoldenSpecBytes) {
    FlowBar b;
    b.startMs = T0 + 2 * Minute; b.timeframeMs = Minute; b.nativeTick = 1;
    b.priceScale = 100; b.baseScale = 100; b.quoteScale = Quote;
    b.open = 100.00; b.high = 101.50; b.low = 99.90; b.close = 101.00; b.lowRow = 99;
    b.baseVolume = 5; b.quoteVolume = 7; b.buyVol = 3; b.sellVol = 2; b.delta = 1; b.count = 4; b.maxRowVol = 3;
    b.rows = {{0, 2, 0, 1, 0}, {2, 0, 3, 0, 2}};
    const auto chunk = proto::fromBars(std::span(&b, 1), Minute, T0, 60);
    const auto summary = bytesOf({
        0x01,                                            // grid count
        0x64, 0, 0, 0, 0, 0, 0, 0,                       // row tick units 100
        0x64, 0, 0, 0, 0, 0, 0, 0,                       // price scale 100
        0x64, 0, 0, 0, 0, 0, 0, 0,                       // base scale 100
        0x00, 0xE1, 0xF5, 0x05, 0, 0, 0, 0,              // quote scale 10^8
        0x01, 0x00, 0x00, 0x3C, 0x00, 0x01,              // one run [0, 60) complete
        0x01,                                            // n
        0x02,                                            // slot gap: 2 - (-1) - 1
        0xA0, 0x9C, 0x01,                                // open: zigzag(10000) = 20000
        0x96, 0x01,                                      // up: 150
        0x0A,                                            // down: 10
        0x6E,                                            // close - low: 110
        0x05, 0x07, 0x03, 0x02, 0x04});                  // base, quote, buy, sell, count
    auto full = summary;
    for (int v : {0x02,                                  // row count
                  0x00, 0x01,                            // offset gaps: 0, then 2 - 0 - 1
                  0x02, 0x00, 0x00, 0x03,                // bid volumes, ask volumes
                  0x01, 0x00, 0x00, 0x02})               // bid counts, ask counts
        full.push_back(uint8_t(v));
    EXPECT_EQ(proto::encode(chunk, proto::Encoding::Spec), full);
    EXPECT_EQ(proto::encode(chunk, proto::Encoding::Spec, true), summary);
    EXPECT_EQ(proto::toBars(decodeSpec(full, Minute, T0, 60)).front(), b);
    const auto summaryOnly = decodeSpec(summary, Minute, T0, 60, true);
    EXPECT_EQ(summaryOnly.close.front(), 10'100);
    EXPECT_TRUE(summaryOnly.rows.empty());
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

TEST(FlowChunkProto, SpecIsSmallestAndFixedIsTheInMemoryWidth) {
    const auto chunk = proto::fromBars(hourBars(), Minute, T0, 60);
    const auto fixed = proto::encode(chunk, proto::Encoding::Fixed);
    const auto varint = proto::encode(chunk, proto::Encoding::Varint);
    const auto spec = proto::encode(chunk, proto::Encoding::Spec);
    // Fixed: grid table 1 + 32, one run 1 + 5, u16 count; per bar u16 slot, 8 x i64, u32 count, u16 row count;
    // per row u16 offset + 24 bytes.
    EXPECT_EQ(fixed.size(), 33 + 6 + 2 + chunk.bars() * (2 + 64 + 4 + 2) + chunk.rows.size() * (2 + 24));
    EXPECT_LT(varint.size(), fixed.size());
    EXPECT_LT(spec.size(), varint.size());
}

TEST(FlowChunkProto, WidestInt16OffsetRoundTrips) {
    FlowBar b;
    b.startMs = T0; b.timeframeMs = Minute; b.nativeTick = 1; b.priceScale = 100;
    b.baseScale = Base; b.quoteScale = Quote;
    b.open = b.low = 100; b.high = b.close = 100 + INT16_MAX;
    b.lowRow = 100;
    b.rows = {{0, 5, 0, 1, 0}, {INT16_MAX, 0, 7, 0, 2}};
    b.baseVolume = 12; b.sellVol = 5; b.buyVol = 7; b.delta = 2; b.maxRowVol = 7; b.count = 3;
    const auto chunk = proto::fromBars(std::span(&b, 1), Minute, T0, 60);
    for (const auto e : encodings)
        EXPECT_EQ(proto::toBars(proto::decode(proto::encode(chunk, e), e, Minute, T0, 60)).front(), b) << int(e);
}

TEST(FlowChunkProto, DayChunkKeepsEachBarsGrid) {
    const auto day1 = dayBar(T0, 1, 91000.12);
    const auto day2 = dayBar(T0 + Day, 5, 91000.12); // the grid changes at midnight: $5 rows
    // Rows are built on each day's own grid: the last offset is floor(high / tick) - floor(low / tick).
    EXPECT_EQ(day1.rows.back().offset, 91036 - 91000);
    EXPECT_EQ(day2.rows.back().offset, 91036 / 5 - 91000 / 5);
    const std::vector<FlowBar> days{day1, day2};
    const auto chunk = proto::fromBars(days, Day, T0, 16);
    ASSERT_EQ(chunk.grids.size(), 2u);
    EXPECT_EQ(chunk.grids[1].rowTickUnits, 500);
    EXPECT_EQ(chunk.grid, (std::vector<uint8_t>{0, 1}));
    for (const auto e : encodings) {
        const auto back = proto::toBars(proto::decode(proto::encode(chunk, e), e, Day, T0, 16));
        ASSERT_EQ(back.size(), 2u);
        EXPECT_EQ(back[0], strip(day1)) << int(e);
        EXPECT_EQ(back[1], strip(day2)) << int(e);
        EXPECT_EQ(back[1].lowRow, 91000 / 5);
        EXPECT_DOUBLE_EQ(back[1].low, 91000.12);
    }
}

// Multi-day composition (1w) is out of scope because it can mix grids.
TEST(FlowChunkProto, ComposeRefusesMixedGrids) {
    const std::vector<FlowBar> days{dayBar(T0, 1, 91000.12), dayBar(T0 + Day, 5, 91000.12)};
    EXPECT_THROW(composeFlowBars(days, 7 * Day), std::invalid_argument);
}

TEST(FlowChunkProto, SixteenDayAssemblyFromDayPieces) {
    const int64_t start = T0 - 3 * Day;
    std::vector<FlowBar> bars;
    std::vector<std::vector<uint8_t>> pieces(16);
    for (int day : {0, 1, 5}) {
        bars.push_back(dayBar(start + day * Day, day == 5 ? 5 : 1, 90000.5 + day));
        pieces[day] = proto::encode(proto::fromBars(std::span(&bars.back(), 1), Day, start + day * Day, 1),
                                    proto::Encoding::Spec);
    }
    // Day 2 is sealed with no trades (an observed empty day).
    pieces[2] = proto::encode(proto::fromBars({}, Day, start + 2 * Day, 1), proto::Encoding::Spec);
    const auto assembled = proto::assembleDayChunk(start, pieces, proto::Status::Unrecorded);
    auto expected = proto::fromBars(bars, Day, start, 16);
    expected.coverage = {{0, 3, proto::Status::Complete}, {3, 5, proto::Status::Unrecorded},
                         {5, 6, proto::Status::Complete}, {6, 16, proto::Status::Unrecorded}};
    EXPECT_EQ(assembled, expected);
    // Re-encoding rebases the open deltas: the wire chunk equals a direct encoding.
    EXPECT_EQ(proto::encode(assembled, proto::Encoding::Spec), proto::encode(expected, proto::Encoding::Spec));
}

proto::DayFile sampleDayFile() {
    proto::DayFile d;
    d.instrument = "coinbase:BTC-USD"; d.dayStartMs = T0; d.grid = {100, 100, Base, Quote};
    d.lateTrades = 2; d.regressions = 1;
    const auto chunk = proto::fromBars(hourBars(), Minute, T0, 60);
    d.sections[0] = {true, uint32_t(proto::encode(chunk, proto::Encoding::Spec, true).size()), 0x1234,
                     proto::encode(chunk, proto::Encoding::Spec)};
    const auto empty = proto::fromBars({}, Minute, T0 + 3 * Hour, 60);
    d.sections[3] = {true, uint32_t(proto::encode(empty, proto::Encoding::Spec, true).size()), 0x99,
                     proto::encode(empty, proto::Encoding::Spec)};
    return d;
}
size_t tableStart(const std::vector<uint8_t>& file) { return size_t(file[6]) | size_t(file[7]) << 8; }

TEST(FlowChunkProto, DayFileRoundTripAndValidation) {
    const auto d = sampleDayFile();
    EXPECT_EQ(d.sections[3].summaryLength, d.sections[3].payload.size()); // no bars: the rows section is empty
    const auto bytes = proto::encodeDayFile(d);
    EXPECT_EQ(proto::parseDayFile(bytes), d);
    auto corrupt = bytes;
    corrupt.back() ^= 1;                                   // a section byte
    EXPECT_ANY_THROW(proto::parseDayFile(corrupt));
    corrupt = bytes;
    corrupt[20] ^= 1;                                      // a header byte
    EXPECT_ANY_THROW(proto::parseDayFile(corrupt));
    EXPECT_ANY_THROW(proto::parseDayFile(std::span(bytes).first(bytes.size() - 1)));
    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_ANY_THROW(proto::parseDayFile(trailing));
}

// The table is covered by its own CRC: a length or a fingerprint changed while
// every payload byte stays intact is refused.
TEST(FlowChunkProto, DayFileTableCorruptionIsRefused) {
    const auto bytes = proto::encodeDayFile(sampleDayFile());
    const size_t entry0 = tableStart(bytes);
    for (size_t field : {size_t(4), size_t(8), size_t(17)}) { // summary length, total length, fingerprint
        auto corrupt = bytes;
        corrupt[entry0 + field] ^= 1;
        EXPECT_ANY_THROW(proto::parseDayFile(corrupt)) << field;
    }
}

// Table contents checked even when every CRC is right.
TEST(FlowChunkProto, DayFileTableRules) {
    auto wrongSummary = sampleDayFile();
    --wrongSummary.sections[0].summaryLength;              // not a summary boundary
    EXPECT_ANY_THROW(proto::parseDayFile(proto::encodeDayFile(wrongSummary)));
    auto zeroSummary = sampleDayFile();
    zeroSummary.sections[3].summaryLength = 0;             // a sealed flow section needs its summary
    EXPECT_ANY_THROW(proto::parseDayFile(proto::encodeDayFile(zeroSummary)));
    auto noPayload = sampleDayFile();
    noPayload.sections[5] = {true, 0, 0, {}};              // sealed without a payload
    EXPECT_ANY_THROW(proto::parseDayFile(proto::encodeDayFile(noPayload)));
    auto bigSummary = sampleDayFile();
    bigSummary.sections[30] = {true, 4, 0, {1, 2, 3, 4}};  // trades.big carries no summary length
    EXPECT_ANY_THROW(proto::parseDayFile(proto::encodeDayFile(bigSummary)));
}

TEST(FlowChunkProto, MalformedPayloadsAreRefused) {
    const auto empty = proto::fromBars({}, Minute, T0, 60);
    for (const auto e : encodings) EXPECT_EQ(proto::decode(proto::encode(empty, e), e, Minute, T0, 60), empty);
    const auto good = proto::encode(empty, proto::Encoding::Spec); // grid count 0, one run, n = 0
    ASSERT_EQ(good.back(), 0x00);
    const auto withN = [&](std::initializer_list<int> n) {
        auto b = good;
        b.pop_back();
        for (int v : n) b.push_back(uint8_t(v));
        return b;
    };
    EXPECT_ANY_THROW(decodeSpec(withN({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02}), Minute, T0, 60));
    EXPECT_ANY_THROW(decodeSpec(withN({0x80, 0x00}), Minute, T0, 60)); // noncanonical zero
    EXPECT_ANY_THROW(decodeSpec(withN({0x3D}), Minute, T0, 60));       // 61 bars in 60 slots
    const auto bytes = proto::encode(proto::fromBars(hourBars(), Minute, T0, 60), proto::Encoding::Spec);
    for (size_t cut : {size_t(3), size_t(40), bytes.size() / 2, bytes.size() - 1})
        EXPECT_ANY_THROW(decodeSpec(std::span(bytes).first(cut), Minute, T0, 60)) << cut;
    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_ANY_THROW(decodeSpec(trailing, Minute, T0, 60));
    auto badRun = bytes;
    badRun[33 + 1 + 2] = 59;                                          // run ends at slot 59, not 60
    EXPECT_ANY_THROW(decodeSpec(badRun, Minute, T0, 60));
    auto building = bytes;
    building[33 + 5] = 4;                                             // status 4 (building) is never served
    EXPECT_ANY_THROW(decodeSpec(building, Minute, T0, 60));
    building[33 + 5] = 1;
    EXPECT_NO_THROW(decodeSpec(building, Minute, T0, 60));
    auto inconsistent = hourBars().front();                           // sell volume above its rows' bid sum
    inconsistent.sellVol += 1; inconsistent.baseVolume += 1; inconsistent.delta -= 1;
    EXPECT_ANY_THROW(decodeSpec(proto::encode(proto::fromBars(std::span(&inconsistent, 1), Minute, T0, 60),
                                              proto::Encoding::Spec), Minute, T0, 60));
    EXPECT_ANY_THROW(decodeSpec(bytes, Minute, T0, 59));               // slot 59 outside a 59-slot span
    EXPECT_ANY_THROW(proto::fromBars(hourBars(), Minute, T0 + Minute, 60));
}

// A rule change makes every day pending; an old-rule file is never servable under
// the new identity, even after the pending list is cleared; a restart with
// pending days advances the epoch again.
TEST(FlowChunkProto, RuleChangeFencesEveryOldRuleDay) {
    const std::vector<int64_t> days{T0 - Day, T0};
    proto::EpochState stored{7, 0xAAAA, {}};
    auto oldFile = sampleDayFile();
    oldFile.ruleIdentity = 0xAAAA;
    ASSERT_TRUE(proto::servable(stored, oldFile));
    const auto oldEpoch = proto::epochOf("flow", stored);
    EXPECT_EQ(proto::startupTransition(stored, 0xAAAA, days), stored); // same rules, nothing pending: no change

    auto next = proto::startupTransition(stored, 0xBBBB, days);
    EXPECT_EQ(next.pending, days);
    EXPECT_NE(proto::epochOf("flow", next), oldEpoch);
    EXPECT_FALSE(proto::servable(next, oldFile));
    proto::finishDay(next, T0);                       // T0 rebuilt...
    EXPECT_FALSE(proto::servable(next, oldFile));     // ...but this file is still the old-rule one
    auto rebuilt = oldFile;
    rebuilt.ruleIdentity = 0xBBBB;
    EXPECT_TRUE(proto::servable(next, rebuilt));
    EXPECT_EQ(proto::parseDayFile(proto::encodeDayFile(rebuilt)).ruleIdentity, 0xBBBBu);

    const auto crashed = proto::startupTransition(next, 0xBBBB, days); // restart with T0 - Day still pending
    EXPECT_EQ(crashed.counter, next.counter + 1);
    EXPECT_NE(proto::epochOf("flow", crashed), proto::epochOf("flow", next));
}
