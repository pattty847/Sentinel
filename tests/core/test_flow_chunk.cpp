// Production flow chunks measured by flow_history_bench --columnar:
// docs/FLOW_CHUNKS.md payload round trip, golden
// bytes, decoder checks, row steps, day file, 16-day assembly and epochs.
#include "flow/FlowChunkCodec.hpp"
#include "flow/FlowDayFile.hpp"
#include "flow/FlowEpoch.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCompose.hpp"
#include <gtest/gtest.h>

using namespace sentinel::flow;
namespace codec = sentinel::flow::chunks;
namespace {
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
constexpr int64_t T0 = 1'763'596'800'000; // 2025-11-20 00:00 UTC, an hour and day start
constexpr int64_t Base = 100'000'000, Quote = 100'000'000;
codec::ChunkData decodeData(std::span<const uint8_t> bytes, int64_t level, int64_t start, uint32_t slots,
                            bool summary = false, uint64_t step = 1) {
    return codec::toData(codec::decode(bytes, level, start, slots, summary, step));
}

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
codec::ChunkData decodeSpec(std::span<const uint8_t> b, int64_t level, int64_t start, uint32_t slots,
                            bool summary = false, uint64_t step = 1) {
    return decodeData(b, level, start, slots, summary, step);
}
}

TEST(FlowChunkTests, HourChunkRoundTrips) {
    const auto bars = hourBars();
    codec::Stats stats;
    const auto chunk = codec::fromBars(bars, Minute, T0, 60, &stats);
    EXPECT_EQ(stats.lowRowMismatch, 0u);
    EXPECT_EQ(stats.nativeTickMismatch, 0u);
    EXPECT_EQ(chunk.slot, (std::vector<uint16_t>{0, 3, 4, 59}));
    EXPECT_EQ(chunk.open.front(), 9'123'456);
    {
        const auto decoded = decodeData(codec::encode(chunk), Minute, T0, 60);
        EXPECT_EQ(decoded, chunk);
        const auto back = codec::toBars(decoded);
        ASSERT_EQ(back.size(), bars.size());
        for (size_t i = 0; i < bars.size(); ++i) EXPECT_EQ(back[i], strip(bars[i])) << ' ' << i;
    }
}

