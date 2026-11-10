#include "chart/feeds/TradeBubbleFeed.hpp"
#include <gtest/gtest.h>
#include <chrono>

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
