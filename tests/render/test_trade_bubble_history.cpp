#include "chart/feeds/TradeBubbleFeed.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <unordered_set>

struct TradeBubbleRendererTest {
    static const trade_bubbles::Tape& tape(const chart::TradeBubbleFeed& feed) { return *feed.tape_; }
};

namespace {
Trade trade(int64_t ms, std::string id) {
    return {std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)),
            "BTC-USD", std::move(id), AggressorSide::Buy, 100, 1};
}
protocol::tradewire::Page page(int64_t from, int64_t to) {
    protocol::tradewire::Page p;
    p.symbol = "BTC-USD"; p.status = "complete";
    p.startMs = p.coveredFromMs = from;
    p.endMs = p.coveredToMs = to;
    return p;
}
}

TEST(TradeBubbleHistory, HistoryAndLiveResendShareOneTapeRow) {
    chart::TradeBubbleFeed feed;
    auto history = page(1000, 2000);
    history.rows.push_back({"42", 1500, 100, 1, AggressorSide::Buy, 1});
    EXPECT_EQ(feed.appendHistory(history), 1u);
    feed.append(trade(1500, "42"));
    ASSERT_EQ(TradeBubbleRendererTest::tape(feed).samples().size(), 1u);
    EXPECT_EQ(TradeBubbleRendererTest::tape(feed).samples()[0].tradeId, "42");
}

TEST(TradeBubbleHistory, LateHistoryStaysOrderedAcrossRingWrap) {
    chart::TradeBubbleFeed feed;
    for (size_t i = 0; i <= trade_bubbles::Tape::Capacity; ++i)
        feed.append(trade(1000 + int64_t(i) * 2, std::to_string(i)));
    auto history = page(1000, 4000);
    history.rows.push_back({"late", 1003, 100, 1, AggressorSide::Buy, 1});
    EXPECT_EQ(feed.appendHistory(history), 1u);
    const auto samples = TradeBubbleRendererTest::tape(feed).samples();
    ASSERT_EQ(samples.size(), trade_bubbles::Tape::Capacity);
    EXPECT_EQ(samples.front().tradeId, "late");
    for (size_t i = 1; i < samples.size(); ++i)
        EXPECT_LE(samples[i-1].timeMs, samples[i].timeMs);
}

namespace {
using trade_bubbles::Sample;
using trade_bubbles::Tape;
std::vector<std::pair<int64_t, std::string>> contents(const Tape& tape) {
    std::vector<std::pair<int64_t, std::string>> out;
    const auto rows = tape.samples();
    for (size_t i = 0; i < rows.size(); ++i) out.emplace_back(rows[i].timeMs, rows[i].tradeId);
    return out;
}
}

// A full 250k-row history page (kTradeHistoryMaxTrades) older than the 50k live rows on
// the tape keeps only its newest 50k rows: the tape holds 100k. Appending row by row
// shifted every live row for each page row (O(rows x tape)); the merge writes each
// retained row once, which the row-write count bounds.
TEST(TradeBubbleHistory, FullHistoryPageKeepsOnlyItsNewestTapeCapacityRowsInLinearWork) {
    constexpr size_t pageRows = protocol::SentinelProtocol::kTradeHistoryMaxTrades;
    constexpr size_t live = 50'000;
    chart::TradeBubbleFeed feed;
    for (size_t i = 0; i < live; ++i) feed.append(trade(10'000'000 + int64_t(i), "L" + std::to_string(i)));
    auto history = page(1'000'000, 1'000'000 + int64_t(pageRows));
    history.rows.reserve(pageRows);
    for (size_t i = 0; i < pageRows; ++i)
        history.rows.push_back({"H" + std::to_string(i), 1'000'000 + int64_t(i), 100, 1, AggressorSide::Sell, 1});
    const auto start = std::chrono::steady_clock::now();
    const uint64_t writesBefore = TradeBubbleRendererTest::tape(feed).rowWrites();
    EXPECT_EQ(feed.appendHistory(history), Tape::Capacity - live);
    // Linear work: each retained row is written once. Row-by-row insertion before the live
    // rows wrote ~50k rows per page row (2.5e9 writes, 24 s).
    const uint64_t writes = TradeBubbleRendererTest::tape(feed).rowWrites() - writesBefore;
    EXPECT_LE(writes, 2 * (pageRows + Tape::Capacity)) << "ingesting the page wrote " << writes << " rows";
    std::cout << "MERGE_250K_MS "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
              << '\n';
    const auto& tape = TradeBubbleRendererTest::tape(feed);
    const auto rows = tape.samples();
    ASSERT_EQ(rows.size(), Tape::Capacity);
    // The newest page rows, then every live row, in event-time order.
    EXPECT_EQ(rows.front().tradeId, "H" + std::to_string(pageRows - (Tape::Capacity - live)));
    EXPECT_EQ(rows[Tape::Capacity - live - 1].tradeId, "H" + std::to_string(pageRows - 1));
    EXPECT_EQ(rows[Tape::Capacity - live].tradeId, "L0");
    EXPECT_EQ(rows.back().tradeId, "L" + std::to_string(live - 1));
    for (size_t i = 1; i < rows.size(); ++i) ASSERT_LT(rows[i-1].timeMs, rows[i].timeMs);
    // A dropped page row's ID is not retained: the same execution arriving live is new.
    feed.append(trade(20'000'000, "H0"));
    EXPECT_EQ(tape.samples().back().tradeId, "H0");
    // Retained IDs are still duplicates.
    EXPECT_EQ(feed.appendHistory(history), 0u);
    feed.append(trade(20'000'001, "L7"));
    EXPECT_EQ(tape.samples().back().tradeId, "H0");
}