// Section 4 bytes written out by hand: one bar in slot 2 of a 1m hour chunk.
TEST(FlowChunkTests, GoldenSpecBytes) {
    FlowBar b;
    b.startMs = T0 + 2 * Minute; b.timeframeMs = Minute; b.nativeTick = 1;
    b.priceScale = 100; b.baseScale = 100; b.quoteScale = Quote;
    b.open = 100.00; b.high = 101.50; b.low = 99.90; b.close = 101.00; b.lowRow = 99;
    b.baseVolume = 5; b.quoteVolume = 7; b.buyVol = 3; b.sellVol = 2; b.delta = 1; b.count = 4; b.maxRowVol = 3;
    b.rows = {{0, 2, 0, 1, 0}, {2, 0, 3, 0, 2}};
    const auto chunk = codec::fromBars(std::span(&b, 1), Minute, T0, 60);
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
    EXPECT_EQ(codec::encode(chunk), full);
    EXPECT_EQ(codec::encode(chunk, true), summary);
    EXPECT_EQ(codec::toBars(decodeSpec(full, Minute, T0, 60)).front(), b);
    const auto summaryOnly = decodeSpec(summary, Minute, T0, 60, true);
    EXPECT_EQ(summaryOnly.close.front(), 10'100);
    EXPECT_TRUE(summaryOnly.rows.empty());
}

TEST(FlowChunkTests, SummaryOnlyIsAPrefixOfTheFullChunk) {
    const auto chunk = codec::fromBars(hourBars(), Minute, T0, 60);
    {
        const auto full = codec::encode(chunk);
        const auto summary = codec::encode(chunk, true);
        ASSERT_LT(summary.size(), full.size());
        EXPECT_TRUE(std::equal(summary.begin(), summary.end(), full.begin()));
    }
}

// Offsets are u16: a bar spans at most 65,535 rows (last offset 65534).
TEST(FlowChunkTests, WidestOffsetRoundTrips) {
    FlowBar b;
    b.startMs = T0; b.timeframeMs = Minute; b.nativeTick = 1; b.priceScale = 100;
    b.baseScale = Base; b.quoteScale = Quote;
    b.open = b.low = 100; b.high = b.close = 100 + INT16_MAX;
    b.lowRow = 100;
    b.rows = {{0, 5, 0, 1, 0}, {INT16_MAX, 0, 7, 0, 2}};
    b.baseVolume = 12; b.sellVol = 5; b.buyVol = 7; b.delta = 2; b.maxRowVol = 7; b.count = 3;
    auto chunk = codec::fromBars(std::span(&b, 1), Minute, T0, 60);
    EXPECT_EQ(codec::toBars(decodeData(codec::encode(chunk), Minute, T0, 60)).front(), b);
    // The widest permitted u16 offset also converts back to a working FlowBar.
    chunk.high[0] = chunk.close[0] = chunk.low[0] + codec::MaxOffset * 100; // row tick units 100
    chunk.offset[1] = uint16_t(codec::MaxOffset);
    EXPECT_EQ(decodeData(codec::encode(chunk), Minute, T0, 60), chunk);
    EXPECT_NO_THROW(codec::toBars(chunk));
    auto tooWide = chunk;                                   // 65,536 rows
    tooWide.high[0] = tooWide.close[0] += 100;
    tooWide.offset[1] += 1;
    EXPECT_ANY_THROW(decodeData(codec::encode(tooWide), Minute, T0, 60));
}

// A bar wider than 65,535 native rows is stored at the smallest power-of-two
// step that fits; nothing is rejected for range.
TEST(FlowChunkTests, WideBarTakesTheSmallestFittingStep) {
    constexpr int64_t tick = 100, low = 1000 * tick;
    EXPECT_EQ(codec::stepFor(low, low + codec::MaxOffset * tick, tick), 1u);
    EXPECT_EQ(codec::stepFor(low, low + (codec::MaxOffset + 1) * tick, tick), 2u);
    EXPECT_EQ(codec::stepFor(low, low + 200'000 * tick, tick), 4u);    // 50,000 rows at step 4
    EXPECT_EQ(codec::stepFor(low, low, tick, 8), 8u);                   // never finer than a constituent
    EXPECT_EQ(codec::stepFor(1, INT64_MAX, 1), uint64_t(1) << 48);      // the widest i64 range fits

    // 70,000 native rows (1000..71000) at step 2: rows 500..35500.
    FlowBar b;
    b.startMs = T0; b.timeframeMs = Minute; b.nativeTick = 1; b.priceScale = 100;
    b.baseScale = Base; b.quoteScale = Quote;
    b.open = b.low = 1000; b.high = b.close = 1001; b.lowRow = 1000;
    b.rows = {{0, 5, 0, 1, 0}, {1, 0, 7, 0, 2}};
    b.baseVolume = 12; b.sellVol = 5; b.buyVol = 7; b.delta = 2; b.maxRowVol = 7; b.count = 3;
    auto chunk = codec::fromBars(std::span(&b, 1), Minute, T0, 60);
    chunk.priceStep = 2;
    chunk.high[0] = chunk.close[0] = 71'000 * tick;
    chunk.offset[1] = 35'000;
    EXPECT_EQ(codec::stepFor(chunk.low[0], chunk.high[0], tick), 2u);
    const auto bytes = codec::encode(chunk);
    EXPECT_EQ(decodeSpec(bytes, Minute, T0, 60, false, 2), chunk);
    EXPECT_ANY_THROW(decodeSpec(bytes, Minute, T0, 60, false, 1));      // rows are not native
    EXPECT_ANY_THROW(decodeSpec(bytes, Minute, T0, 60, false, 3));      // not a power of two
    EXPECT_ANY_THROW(decodeSpec(bytes, Minute, T0, 60, false, uint64_t(1) << 49));
    EXPECT_NO_THROW(codec::toBars(chunk));
}

TEST(FlowChunkTests, DayChunkKeepsEachBarsGrid) {
    const auto day1 = dayBar(T0, 1, 91000.12);
    const auto day2 = dayBar(T0 + Day, 5, 91000.12); // the grid changes at midnight: $5 rows
    // Rows are built on each day's own grid: the last offset is floor(high / tick) - floor(low / tick).
    EXPECT_EQ(day1.rows.back().offset, 91036 - 91000);
    EXPECT_EQ(day2.rows.back().offset, 91036 / 5 - 91000 / 5);
    const std::vector<FlowBar> days{day1, day2};
    const auto chunk = codec::fromBars(days, Day, T0, 16);
    ASSERT_EQ(chunk.grids.size(), 2u);
    EXPECT_EQ(chunk.grids[1].rowTickUnits, 500);
    EXPECT_EQ(chunk.grid, (std::vector<uint8_t>{0, 1}));
    {
        const auto back = codec::toBars(decodeData(codec::encode(chunk), Day, T0, 16));
        ASSERT_EQ(back.size(), 2u);
        EXPECT_EQ(back[0], strip(day1));
        EXPECT_EQ(back[1], strip(day2));
        EXPECT_EQ(back[1].lowRow, 91000 / 5);
        EXPECT_DOUBLE_EQ(back[1].low, 91000.12);
    }
}

// Multi-day composition (1w) is out of scope because it can mix grids.
TEST(FlowChunkTests, ComposeRefusesMixedGrids) {
    const std::vector<FlowBar> days{dayBar(T0, 1, 91000.12), dayBar(T0 + Day, 5, 91000.12)};
    EXPECT_THROW(composeFlowBars(days, 7 * Day), std::invalid_argument);
}

TEST(FlowChunkTests, SixteenDayAssemblyFromDayPieces) {
    const int64_t start = T0 - 3 * Day;
    std::vector<FlowBar> bars;
    std::vector<std::vector<uint8_t>> pieces(16);
    for (int day : {0, 1, 5}) {
        bars.push_back(dayBar(start + day * Day, day == 5 ? 5 : 1, 90000.5 + day));
        pieces[day] = codec::encode(codec::fromBars(std::span(&bars.back(), 1), Day, start + day * Day, 1));
    }
    // Day 2 is sealed with no trades (an observed empty day).
    pieces[2] = codec::encode(codec::fromBars({}, Day, start + 2 * Day, 1));
    const auto assembled = codec::assembleDayChunk(start, pieces, codec::Status::Unrecorded);
    auto expected = codec::fromBars(bars, Day, start, 16);
    expected.coverage = {{0, 3, codec::Status::Complete}, {3, 5, codec::Status::Unrecorded},
                         {5, 6, codec::Status::Complete}, {6, 16, codec::Status::Unrecorded}};
    EXPECT_EQ(assembled, expected);
    // Re-encoding rebases the open deltas: the wire chunk equals a direct encoding.
    EXPECT_EQ(codec::encode(assembled), codec::encode(expected));
}

// Pieces at different steps compose at the coarsest: finer pieces are re-binned.
TEST(FlowChunkTests, SixteenDayAssemblyComposesAtTheCoarsestStep) {
    const int64_t start = T0 - 3 * Day;
    const auto day0 = dayBar(start, 1, 91000.12), day1 = dayBar(start + Day, 1, 91000.12);
    std::vector<std::vector<uint8_t>> pieces(16);
    std::vector<uint64_t> steps(16, 1);
    pieces[0] = codec::encode(codec::fromBars(std::span(&day0, 1), Day, start, 1));
    pieces[1] = codec::encode(codec::rebin(codec::fromBars(std::span(&day1, 1), Day, start + Day, 1), 8));
    steps[1] = 8;
    const auto assembled = codec::assembleDayChunk(start, pieces, codec::Status::Unrecorded, steps);
    EXPECT_EQ(assembled.priceStep, 8u);
    // Native rows 91000, 91007, 91014, 91022, 91029, 91036 at step 8: 11375 (the first two merged) .. 11379.
    const std::vector<uint16_t> offsets{0, 1, 2, 3, 4, 0, 1, 2, 3, 4};
    EXPECT_EQ(assembled.offset, offsets);
    EXPECT_EQ(assembled.rowBegin, (std::vector<uint32_t>{0, 5, 10}));
    EXPECT_EQ(assembled.rows[0], (codec::FlowRow{150'000'000, 50'000'000, 1, 1})); // sell 1.5 + buy 0.5
    const auto wire = codec::encode(assembled);
    EXPECT_EQ(decodeSpec(wire, Day, start, 16, false, 8), assembled);
    EXPECT_ANY_THROW(decodeSpec(wire, Day, start, 16, false, 1));
}

// Hour 0 is stored at step 2 (step exponent 1), hour 3 is an observed empty hour.
codec::DayFile sampleDayFile() {
    codec::DayFile d;
    d.instrument = "coinbase:BTC-USD"; d.dayStartMs = T0; d.grid = {100, 100, Base, Quote};
    d.lateTrades = 2; d.regressions = 1;
    const auto chunk = codec::rebin(codec::fromBars(hourBars(), Minute, T0, 60), 2);
    d.sections[0] = {true, uint32_t(codec::encode(chunk, true).size()), 0x1234,
                     codec::encode(chunk), 1};
    const auto empty = codec::fromBars({}, Minute, T0 + 3 * Hour, 60);
    d.sections[3] = {true, uint32_t(codec::encode(empty, true).size()), 0x99,
                     codec::encode(empty)};
    return d;
}
size_t tableStart(const std::vector<uint8_t>& file) { return size_t(file[6]) | size_t(file[7]) << 8; }

TEST(FlowChunkTests, DayFileRoundTripAndValidation) {
    const auto d = sampleDayFile();
    EXPECT_EQ(d.sections[3].summaryLength, d.sections[3].payload.size()); // no bars: the rows section is empty
    const auto bytes = codec::encodeDayFile(d);
    EXPECT_EQ(codec::parseDayFile(bytes), d);
    auto corrupt = bytes;
    corrupt.back() ^= 1;                                   // a section byte
    EXPECT_ANY_THROW(codec::parseDayFile(corrupt));
    corrupt = bytes;
    corrupt[20] ^= 1;                                      // a header byte
    EXPECT_ANY_THROW(codec::parseDayFile(corrupt));
    EXPECT_ANY_THROW(codec::parseDayFile(std::span(bytes).first(bytes.size() - 1)));
    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_ANY_THROW(codec::parseDayFile(trailing));
}

// The table is covered by its own CRC: a length or a fingerprint changed while
// every payload byte stays intact is refused.
TEST(FlowChunkTests, DayFileTableCorruptionIsRefused) {
    const auto bytes = codec::encodeDayFile(sampleDayFile());
    const size_t entry0 = tableStart(bytes);
    for (size_t field : {size_t(4), size_t(8), size_t(17), size_t(25)}) { // summary, total length, fingerprint, step
        auto corrupt = bytes;
        corrupt[entry0 + field] ^= 1;
        EXPECT_ANY_THROW(codec::parseDayFile(corrupt)) << field;
    }
}

// Table contents checked even when every CRC is right.
TEST(FlowChunkTests, DayFileTableRules) {
    auto wrongSummary = sampleDayFile();
    --wrongSummary.sections[0].summaryLength;              // not a summary boundary
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(wrongSummary)));
    auto zeroSummary = sampleDayFile();
    zeroSummary.sections[3].summaryLength = 0;             // a sealed flow section needs its summary
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(zeroSummary)));
    auto noPayload = sampleDayFile();
    noPayload.sections[5] = {true, 0, 0, {}};              // sealed without a payload
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(noPayload)));
    auto bigSummary = sampleDayFile();
    bigSummary.sections[30] = {true, 4, 0, {1, 2, 3, 4}};  // trades.big carries no summary length
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(bigSummary)));
    auto wrongStep = sampleDayFile();
    wrongStep.sections[0].stepExponent = 0;                // hour 0 was stored at step 2
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(wrongStep)));
    auto absentStep = sampleDayFile();
    absentStep.sections[5].stepExponent = 1;               // an absent entry has no step
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(absentStep)));
    auto bigStep = sampleDayFile();
    bigStep.sections[30] = {true, 0, 0, {1, 2, 3, 4}, 1};  // nor does trades.big
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(bigStep)));
    auto coarse = sampleDayFile();
    coarse.sections[3].stepExponent = 49;                  // above 2^48
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(coarse)));
}

