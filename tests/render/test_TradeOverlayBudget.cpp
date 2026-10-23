#include <gtest/gtest.h>
#include "servermodel/TradeOverlayPublisher.hpp"
#include <algorithm>

// Pure worker tests: no Qt application, event loop, GUI or network is required.
namespace {
using namespace trade_overlay;
using OverlayTrade = ServerDataModel::FootprintTradeSample;
constexpr int64_t day = 172800000, hour = 3600000;
Request historyRequest() {
    Request q;
    q.symbol = "BTC-USD"; q.kind = Kind::TpoHistory; q.requestId = "tpo-1-0";
    q.grid = {16, 10, 2, 120}; q.footprintMs = hour; q.tpoMs = hour;
    q.nowMs = day + 2 * hour; q.count = 2;
    return q;
}
}

TEST(TradeOverlayBudget, OversizedUnorderedTapeProducesCompletePage) {
    const auto q = historyRequest();
    const auto window = tradeWindow(q);
    std::vector<OverlayTrade> selected;
    OverlayTradeSelection selection(window.startMs, window.endMs, kMaxTrades,
                                    selected, OverlayTradeBudget::Newest);
    // Newest trades arrive first. A retained tape tail is not a timestamp tail.
    for (size_t i = kMaxTrades; i > 0; --i)
        ASSERT_TRUE(selection.add({day + hour + static_cast<int64_t>(i), 111, 1, AggressorSide::Buy}));
    ASSERT_TRUE(selection.add({day + 2000, 117, 1, AggressorSide::Buy}));
    ASSERT_TRUE(selection.add({day + 1000, 113, 1, AggressorSide::Sell}));
    ASSERT_TRUE(selection.add({window.endMs, 119, 1, AggressorSide::Buy}));
    ASSERT_TRUE(selection.add({day - 1, 119, 1, AggressorSide::Buy}));
    selection.finish();
    ASSERT_TRUE(selection.budgetExceeded());
    ASSERT_EQ(selected.size(), kMaxTrades);
    EXPECT_EQ(selected.front().timestampMs, day + hour + 1);
    EXPECT_EQ(selected.back().timestampMs, day + hour + kMaxTrades);
    EXPECT_EQ(selection.retainedFromMs(), day + 2001);
    int calls = 0;
    const auto candles = fetchTpoCandles(q, selection.retainedFromMs(),
        [&](int64_t start, int64_t end, int64_t granularity, int limit) {
            ++calls;
            EXPECT_EQ(start * 1000, day); EXPECT_EQ(end * 1000, day + hour);
            EXPECT_EQ(granularity, 3600); EXPECT_EQ(limit, 1);
            CandleFetchResult result; result.ok = true;
            OHLCVBar bar; bar.timestamp_ms = day; bar.high = 117; bar.low = 113; bar.close = 115;
            result.candles.push_back(bar);
            return result;
        });
    ASSERT_TRUE(candles.ok); EXPECT_EQ(calls, 1);
    const auto page = build(q, selected, candles.candles, selection.retainedFromMs());
    ASSERT_TRUE(page.error.empty()) << page.error;
    ASSERT_EQ(page.messages.size(), 1);
    const auto wire = nlohmann::json::parse(page.messages.front());
    EXPECT_EQ(wire["type"], "tpo_history_chunk"); EXPECT_EQ(wire["request_id"], q.requestId);
    ASSERT_EQ(wire["columns"].size(), 2);
    EXPECT_EQ(wire["columns"][0]["letters"], "AEFBQQAAAAAAAA=="); // rows 1..3: A
    EXPECT_EQ(wire["columns"][1]["letters"], "AAAAAEIAAAAAAA=="); // row 4: B
    EXPECT_EQ(wire["columns"][0]["time_start"], day);
    EXPECT_EQ(wire["columns"][1]["time_end"], day + 2 * hour);
}

TEST(TradeOverlayBudget, DroppedTimestampTieAtCandleBoundaryIsBackfilled) {
    const auto q = historyRequest();
    std::vector<OverlayTrade> selected;
    OverlayTradeSelection selection(day, day + 2 * hour, kMaxTrades,
                                    selected, OverlayTradeBudget::Newest);
    for (size_t i = 0; i < kMaxTrades; ++i)
        ASSERT_TRUE(selection.add({day + hour, 111, 1, AggressorSide::Buy}));
    ASSERT_TRUE(selection.add({day + hour, 117, 1, AggressorSide::Sell}));
    selection.finish();
    ASSERT_EQ(selected.size(), kMaxTrades);
    ASSERT_TRUE(selection.budgetExceeded());
    EXPECT_EQ(selection.retainedFromMs(), day + hour + 1);
    const auto candles = fetchTpoCandles(q, selection.retainedFromMs(),
        [&](int64_t start, int64_t end, int64_t granularity, int) {
            EXPECT_EQ(end * 1000, day + 2 * hour);
            CandleFetchResult result; result.ok = true;
            for (auto t = start * 1000; t < end * 1000; t += granularity * 1000) {
                OHLCVBar bar; bar.timestamp_ms = t; bar.high = 117; bar.low = 111; bar.close = 115;
                result.candles.push_back(bar);
            }
            return result;
        });
    ASSERT_TRUE(candles.ok);
    const auto page = build(q, selected, candles.candles, selection.retainedFromMs());
    ASSERT_TRUE(page.error.empty()) << page.error;
    const auto wire = nlohmann::json::parse(page.messages.at(0));
    EXPECT_EQ(wire["columns"][1]["letters"], "AEJCQkIAAAAAAA=="); // rows 1..4: B
}

TEST(TradeOverlayBudget, NewestSelectionPreservesSevenDayCandleInputBudget) {
    auto q = historyRequest(); q.session = SessionManager::SessionType::M1;
    q.nowMs = 1791583200000; // 2026-10-09 22:00 UTC, inside the monthly session
    q.count = 8 * 24;
    const auto window = tradeWindow(q);
    std::vector<OverlayTrade> selected;
    OverlayTradeSelection selection(window.startMs, window.endMs, kMaxTrades,
                                    selected, OverlayTradeBudget::Newest);
    for (size_t i = 0; i <= kMaxTrades; ++i)
        ASSERT_TRUE(selection.add({window.endMs - 1, 111, 1, AggressorSide::Buy}));
    selection.finish();
    ASSERT_TRUE(selection.budgetExceeded());
    const auto candles = fetchTpoCandles(q, selection.retainedFromMs(), [](auto, auto, auto, auto) {
        ADD_FAILURE() << "over-budget candle window must fail before REST";
        return CandleFetchResult{};
    });
    EXPECT_FALSE(candles.ok);
    EXPECT_EQ(candles.error, "TPO candle history exceeds input budget");
}
