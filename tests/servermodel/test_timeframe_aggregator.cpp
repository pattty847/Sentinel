#include <gtest/gtest.h>
#include "servermodel/TimeframeAggregator.hpp"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <QtGlobal>
#include <chrono>
#include <cstdio>
#include <tuple>

namespace {
Trade tradeAt(int64_t timestampMs, double price, double size) {
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
    trade.price = price;
    trade.size = size;
    return trade;
}

struct Emitted {
    bool closed;
    int64_t tf;
    OHLCVBar bar;
};

bool sameBar(const OHLCVBar& a, const OHLCVBar& b) {
    return std::tie(a.timestamp_ms, a.open, a.high, a.low, a.close, a.volume, a.count, a.is_closed) ==
           std::tie(b.timestamp_ms, b.open, b.high, b.low, b.close, b.volume, b.count, b.is_closed);
}

// Every barUpdated/barClosed emission for the given timeframes, in order.
struct Recorder {
    std::vector<Emitted> events;
    Recorder(TimeframeAggregator& aggregator, std::vector<int64_t> tfs) {
        const auto keep = [this, tfs](bool closed, int64_t tf, const OHLCVBar& bar) {
            if (std::find(tfs.begin(), tfs.end(), tf) != tfs.end()) events.push_back({closed, tf, bar});
        };
        QObject::connect(&aggregator, &TimeframeAggregator::barClosed,
                         [keep](const QString&, int64_t tf, const OHLCVBar& bar) { keep(true, tf, bar); });
        QObject::connect(&aggregator, &TimeframeAggregator::barUpdated,
                         [keep](const QString&, int64_t tf, const OHLCVBar& bar) { keep(false, tf, bar); });
    }
};

void expectSameHistory(const std::vector<OHLCVBar>& a, const std::vector<OHLCVBar>& b, int64_t tf) {
    ASSERT_EQ(a.size(), b.size()) << tf;
    for (size_t i = 0; i < a.size(); ++i) EXPECT_TRUE(sameBar(a[i], b[i])) << tf << " bar " << i;
}

uint64_t lateDropsOf(const TimeframeAggregator& aggregator, const std::string& symbol, int64_t tf) {
    for (const auto& drops : aggregator.lateDrops())
        if (drops.symbol == symbol && drops.timeframeMs == tf) return drops.count;
    ADD_FAILURE() << "no late-drop series for " << symbol << " " << tf;
    return 0;
}

QStringList* g_warnings = nullptr;
struct WarningCapture {
    QStringList lines;
    QtMessageHandler previous;
    WarningCapture() {
        g_warnings = &lines;
        previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& text) {
            if (type == QtWarningMsg && g_warnings) g_warnings->push_back(text);
        });
    }
    ~WarningCapture() {
        qInstallMessageHandler(previous);
        g_warnings = nullptr;
    }
    bool contains(const QString& text) const {
        return std::any_of(lines.begin(), lines.end(), [&](const QString& l) { return l.contains(text); });
    }
};
} // namespace

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