TEST(FlowChunkTests, MalformedPayloadsAreRefused) {
    const auto empty = codec::fromBars({}, Minute, T0, 60);
    EXPECT_EQ(decodeData(codec::encode(empty), Minute, T0, 60), empty);
    const auto good = codec::encode(empty); // grid count 0, one run, n = 0
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
    const auto bytes = codec::encode(codec::fromBars(hourBars(), Minute, T0, 60));
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
    EXPECT_ANY_THROW(decodeSpec(codec::encode(codec::fromBars(std::span(&inconsistent, 1), Minute, T0, 60)), Minute, T0, 60));
    EXPECT_ANY_THROW(decodeSpec(bytes, Minute, T0, 59));               // slot 59 outside a 59-slot span
    EXPECT_ANY_THROW(codec::fromBars(hourBars(), Minute, T0 + Minute, 60));
}

// A rule change makes every day pending; an old-rule file is never servable under
// the new identity, even after the pending list is cleared; a restart with
// pending days advances the epoch again.
TEST(FlowChunkTests, RuleChangeFencesEveryOldRuleDay) {
    const std::vector<int64_t> days{T0 - Day, T0};
    codec::EpochState stored{7, 0xAAAA, {}};
    auto oldFile = sampleDayFile();
    oldFile.ruleIdentity = 0xAAAA;
    ASSERT_TRUE(codec::servable(stored, oldFile));
    const auto domain = codec::flowDomain("coinbase:BTC-USD");
    const auto oldEpoch = codec::epochOf(domain, stored);
    EXPECT_EQ(codec::startupTransition(stored, 0xAAAA, days), stored); // same rules, nothing pending: no change

    auto next = codec::startupTransition(stored, 0xBBBB, days);
    EXPECT_EQ(next.pending, days);
    EXPECT_NE(codec::epochOf(domain, next), oldEpoch);
    EXPECT_FALSE(codec::servable(next, oldFile));
    codec::finishDay(next, T0);                       // T0 rebuilt...
    EXPECT_FALSE(codec::servable(next, oldFile));     // ...but this file is still the old-rule one
    auto rebuilt = oldFile;
    rebuilt.ruleIdentity = 0xBBBB;
    EXPECT_TRUE(codec::servable(next, rebuilt));
    EXPECT_EQ(codec::parseDayFile(codec::encodeDayFile(rebuilt)).ruleIdentity, 0xBBBBu);

    const auto crashed = codec::startupTransition(next, 0xBBBB, days); // restart with T0 - Day still pending
    EXPECT_EQ(crashed.counter, next.counter + 1);
    EXPECT_NE(codec::epochOf(domain, crashed), codec::epochOf(domain, next));
}

