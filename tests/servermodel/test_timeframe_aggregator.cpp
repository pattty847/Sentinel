#include <gtest/gtest.h>
#include "servermodel/TimeframeAggregator.hpp"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <QtGlobal>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
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
    aggregator.advance("BTC-USD", 300'000);
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

// One journal input: trades at 100 ms offsets (some quiet buckets,
// one late trade) with watermark advances in between.
void feedJournal(TimeframeAggregator& aggregator) {
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
        // A timeframe without a cap keeps at most 10,000, trimmed by 1,000,
        // while another is capped. One trade per second (and per minute):
        // every trade closes exactly one bar, with no quiet bucket to carry.
        TimeframeAggregator seconds({1'000}, {{100, 18'000}});
        for (int k = 0; k <= 10'001; ++k) seconds.onTrade(tradeAt(t0 + 1'000 * k, 1, 1));
        EXPECT_EQ(seconds.getHistory("BTC-USD", 1'000, 1'000'000).size(), 9'001u);
        TimeframeAggregator minutes({60'000}, {{100, 18'000}});
        for (int k = 0; k <= 10'001; ++k) minutes.onTrade(tradeAt(t0 + 60'000 * k, 1, 1));
        EXPECT_EQ(minutes.getHistory("BTC-USD", 60'000, 1'000'000).size(), 9'001u);
    }
}