// The merge equals appending each row: same rows, same order (equal event times keep
// arrival order), same rows rejected. Rounds alternate a tape that overflows (unique page
// IDs) and one that does not (page IDs repeat and reuse retained IDs).
TEST(TradeBubbleHistory, MergeMatchesRowByRowAppend) {
    std::mt19937_64 random(7);
    for (int round = 0; round < 12; ++round) {
        const bool overflow = round % 2;
        auto byRow = std::make_unique<Tape>(), merged = std::make_unique<Tape>();
        const size_t existing = overflow ? Tape::Capacity - 1000 + random() % 3000 : random() % 20'000;
        // Live rows arrive in time order (row-by-row appends of a shuffled tape would be quadratic).
        std::vector<int64_t> times(existing);
        for (auto& t : times) t = int64_t(1 + random() % 400'000);
        std::sort(times.begin(), times.end());
        for (size_t i = 0; i < existing; ++i) {
            const Sample s{times[i], 100, 1, AggressorSide::Buy, "E" + std::to_string(overflow ? i : i % 20'000)};
            byRow->append(s); merged->append(s);
        }
        std::vector<Sample> page;
        const size_t rows = overflow ? 1 + random() % 1000 : random() % 8'000;
        for (size_t i = 0; i < rows; ++i) {
            std::string id = overflow ? "P" + std::to_string(i)
                           : i % 5 ? "P" + std::to_string(random() % 6'000)
                           : i % 2 ? "" : "E" + std::to_string(random() % 20'000);
            Sample s{int64_t(1 + random() % 400'000), 100, 1, i % 97 ? AggressorSide::Sell : AggressorSide::Unknown, id};
            s.exchangeNs = int64_t(random() % 3);
            page.push_back(s);
        }
        size_t appended = 0;
        for (const auto& s : page) appended += byRow->append(s);
        const size_t retained = merged->merge(page);
        // Row by row also counts page rows that later page rows evicted; the merge counts retained rows.
        if (!overflow) EXPECT_EQ(retained, appended) << "round " << round;
        else EXPECT_LE(retained, appended) << "round " << round;
        const auto want = contents(*byRow), got = contents(*merged);
        ASSERT_EQ(got.size(), want.size()) << "round " << round;
        for (size_t i = 0; i < want.size(); ++i)
            ASSERT_EQ(got[i], want[i]) << "round " << round << " row " << i;
        std::unordered_set<uint64_t> sequences;
        const auto m = merged->samples();
        for (size_t i = 0; i < m.size(); ++i) ASSERT_TRUE(sequences.insert(m[i].sequence).second);
    }
}

TEST(TradeBubbleHistory, PageLargerThanTheTapeKeepsItsNewestRows) {
    auto tape = std::make_unique<Tape>();
    std::vector<Sample> page;
    for (size_t i = 0; i < Tape::Capacity + 500; ++i)
        page.push_back({int64_t(1000 + (Tape::Capacity + 500 - i)), 100, 1, AggressorSide::Buy, std::to_string(i)});
    EXPECT_EQ(tape->merge(page), Tape::Capacity);
    const auto rows = tape->samples();
    EXPECT_EQ(rows.front().timeMs, 1501);
    EXPECT_EQ(rows.back().timeMs, int64_t(1000 + Tape::Capacity + 500));
    // A merged row evicted later releases its ID like an appended one.
    EXPECT_TRUE(tape->append({int64_t(1000 + Tape::Capacity + 600), 100, 1, AggressorSide::Buy, "fresh"}));
    EXPECT_TRUE(tape->append({int64_t(1000 + Tape::Capacity + 700), 100, 1, AggressorSide::Buy,
                              std::to_string(Tape::Capacity + 499)}));
}