// Each instrument has its own epoch domain and file: a rule change for one (its
// big-trade floor) moves only its epoch and fences only its days.
TEST(FlowChunkTests, EpochDomainIsPerInstrument) {
    const std::vector<int64_t> days{T0 - Day, T0};
    const codec::EpochState btc{7, 0xAAAA, {}}, eth{7, 0xAAAA, {}};
    EXPECT_NE(codec::epochOf(codec::flowDomain("coinbase:BTC-USD"), btc),
              codec::epochOf(codec::flowDomain("coinbase:ETH-USD"), eth));
    const auto btcNext = codec::startupTransition(btc, 0xBBBB, days); // BTC's floor changed
    const auto ethNext = codec::startupTransition(eth, 0xAAAA, days); // ETH's rules did not
    EXPECT_EQ(btcNext.pending, days);
    EXPECT_EQ(ethNext, eth);
    auto ethFile = sampleDayFile();
    ethFile.instrument = "coinbase:ETH-USD";
    ethFile.ruleIdentity = 0xAAAA;
    EXPECT_TRUE(codec::servable(ethNext, ethFile));
}

namespace {
// Deliberately raw writer: malformed cases reach the decoder even when the
// production encoder correctly refuses the same invalid value.
struct Payload {
    uint8_t grids = 1, gridIndex = 0;
    int64_t tick = 1;
    uint16_t slots = 60;
    std::vector<codec::CoverageRun> runs;
    uint64_t n = 1, gap = 0, open = 200, up = 2, down = 0, close = 2;
    uint64_t base = 3, quote = 300, buy = 2, sell = 1, count = 2;
    uint64_t rowCount = 2;
    std::vector<uint64_t> offsets{0, 1}, bids{1, 0}, asks{0, 2}, bidCounts{1, 0}, askCounts{0, 1};
    std::vector<uint8_t> openBytes;
    std::vector<uint8_t> bytes(bool summary = false) const {
        std::vector<uint8_t> out;
        const auto fixed = [&](uint64_t v, size_t width) {
            for (size_t i = 0; i < width; ++i) out.push_back(uint8_t(v >> (i * 8)));
        };
        const auto var = [&](uint64_t v) {
            while (v >= 128) { out.push_back(uint8_t(v) | 128); v >>= 7; }
            out.push_back(uint8_t(v));
        };
        fixed(grids, 1);
        for (size_t i = 0; i < grids; ++i) { fixed(uint64_t(tick), 8); for (int j = 0; j < 3; ++j) fixed(1, 8); }
        const auto coverage = runs.empty() ? std::vector<codec::CoverageRun>{{0, slots, codec::Status::Complete}} : runs;
        fixed(coverage.size(), 1);
        for (const auto& r : coverage) { fixed(r.fromSlot, 2); fixed(r.toSlot, 2); fixed(uint8_t(r.status), 1); }
        var(n); var(gap);
        if (grids > 1) fixed(gridIndex, 1);
        if (openBytes.empty()) var(open); else out.insert(out.end(), openBytes.begin(), openBytes.end());
        for (auto v : {up, down, close, base, quote, buy, sell, count}) var(v);
        if (summary) return out;
        var(rowCount);
        for (const auto* field : {&offsets, &bids, &asks, &bidCounts, &askCounts}) for (auto v : *field) var(v);
        return out;
    }
};
void refused(const Payload& p) { EXPECT_ANY_THROW(codec::decode(p.bytes(), Minute, T0, 60)); }
}
TEST(FlowChunkDecode, RawFixtureIsValid) { EXPECT_NO_THROW(codec::decode(Payload{}.bytes(), Minute, T0, 60)); }
TEST(FlowChunkDecode, VarintTenthByteAboveOne) { Payload p; p.openBytes.assign(10, 128); p.openBytes.back() = 2; refused(p); }
TEST(FlowChunkDecode, VarintLongerThanTenBytes) { Payload p; p.openBytes.assign(11, 128); p.openBytes.back() = 1; refused(p); }
TEST(FlowChunkDecode, NoncanonicalVarintZero) { Payload p; p.openBytes = {128, 0}; refused(p); }
TEST(FlowChunkDecode, NoncanonicalVarintNonzero) { Payload p; p.openBytes = {200, 0}; refused(p); }
TEST(FlowChunkDecode, TruncatedVarint) {
    auto bytes = Payload{}.bytes(); bytes.back() = 128;
    EXPECT_ANY_THROW(codec::decode(bytes, Minute, T0, 60));
}
TEST(FlowChunkDecode, StepZero) { EXPECT_ANY_THROW(codec::decode(Payload{}.bytes(), Minute, T0, 60, false, 0)); }
TEST(FlowChunkDecode, StepNotPowerOfTwo) { EXPECT_ANY_THROW(codec::decode(Payload{}.bytes(), Minute, T0, 60, false, 3)); }
TEST(FlowChunkDecode, StepAboveMaximum) { EXPECT_ANY_THROW(codec::decode(Payload{}.bytes(), Minute, T0, 60, false, uint64_t(1) << 49)); }
TEST(FlowChunkDecode, CoverageHole) { Payload p; p.runs = {{1, 60, codec::Status::Complete}}; refused(p); }
TEST(FlowChunkDecode, CoverageOverlap) { Payload p; p.runs = {{0, 30, codec::Status::Complete}, {29, 60, codec::Status::Partial}}; refused(p); }
TEST(FlowChunkDecode, CoverageIncomplete) { Payload p; p.runs = {{0, 59, codec::Status::Complete}}; refused(p); }
TEST(FlowChunkDecode, CoverageBeyondSpan) { Payload p; p.runs = {{0, 61, codec::Status::Complete}}; refused(p); }
TEST(FlowChunkDecode, CoverageEmptyRun) { Payload p; p.runs = {{0, 0, codec::Status::Complete}}; refused(p); }
TEST(FlowChunkDecode, CoverageUnknownStatus) { Payload p; p.runs = {{0, 60, codec::Status(9)}}; refused(p); }
TEST(FlowChunkDecode, BarCountAboveSlots) { Payload p; p.n = 61; refused(p); }
// Ascending slots/offsets are implicit in gap+1; negative gaps would wrap to
// huge unsigned values and must be rejected before the addition can overflow.
TEST(FlowChunkDecode, NonascendingSlotGap) { Payload p; p.gap = UINT64_MAX; refused(p); }
TEST(FlowChunkDecode, SlotOutsideSpan) { Payload p; p.gap = 60; refused(p); }
TEST(FlowChunkDecode, MissingGrid) { Payload p; p.grids = 0; refused(p); }
TEST(FlowChunkDecode, InvalidGrid) { Payload p; p.tick = 0; refused(p); }
TEST(FlowChunkDecode, TooManyGridsForHour) { Payload p; p.grids = 2; refused(p); }
TEST(FlowChunkDecode, GridIndexOutOfRange) {
    Payload p; p.grids = 2; p.gridIndex = 2; p.slots = 16;
    EXPECT_ANY_THROW(codec::decode(p.bytes(), Day, T0, 16));
}
TEST(FlowChunkDecode, PriceDeltaOutsideI64) { Payload p; p.up = uint64_t(INT64_MAX) + 1; refused(p); }
TEST(FlowChunkDecode, HighPriceOverflow) { Payload p; p.up = INT64_MAX; refused(p); }
TEST(FlowChunkDecode, LowPriceOverflow) { Payload p; p.open = UINT64_MAX; p.down = 1; refused(p); }
TEST(FlowChunkDecode, ClosePriceOverflow) { Payload p; p.close = INT64_MAX; refused(p); }
TEST(FlowChunkDecode, OpenDeltaOverflowFromPreviousClose) {
    auto c = codec::fromBars(hourBars(), Minute, T0, 60);
    c.open[0] = c.low[0] = c.high[0] = c.close[0] = INT64_MAX;
    auto bytes = codec::encode(c, true);
    // Locate second open delta after header (39), n (1), and four slot gaps.
    size_t at = 44;
    while (bytes[at++] & 128) {}
    size_t end = at;
    while (bytes[end++] & 128) {}
    bytes.erase(bytes.begin() + at, bytes.begin() + end);
    bytes.insert(bytes.begin() + at, 2); // +1 on INT64_MAX
    EXPECT_ANY_THROW(codec::decode(bytes, Minute, T0, 60, true));
}
TEST(FlowChunkDecode, NonpositiveLow) { Payload p; p.down = 100; refused(p); }
TEST(FlowChunkDecode, CloseAboveHigh) { Payload p; p.close = 3; refused(p); }
TEST(FlowChunkDecode, SideVolumesAboveBase) { Payload p; p.base = 2; refused(p); }
TEST(FlowChunkDecode, VolumeOutsideI64) { Payload p; p.quote = uint64_t(INT64_MAX) + 1; refused(p); }
TEST(FlowChunkDecode, CountOutsideU32) { Payload p; p.count = uint64_t(UINT32_MAX) + 1; refused(p); }
TEST(FlowChunkDecode, BarWithoutRows) { Payload p; p.rowCount = 0; refused(p); }
TEST(FlowChunkDecode, TooManyRows) { Payload p; p.rowCount = 65536; refused(p); }
TEST(FlowChunkDecode, BidSumDisagreesWithSell) { Payload p; p.bids[0] = 0; refused(p); }
TEST(FlowChunkDecode, AskSumDisagreesWithBuy) { Payload p; p.asks[1] = 1; refused(p); }
TEST(FlowChunkDecode, BidSumOverflow) { Payload p; p.bids = {INT64_MAX, 1}; refused(p); }
TEST(FlowChunkDecode, KnownCountsAboveCount) { Payload p; p.bidCounts[0] = 2; refused(p); }
TEST(FlowChunkDecode, RowCountOutsideU32) { Payload p; p.bidCounts[0] = uint64_t(UINT32_MAX) + 1; refused(p); }
TEST(FlowChunkDecode, NonascendingOffsetGap) { Payload p; p.offsets[1] = UINT64_MAX; refused(p); }
TEST(FlowChunkDecode, OffsetAbove65534) { Payload p; p.offsets[1] = 65534; refused(p); }
TEST(FlowChunkDecode, FirstOffsetNotZero) { Payload p; p.offsets[0] = 1; refused(p); }
TEST(FlowChunkDecode, LastOffsetNotHighRow) { Payload p; p.offsets[1] = 0; refused(p); }
TEST(FlowChunkDecode, TrailingBytes) {
    auto bytes = Payload{}.bytes(); bytes.push_back(0);
    EXPECT_ANY_THROW(codec::decode(bytes, Minute, T0, 60));
}
TEST(FlowChunkDecode, EveryTruncationIsRefused) {
    const auto bytes = Payload{}.bytes();
    for (size_t n = 0; n < bytes.size(); ++n)
        EXPECT_ANY_THROW(codec::decode(std::span(bytes).first(n), Minute, T0, 60)) << n;
}
TEST(FlowChunkDecode, AllFieldsShareOneAllocation) {
    auto c = codec::decode(Payload{}.bytes(), {{}, "flow.rows", Minute, 1, T0}, 60, 42);
    EXPECT_EQ(c.epoch, 42u); EXPECT_EQ(c.bars, 1); EXPECT_EQ(c.rows, 2u);
    const auto check = [&](auto span) {
        if (span.empty()) return;
        const auto begin = reinterpret_cast<uintptr_t>(span.data());
        const auto base = reinterpret_cast<uintptr_t>(c.storage.get());
        EXPECT_GE(begin, base); EXPECT_LE(begin + span.size_bytes(), base + c.storageBytes);
        EXPECT_EQ(begin % alignof(typename decltype(span)::value_type), 0u);
    };
    check(c.grids); check(c.coverage); check(c.open); check(c.high); check(c.low); check(c.close);
    check(c.baseVol); check(c.quoteVol); check(c.buyVol); check(c.sellVol); check(c.count); check(c.rowBegin);
    check(c.slot); check(c.grid); check(c.row); check(c.offset);
    const auto* address = c.row.data();
    auto moved = std::move(c);
    EXPECT_EQ(moved.row.data(), address); EXPECT_EQ(moved.row[1].askVol, 2);
}
TEST(FlowChunkDecode, SummaryHasNoRowArrays) {
    const auto c = codec::decode(Payload{}.bytes(true), Minute, T0, 60, true);
    EXPECT_TRUE(c.rowBegin.empty()); EXPECT_TRUE(c.row.empty()); EXPECT_TRUE(c.offset.empty());
    EXPECT_TRUE(c.grid.empty()); EXPECT_EQ(c.key.priceStep, 0u);
}
TEST(FlowChunkTests, EncoderReportsSummaryBoundary) {
    const auto c = codec::fromBars(hourBars(), Minute, T0, 60);
    uint32_t length = 0;
    const auto full = codec::encode(c, false, &length);
    const auto summary = codec::encode(c, true);
    EXPECT_EQ(length, summary.size()); EXPECT_TRUE(std::equal(summary.begin(), summary.end(), full.begin()));
}
TEST(FlowChunkTests, ExplicitRecorderGridRebinsStoredSteps) {
    FlowBarBuilder fine(Minute, 1, 1, 1, 1), coarse(Minute, 4, 1, 1, 1);
    ASSERT_EQ(fine.add({"a", T0, 101, 1, Side::Buy}), FlowBarBuilder::AddResult::Accepted);
    ASSERT_EQ(coarse.add({"b", T0 + Minute, 107, 2, Side::Sell}), FlowBarBuilder::AddResult::Accepted);
    const std::vector<FlowBar> bars{fine.bars()[0], coarse.bars()[0]};
    const std::array<codec::FlowGrid, 2> grids{{{1, 1, 1, 1}, {1, 1, 1, 1}}};
    const std::array<codec::CoverageRun, 2> coverage{{{0, 2, codec::Status::Partial}, {2, 60, codec::Status::Unrecorded}}};
    const auto c = codec::fromBars(bars, Minute, T0, 60, nullptr, grids, coverage);
    EXPECT_EQ(c.priceStep, 4u); EXPECT_EQ(c.grids.size(), 1u);
    const auto result = codec::decode(codec::encode(c), Minute, T0, 60, false, c.priceStep);
    EXPECT_EQ(codec::toData(result), c);
    const auto back = codec::toBars(result);
    EXPECT_EQ(back[0].nativeTick, 4); EXPECT_EQ(back[0].lowRow, 25);
    EXPECT_EQ(back[1].nativeTick, 4); EXPECT_EQ(back[1].lowRow, 26);
}
TEST(FlowEpoch, FileRoundTrip) {
    const codec::EpochState state{123, 456, {T0 - Day, T0}};
    EXPECT_EQ(codec::parseEpochFile(codec::encodeEpochFile(state)), state);
}
TEST(FlowEpoch, FileCrcRefusal) {
    const auto good = codec::encodeEpochFile({123, 456, {T0}});
    for (size_t i = 0; i < good.size(); ++i) {
        auto bad = good; bad[i] ^= 1;
        EXPECT_ANY_THROW(codec::parseEpochFile(bad)) << i;
    }
}
TEST(FlowEpoch, MissingOrUnreadableFileFencesEveryDay) {
    const std::vector<int64_t> days{T0 - Day, T0};
    const auto recovered = codec::startupTransition(std::nullopt, 789, days, 1000000);
    EXPECT_EQ(recovered, (codec::EpochState{1000000, 789, days}));
    auto day = sampleDayFile(); day.ruleIdentity = 789;
    EXPECT_FALSE(codec::servable(recovered, day));
    std::optional<codec::EpochState> unreadable;
    try { unreadable = codec::parseEpochFile({}); } catch (const std::exception&) {}
    EXPECT_EQ(codec::startupTransition(unreadable, 789, days, 1000000), recovered);
}
TEST(FlowEpoch, EveryRuleValueChangesIdentity) {
    const codec::RuleValues original;
    const auto identity = codec::ruleIdentity(original);
    auto r = original; ++r.builderVersion; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.sealWindowMs; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.scaleVersion; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.bigFloorQuote; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.summaryVersion; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.rowsVersion; EXPECT_NE(codec::ruleIdentity(r), identity);
    r = original; ++r.bigVersion; EXPECT_NE(codec::ruleIdentity(r), identity);
}