TEST(TimeframeAggregatorHundredMs, CarriesAtMostThreeHundredThenJumps) {
    constexpr int64_t t0 = 1'000'000;
    TimeframeAggregator aggregator({100, 1'000});
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

namespace {
constexpr int64_t kNoon = 1'798'761'600'000 + 12 * 3'600'000; // 2027-01-01 12:00:00 UTC
constexpr int64_t at(int minute, int second, int milli = 0) {
    return kNoon + minute * 60'000 + second * 1'000 + milli;
}
OHLCVBar bar(int64_t ts, double price, uint32_t count, bool closed) {
    OHLCVBar b;
    b.timestamp_ms = ts;
    b.open = b.high = b.low = b.close = price;
    b.volume = count;
    b.count = count;
    b.is_closed = closed;
    return b;
}
bool hasBar(const OHLCVBar& b) { return b.count != 0 || b.timestamp_ms != 0; }
} // namespace

TEST(TimeframeAggregatorRestore, InstallsClosedAndActiveWithoutEmitting) {
    TimeframeAggregator aggregator({100, 1'000, 60'000});
    Recorder recorder(aggregator, {100, 1'000, 60'000});
    TimeframeAggregator::Series series;
    series.closed = {bar(at(0, 0), 10, 1, true), bar(at(0, 1), 11, 2, true)};
    series.active = bar(at(0, 2), 12, 2, false);
    ASSERT_TRUE(aggregator.restoreSeries("BTC-USD", 1'000, series));
    EXPECT_TRUE(recorder.events.empty());
    EXPECT_FALSE(aggregator.restoreSeries("BTC-USD", 1'000, series)); // history present
    EXPECT_FALSE(aggregator.restoreSeries("BTC-USD", 300'000, series)); // not a trade timeframe
    TimeframeAggregator closedOnly({100, 1'000, 60'000});
    TimeframeAggregator::Series closedSeries;
    closedSeries.closed = series.closed;
    ASSERT_TRUE(closedOnly.restoreSeries("BTC-USD", 1'000, closedSeries));
    EXPECT_FALSE(closedOnly.restoreSeries("BTC-USD", 1'000, closedSeries)); // closed bars, no open bar
    EXPECT_TRUE(closedOnly.restoreSeries("BTC-USD", 100, closedSeries));   // other timeframes are separate
    expectSameHistory(aggregator.getHistory("BTC-USD", 1'000), series.closed, 1'000);
    // The next trade closes the restored open bar with its count.
    aggregator.onTrade(tradeAt(at(0, 3, 500), 13, 1));
    auto history = aggregator.getHistory("BTC-USD", 1'000);
    ASSERT_EQ(history.size(), 3u);
    EXPECT_TRUE(sameBar(history.back(), bar(at(0, 2), 12, 2, true)));
    EXPECT_EQ(aggregator.exportSeries("BTC-USD", 1'000).active.count, 1u);

    // A watermark after a restored carried bar continues the carry.
    TimeframeAggregator carried({100, 1'000, 60'000});
    TimeframeAggregator::Series quiet;
    quiet.active = bar(at(0, 2), 12, 0, false);
    quiet.active.volume = 0;
    ASSERT_TRUE(carried.restoreSeries("BTC-USD", 1'000, quiet));
    EXPECT_FALSE(carried.restoreSeries("BTC-USD", 1'000, {})); // open bar present
    carried.advance("BTC-USD", at(0, 5));
    history = carried.getHistory("BTC-USD", 1'000);
    ASSERT_EQ(history.size(), 3u);
    for (size_t i = 0; i < history.size(); ++i) {
        EXPECT_EQ(history[i].timestamp_ms, at(0, 2 + int(i)));
        EXPECT_DOUBLE_EQ(history[i].close, 12);
    }
    EXPECT_EQ(carried.exportSeries("BTC-USD", 1'000).active.timestamp_ms, at(0, 5));

    // Export round-trips bars and the floor.
    TimeframeAggregator source({100, 1'000, 60'000});
    source.seedJournalFloor("BTC-USD", at(0, 59), {100});
    source.onTrade(tradeAt(at(1, 0, 250), 20, 1));
    source.advance("BTC-USD", at(1, 1));
    const auto exported = source.exportSeries("BTC-USD", 100);
    ASSERT_EQ(exported.floorMs, std::optional<int64_t>(at(1, 0, 200))); // the first trade's bucket
    TimeframeAggregator target({100, 1'000, 60'000});
    ASSERT_TRUE(target.restoreSeries("BTC-USD", 100, exported));
    const auto back = target.exportSeries("BTC-USD", 100);
    expectSameHistory(back.closed, exported.closed, 100);
    EXPECT_TRUE(sameBar(back.active, exported.active));
    EXPECT_EQ(back.floorMs, exported.floorMs);
    // A floor that is on refuses a second install even with no bars.
    TimeframeAggregator floored({100, 1'000, 60'000});
    floored.seedJournalFloor("BTC-USD", at(0, 59), {1'000});
    EXPECT_FALSE(floored.restoreSeries("BTC-USD", 1'000, {}));
}

TEST(TimeframeAggregatorRestore, JournalFloorDropsStaleTradesBeforeTheFirstBar) {
    for (const bool watermarkFirst : {false, true}) {
        SCOPED_TRACE(watermarkFirst ? "advance between seed and trade" : "seed then trade");
        TimeframeAggregator aggregator({100, 1'000, 60'000});
        aggregator.seedJournalFloor("BTC-USD", at(0, 59), {100, 1'000});
        const int64_t floor = watermarkFirst ? at(1, 30) : at(0, 59);
        if (watermarkFirst) aggregator.advance("BTC-USD", at(1, 30));
        aggregator.onTrade(tradeAt(at(-2, 0), 200, 1)); // 11:58:00, delayed
        for (const int64_t tf : {100, 1'000}) {
            EXPECT_TRUE(aggregator.getHistory("BTC-USD", tf).empty()) << tf;
            const auto series = aggregator.exportSeries("BTC-USD", tf);
            EXPECT_FALSE(hasBar(series.active)) << tf;
            EXPECT_EQ(series.floorMs, std::optional<int64_t>(floor)) << tf;
            EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", tf), 1u) << tf;
        }
        // The unseeded minute accepts it as today.
        EXPECT_EQ(aggregator.exportSeries("BTC-USD", 60'000).active.count, 1u);
        EXPECT_EQ(aggregator.exportSeries("BTC-USD", 60'000).floorMs, std::nullopt);
        EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 60'000), 0u);
        // An older watermark never lowers the floor.
        aggregator.advance("BTC-USD", at(0, 30));
        EXPECT_EQ(aggregator.exportSeries("BTC-USD", 1'000).floorMs, std::optional<int64_t>(floor));
        aggregator.advance("BTC-USD", at(1, 30));
        aggregator.onTrade(tradeAt(at(1, 31), 300, 1));
        const auto second = aggregator.exportSeries("BTC-USD", 1'000);
        EXPECT_TRUE(sameBar(second.active, bar(at(1, 31), 300, 1, false)));
        EXPECT_TRUE(sameBar(aggregator.exportSeries("BTC-USD", 100).active,
                            bar(at(1, 31), 300, 1, false)));
        // With a bar present the floor is neither written nor read.
        aggregator.advance("BTC-USD", at(1, 40));
        for (const int64_t tf : {100, 1'000})
            EXPECT_EQ(aggregator.exportSeries("BTC-USD", tf).floorMs, std::optional<int64_t>(at(1, 31)))
                << tf; // raised to the first accepted trade's bucket, then frozen
        aggregator.onTrade(tradeAt(at(1, 0), 400, 1)); // dropped by the bar test
        for (const int64_t tf : {100, 1'000})
            EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", tf), 2u) << tf;
    }
}

TEST(TimeframeAggregatorRestore, FloorTravelsWithAnEmptySeries) {
    TimeframeAggregator source({100, 1'000, 60'000});
    source.seedJournalFloor("BTC-USD", at(0, 59), {100, 1'000});
    source.advance("BTC-USD", at(1, 30));
    TimeframeAggregator target({100, 1'000, 60'000});
    for (const int64_t tf : {100, 1'000}) {
        const auto series = source.exportSeries("BTC-USD", tf);
        ASSERT_FALSE(hasBar(series.active));
        ASSERT_EQ(series.floorMs, std::optional<int64_t>(at(1, 30)));
        ASSERT_TRUE(target.restoreSeries("BTC-USD", tf, series));
    }
    target.onTrade(tradeAt(at(-2, 0), 200, 1));
    for (const int64_t tf : {100, 1'000}) {
        EXPECT_FALSE(hasBar(target.exportSeries("BTC-USD", tf).active)) << tf;
        EXPECT_EQ(lateDropsOf(target, "BTC-USD", tf), 1u) << tf;
    }
    target.onTrade(tradeAt(at(1, 31), 300, 1));
    for (const int64_t tf : {100, 1'000})
        EXPECT_EQ(target.exportSeries("BTC-USD", tf).active.count, 1u) << tf;

    // An unseeded series carries no floor and installs none.
    TimeframeAggregator unseeded({100, 1'000, 60'000});
    const auto none = unseeded.exportSeries("BTC-USD", 1'000);
    EXPECT_EQ(none.floorMs, std::nullopt);
    TimeframeAggregator plain({100, 1'000, 60'000});
    ASSERT_TRUE(plain.restoreSeries("BTC-USD", 1'000, none));
    plain.advance("BTC-USD", at(1, 30));
    plain.onTrade(tradeAt(at(-2, 0), 200, 1));
    EXPECT_EQ(plain.exportSeries("BTC-USD", 1'000).active.count, 1u);
    EXPECT_EQ(lateDropsOf(plain, "BTC-USD", 1'000), 0u);
    EXPECT_EQ(plain.exportSeries("BTC-USD", 1'000).floorMs, std::nullopt);
}

TEST(TimeframeAggregatorRestore, UnseededLivePathIsUnchanged) {
    TimeframeAggregator aggregator({100, 1'000, 60'000});
    aggregator.advance("BTC-USD", at(1, 30));
    aggregator.onTrade(tradeAt(at(-2, 0), 200, 1));
    for (const int64_t tf : {100, 1'000, 60'000}) {
        const auto series = aggregator.exportSeries("BTC-USD", tf);
        EXPECT_EQ(series.active.count, 1u) << tf;
        EXPECT_EQ(series.floorMs, std::nullopt) << tf;
        EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", tf), 0u) << tf;
    }
}

// Restoring 100 ms and 1 s leaves the minute and its rollups unfloored: a
// delayed trade after the restore opens the minute exactly as in a fresh
// aggregator, while the restored timeframes drop it as the continuous one does.
TEST(TimeframeAggregatorRestore, ShortSeriesInstallLeavesMinuteUnfloored) {
    const std::vector<int64_t> tfs{100, 1'000, 60'000, 300'000};
    TimeframeAggregator continuous(tfs);
    int64_t sent = INT64_MIN;
    const auto watermark = [&](TimeframeAggregator& a, int64_t receiveMs) {
        if (receiveMs / 1000 > sent) {
            sent = receiveMs / 1000;
            a.advance("BTC-USD", receiveMs - 1'000);
        }
    };
    for (int k = 0; k < 60; ++k) {
        continuous.onTrade(tradeAt(at(0, k, 300), 100 + k, 1));
        watermark(continuous, at(0, k, 400));
    }
    // Restored state at a record received 12:00:59.900 (active bars at 12:00:59).
    TimeframeAggregator restored(tfs);
    for (const int64_t tf : {100, 1'000}) {
        auto series = continuous.exportSeries("BTC-USD", tf);
        ASSERT_EQ(series.active.timestamp_ms, at(0, 59, tf == 100 ? 300 : 0));
        ASSERT_TRUE(restored.restoreSeries("BTC-USD", tf, std::move(series)));
    }
    TimeframeAggregator fresh(tfs); // the live minute starts empty after a restart
    const auto feed = [&](const std::function<void(TimeframeAggregator&)>& step) {
        for (auto* a : {&continuous, &restored, &fresh}) step(*a);
    };
    feed([](TimeframeAggregator& a) { a.advance("BTC-USD", at(1, 0)); }); // heartbeat 12:01:01
    feed([](TimeframeAggregator& a) { a.onTrade(tradeAt(at(0, 59, 900), 500, 2)); });
    EXPECT_EQ(lateDropsOf(restored, "BTC-USD", 60'000), 0u);
    for (const int64_t tf : {100, 1'000}) {
        EXPECT_EQ(lateDropsOf(restored, "BTC-USD", tf), 1u) << tf;
        EXPECT_EQ(lateDropsOf(restored, "BTC-USD", tf), lateDropsOf(continuous, "BTC-USD", tf) ) << tf;
    }
    feed([](TimeframeAggregator& a) { a.advance("BTC-USD", at(3, 0)); });
    const auto minutes = restored.getHistory("BTC-USD", 60'000);
    ASSERT_EQ(minutes.size(), 3u); // 12:00 (one trade) and two carried minutes
    EXPECT_EQ(minutes[0].timestamp_ms, at(0, 0));
    EXPECT_EQ(minutes[0].count, 1u);
    EXPECT_DOUBLE_EQ(minutes[0].open, 500);
    expectSameHistory(minutes, fresh.getHistory("BTC-USD", 60'000), 60'000);
    EXPECT_TRUE(sameBar(restored.exportSeries("BTC-USD", 300'000).active,
                        fresh.exportSeries("BTC-USD", 300'000).active));
    for (const int64_t tf : {100, 1'000}) {
        expectSameHistory(restored.getHistory("BTC-USD", tf, 100'000),
                          continuous.getHistory("BTC-USD", tf, 100'000), tf);
        EXPECT_EQ(restored.exportSeries("BTC-USD", tf).floorMs, std::nullopt) << tf;
    }
}

// The floor guards only a timeframe without a bar: seeding after the first bar
// changes nothing, and an installed floor above the open bar is never read.
TEST(TimeframeAggregatorRestore, FloorIsNeitherSeededNorReadOnceABarExists) {
    TimeframeAggregator aggregator({100, 1'000, 60'000});
    aggregator.seedJournalFloor("BTC-USD", at(0, 59), {1'000});
    aggregator.onTrade(tradeAt(at(1, 31), 300, 1));
    const auto before = aggregator.exportSeries("BTC-USD", 1'000).floorMs;
    ASSERT_EQ(before, std::optional<int64_t>(at(1, 31)));
    aggregator.seedJournalFloor("BTC-USD", at(2, 0), {1'000});
    EXPECT_EQ(aggregator.exportSeries("BTC-USD", 1'000).floorMs, before);
    aggregator.onTrade(tradeAt(at(1, 31, 500), 310, 1));
    EXPECT_EQ(aggregator.exportSeries("BTC-USD", 1'000).active.count, 2u);
    EXPECT_EQ(lateDropsOf(aggregator, "BTC-USD", 1'000), 0u);
    // An unseeded timeframe with a bar is not turned on by a seed either.
    aggregator.seedJournalFloor("BTC-USD", at(2, 0), {100});
    EXPECT_EQ(aggregator.exportSeries("BTC-USD", 100).floorMs, std::nullopt);

    // A hand-built series whose floor lies above its open bar: the bar test
    // alone decides once a bar exists.
    TimeframeAggregator installed({100, 1'000, 60'000});
    TimeframeAggregator::Series series;
    series.active = bar(at(1, 31), 300, 1, false);
    series.floorMs = at(2, 0);
    ASSERT_TRUE(installed.restoreSeries("BTC-USD", 1'000, series));
    installed.onTrade(tradeAt(at(1, 31, 500), 310, 1));
    EXPECT_EQ(installed.exportSeries("BTC-USD", 1'000).active.count, 2u);
    EXPECT_EQ(lateDropsOf(installed, "BTC-USD", 1'000), 0u);
}