// One journal-clocked input: trades at 100 ms offsets (some quiet buckets,
// one late trade) with watermark advances in between.
void feedJournal(TimeframeAggregator& aggregator) {
    aggregator.setJournalClock("BTC-USD");
    constexpr int64_t t0 = 1'700'000'000'000;
    int64_t t = t0;
    double price = 100;
    for (int i = 0; i < 2'000; ++i) {
        t += (i % 7 == 3) ? 700 : 100; // quiet stretches carry
        price += (i % 5) - 2;
        aggregator.onTrade(tradeAt(t + (i % 3) * 10, price, 0.5 + (i % 4)));
        if (i % 11 == 0) aggregator.onTrade(tradeAt(t - 150, price - 1, 1)); // late for 100 ms
        if (i % 13 == 0) aggregator.advance("BTC-USD", t + 250);
    }
    aggregator.advance("BTC-USD", t + 200'000);
}

TEST(TimeframeAggregatorHundredMs, BarsAndUnchangedSecondAndMinute) {
    TimeframeAggregator with({100, 1'000, 60'000});
    TimeframeAggregator without({1'000, 60'000});
    Recorder withEvents(with, {1'000, 60'000});
    Recorder withoutEvents(without, {1'000, 60'000});
    feedJournal(with);
    feedJournal(without);
    for (const int64_t tf : {int64_t(1'000), int64_t(60'000)})
        expectSameHistory(with.getHistory("BTC-USD", tf, 100'000), without.getHistory("BTC-USD", tf, 100'000), tf);
    ASSERT_EQ(withEvents.events.size(), withoutEvents.events.size());
    for (size_t i = 0; i < withEvents.events.size(); ++i) {
        const auto& a = withEvents.events[i];
        const auto& b = withoutEvents.events[i];
        EXPECT_TRUE(a.closed == b.closed && a.tf == b.tf && sameBar(a.bar, b.bar)) << "event " << i;
    }
    EXPECT_GT(with.getHistory("BTC-USD", 1'000, 100'000).size(), 100u);
    EXPECT_TRUE(without.getHistory("BTC-USD", 100, 100'000).empty());

    // Exact 100 ms OHLCV from a small sequence.
    TimeframeAggregator small({100, 1'000});
    small.setJournalClock("BTC-USD");
    small.onTrade(tradeAt(1'000'010, 10, 1));
    small.onTrade(tradeAt(1'000'050, 12, 2));
    small.onTrade(tradeAt(1'000'090, 9, 3));
    small.onTrade(tradeAt(1'000'120, 11, 4)); // closes [1'000'000, 1'000'100)
    small.advance("BTC-USD", 1'000'300);       // closes 1'000'100 and carries 1'000'200
    const auto bars = small.getHistory("BTC-USD", 100, 100);
    ASSERT_EQ(bars.size(), 3u);
    EXPECT_TRUE(sameBar(bars[0], {1'000'000, 10, 12, 9, 9, 6, 3, true}));
    EXPECT_TRUE(sameBar(bars[1], {1'000'100, 11, 11, 11, 11, 4, 1, true}));
    EXPECT_TRUE(sameBar(bars[2], {1'000'200, 11, 11, 11, 11, 0, 0, true}));
    const auto second = small.getHistory("BTC-USD", 1'000, 100);
    EXPECT_TRUE(second.empty()); // 1 s bucket 1'000'000 is still open
}

TEST(TimeframeAggregatorHundredMs, OnlyHundredIsAcceptedBelowOneSecond) {
    for (const int64_t rejected : {int64_t(250), int64_t(500)}) {
        WarningCapture warnings;
        TimeframeAggregator aggregator({rejected, 1'000});
        EXPECT_TRUE(warnings.contains(QStringLiteral("unsupported timeframe_ms=%1").arg(rejected))) << rejected;
    }
    WarningCapture warnings;
    TimeframeAggregator aggregator({100, 1'000});
    EXPECT_FALSE(warnings.contains(QStringLiteral("unsupported timeframe_ms"))) << warnings.lines.join('\n').toStdString();
    aggregator.onTrade(tradeAt(1'000'010, 10, 1));
    aggregator.onTrade(tradeAt(1'000'110, 10, 1));
    EXPECT_EQ(aggregator.getHistory("BTC-USD", 100).size(), 1u);
}

TEST(TimeframeAggregatorHundredMs, PerTimeframeRetention) {
    std::printf("sizeof(OHLCVBar)=%zu\n", sizeof(OHLCVBar));
    constexpr int64_t t0 = 1'700'000'000'000;
    {
        TimeframeAggregator aggregator({100, 1'000, 60'000}, {{100, 18'000}});
        // Each trade closes the previous 100 ms bar: trade k closes bar k (1-based).
        aggregator.onTrade(tradeAt(t0, 1, 1));
        for (int k = 1; k <= 19'800; ++k) aggregator.onTrade(tradeAt(t0 + 100 * k, 1 + k, 1));
        EXPECT_EQ(aggregator.getHistory("BTC-USD", 100, 1'000'000).size(), 19'800u);
        aggregator.onTrade(tradeAt(t0 + 100 * 19'801, 1, 1));
        const auto kept = aggregator.getHistory("BTC-USD", 100, 1'000'000);
        ASSERT_EQ(kept.size(), 18'000u);
        EXPECT_EQ(kept.front().timestamp_ms, t0 + 100 * 1'801); // bar 1,802; 1,801 erased
        EXPECT_EQ(kept.back().timestamp_ms, t0 + 100 * 19'800);
    }
    {
        // One trade per minute: every trade closes one bar of each timeframe.
        TimeframeAggregator aggregator({100, 1'000, 60'000}, {{100, 18'000}});
        for (int k = 0; k <= 10'001; ++k) aggregator.onTrade(tradeAt(t0 + 60'000 * k, 1, 1));
        EXPECT_EQ(aggregator.getHistory("BTC-USD", 100, 1'000'000).size(), 10'001u);
        EXPECT_EQ(aggregator.getHistory("BTC-USD", 1'000, 1'000'000).size(), 9'001u);
        EXPECT_EQ(aggregator.getHistory("BTC-USD", 60'000, 1'000'000).size(), 9'001u);
    }
}

TEST(TimeframeAggregatorHundredMs, CarriesAtMostThreeHundredThenJumps) {
    constexpr int64_t t0 = 1'000'000;
    TimeframeAggregator aggregator({100, 1'000});
    aggregator.setJournalClock("BTC-USD");
    aggregator.onTrade(tradeAt(t0 + 10, 5, 1));
    aggregator.advance("BTC-USD", t0 + 100 * 500);
    auto bars = aggregator.getHistory("BTC-USD", 100, 1'000);
    ASSERT_EQ(bars.size(), 301u); // the traded bar + 300 carried
    for (size_t i = 0; i < bars.size(); ++i) EXPECT_EQ(bars[i].timestamp_ms, t0 + 100 * int64_t(i));
    EXPECT_DOUBLE_EQ(bars.back().close, 5);
    EXPECT_EQ(bars.back().count, 0u);
    aggregator.advance("BTC-USD", t0 + 100 * 501);
    bars = aggregator.getHistory("BTC-USD", 100, 1'000);
    ASSERT_EQ(bars.size(), 302u);
    EXPECT_EQ(bars.back().timestamp_ms, t0 + 100 * 500); // jumped: 301..499 never exist
}

TEST(TimeframeAggregatorHundredMs, LateTradesCountedPerTimeframe) {
    TimeframeAggregator aggregator({100, 1'000, 60'000});
    aggregator.setJournalClock("BTC-USD");
    aggregator.onTrade(tradeAt(1'001'250, 10, 1));
    aggregator.onTrade(tradeAt(1'001'150, 11, 1)); // older than open 100 ms bucket, same 1 s bucket
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 100), 1u);
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 1'000), 0u);
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 60'000), 0u);
    aggregator.advance("BTC-USD", 1'001'500); // watermark closes 1'001'200..1'001'400
    aggregator.onTrade(tradeAt(1'001'350, 12, 1)); // below a watermark-closed 100 ms bucket
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 100), 2u);
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 1'000), 0u);
    const auto second = aggregator.getHistory("BTC-USD", 1'000, 100);
    EXPECT_TRUE(second.empty());
    aggregator.advance("BTC-USD", 1'002'000);
    const auto closedSecond = aggregator.getHistory("BTC-USD", 1'000, 100);
    ASSERT_EQ(closedSecond.size(), 1u);
    EXPECT_EQ(closedSecond[0].count, 3u); // all three trades reached the 1 s bar
}

TEST(TimeframeAggregatorHundredMs, HistoryPageEndsAtOrBeforeEnd) {
    constexpr int64_t t0 = 1'700'000'000'000;
    TimeframeAggregator aggregator({100, 1'000});
    for (int k = 0; k <= 1'000; ++k) aggregator.onTrade(tradeAt(t0 + 100 * k, 1, 1)); // 1,000 closed
    const int64_t endMs = t0 + 100 * 700;
    auto page = aggregator.getHistory("BTC-USD", 100, endMs, 350);
    ASSERT_EQ(page.size(), 350u);
    EXPECT_EQ(page.back().timestamp_ms, endMs); // inclusive
    EXPECT_EQ(page.front().timestamp_ms, endMs - 100 * 349);
    page = aggregator.getHistory("BTC-USD", 100, endMs + 50, 350);
    ASSERT_EQ(page.size(), 350u);
    EXPECT_EQ(page.back().timestamp_ms, endMs);
    page = aggregator.getHistory("BTC-USD", 100, t0 + 100 * 99, 350);
    ASSERT_EQ(page.size(), 100u);
    EXPECT_EQ(page.front().timestamp_ms, t0);
    EXPECT_TRUE(aggregator.getHistory("BTC-USD", 100, t0 - 1, 350).empty());
}