namespace {
void putFixed(std::vector<uint8_t>& bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes.at(at + i) = uint8_t(value >> (8 * i));
}
uint32_t referenceCrc(std::span<const uint8_t> bytes) {
    uint32_t crc = UINT32_MAX;
    for (auto byte : bytes) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0);
    }
    return ~crc;
}
void repairDayCrcs(std::vector<uint8_t>& bytes) {
    const auto header = tableStart(bytes);
    putFixed(bytes, header - 4, referenceCrc(std::span(bytes).subspan(header, codec::DaySections * codec::TableEntryBytes)), 4);
    putFixed(bytes, 8, 0, 4);
    putFixed(bytes, 8, referenceCrc(std::span(bytes).first(header)), 4);
}
void badEntry(size_t entry, size_t field, uint64_t value, size_t width) {
    auto bytes = codec::encodeDayFile(sampleDayFile());
    putFixed(bytes, tableStart(bytes) + entry * codec::TableEntryBytes + field, value, width);
    repairDayCrcs(bytes);
    EXPECT_ANY_THROW(codec::parseDayFile(bytes));
}
}
TEST(FlowDayFile, StateOutsideZeroOrOne) { badEntry(0, 16, 2, 1); }
TEST(FlowDayFile, AbsentWithPayload) { badEntry(0, 16, 0, 1); }
TEST(FlowDayFile, EmptyOffsetMustBeZero) { badEntry(1, 0, 1, 4); }
TEST(FlowDayFile, SectionsMustBeBackToBack) { badEntry(0, 0, 0, 4); }
TEST(FlowDayFile, SectionLengthOutsideFile) { badEntry(0, 8, UINT32_MAX, 4); }
TEST(FlowDayFile, SectionCrc) { badEntry(0, 12, 0, 4); }
TEST(FlowDayFile, SummaryLengthAboveTotal) { badEntry(0, 4, UINT32_MAX, 4); }
TEST(FlowDayFile, AbsentSummaryMustBeZero) { badEntry(1, 4, 1, 4); }
TEST(FlowDayFile, AbsentStepMustBeZero) { badEntry(1, 25, 1, 1); }
TEST(FlowDayFile, BigTradeEntriesRemainAbsent) {
    auto day = sampleDayFile();
    const auto payload = day.sections[0].payload;
    day.sections = {};
    auto bytes = codec::encodeDayFile(day);
    const auto entry = tableStart(bytes) + 26 * codec::TableEntryBytes;
    putFixed(bytes, entry, bytes.size(), 4);
    putFixed(bytes, entry + 8, payload.size(), 4);
    putFixed(bytes, entry + 12, referenceCrc(payload), 4);
    putFixed(bytes, entry + 16, 1, 1);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    repairDayCrcs(bytes);
    EXPECT_ANY_THROW(codec::parseDayFile(bytes));
    day.sections[26] = sampleDayFile().sections[0];
    EXPECT_ANY_THROW(codec::encodeDayFile(day));
}
TEST(FlowDayFile, InvalidPayloadWithValidSectionCrcIsRefused) {
    auto d = sampleDayFile();
    d.sections[0].payload.back() = 0x80;
    EXPECT_ANY_THROW(codec::parseDayFile(codec::encodeDayFile(d)));
}
TEST(FlowDayFile, TruncatedHeaderAndTable) {
    const auto bytes = codec::encodeDayFile(sampleDayFile());
    const auto through = tableStart(bytes) + codec::DaySections * codec::TableEntryBytes;
    for (size_t length = 0; length < through; ++length)
        EXPECT_ANY_THROW(codec::parseDayFile(std::span(bytes).first(length))) << length;
}
TEST(FlowEpoch, FileLengthAndPendingOrdering) {
    auto bytes = codec::encodeEpochFile({123, 456, {T0 - Day, T0}});
    auto trailing = bytes; trailing.push_back(0);
    EXPECT_ANY_THROW(codec::parseEpochFile(trailing));
    for (size_t n = 0; n < bytes.size(); ++n)
        EXPECT_ANY_THROW(codec::parseEpochFile(std::span(bytes).first(n))) << n;
    putFixed(bytes, 28, uint64_t(T0 - Day), 8); // duplicate pending day
    putFixed(bytes, bytes.size() - 4, referenceCrc(std::span(bytes).first(bytes.size() - 4)), 4);
    EXPECT_ANY_THROW(codec::parseEpochFile(bytes));
    EXPECT_ANY_THROW(codec::encodeEpochFile({123, 456, {T0 + 1}}));
}
TEST(FlowEpoch, ExhaustedCounterCannotWrapToAnOldEpoch) {
    EXPECT_ANY_THROW(codec::startupTransition(codec::EpochState{UINT64_MAX, 1, {}}, 2, {T0}));
    EXPECT_ANY_THROW(codec::startupTransition(codec::EpochState{UINT64_MAX, 1, {T0}}, 1, {T0}));
}
TEST(FlowChunkTests, UnsealedDayPreventsEncodingAssembly) {
    std::vector<std::vector<uint8_t>> pieces(16);
    const auto chunk = codec::assembleDayChunk(T0, pieces, codec::Status::Building);
    EXPECT_ANY_THROW(codec::encode(chunk));
}

TEST(FlowEpoch, CanonicalIdentityGoldenValues) {
    EXPECT_EQ(codec::ruleIdentity({}), 0x04d7071dacc696c0ULL);
    EXPECT_EQ(codec::epochOf("flow:coinbase:BTC-USD", {123, 456, {}}), 0xfc34a40d870ac149ULL);
    const auto bytes = codec::encodeEpochFile({123, 456, {}});
    EXPECT_EQ(bytes.size(), 24u);
    EXPECT_EQ(bytes[0], 123); EXPECT_EQ(bytes[8], 200); EXPECT_EQ(bytes[9], 1);
    EXPECT_EQ(referenceCrc(std::span(bytes).first(20)),
              uint32_t(bytes[20]) | uint32_t(bytes[21]) << 8 | uint32_t(bytes[22]) << 16 | uint32_t(bytes[23]) << 24);
}
TEST(FlowChunkDecode, MultiGridIndexSpanSharesStorage) {
    const std::array<FlowBar, 2> bars{dayBar(T0, 1, 91000.12), dayBar(T0 + Day, 5, 91000.12)};
    const auto c = codec::decode(codec::encode(codec::fromBars(bars, Day, T0, 16)), Day, T0, 16);
    ASSERT_EQ(c.grid.size(), 2u); EXPECT_EQ(c.grid[0], 0); EXPECT_EQ(c.grid[1], 1);
    const auto begin = reinterpret_cast<uintptr_t>(c.storage.get());
    const auto address = reinterpret_cast<uintptr_t>(c.grid.data());
    EXPECT_GE(address, begin); EXPECT_LE(address + c.grid.size_bytes(), begin + c.storageBytes);
}
TEST(FlowChunkTests, MaximumStepRoundTrips) {
    auto c = codec::fromBars(hourBars(), Minute, T0, 60);
    c = codec::rebin(c, uint64_t(1) << 48);
    EXPECT_EQ(decodeSpec(codec::encode(c), Minute, T0, 60, false, uint64_t(1) << 48), c);
}
TEST(FlowDayFile, NonHourFingerprintMustBeZero) { badEntry(24, 17, 1, 8); }
