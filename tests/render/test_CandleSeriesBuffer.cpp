#include <gtest/gtest.h>

#include "datasources/CandleSeriesBuffer.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include "datasources/CandleBackfillState.hpp"
#include <QCoreApplication>
#include <QEvent>

#include <vector>
#include "marketdata/rest/CoinbaseRestClient.hpp"

namespace {

using Bar = CandleSeriesBuffer::CandleBar;
const QString kSym = QStringLiteral("BTC-USD");
constexpr int64_t kTfMs = 60000;

Bar bar(qint64 minute, double close, bool closed) {
    Bar b;
    b.timeStartMs = minute * 60'000;
    b.timeEndMs = b.timeStartMs + 60'000;
    b.open = b.high = b.low = b.close = close;
    b.isClosed = closed;
    return b;
}

std::vector<Bar> visible(const CandleSeriesBuffer& buffer) {
    std::vector<Bar> out;
    buffer.getVisibleSlice(kSym, kTfMs, 0, 1'000 * 60'000, out);
    return out;
}

bool sortedByStart(const std::vector<Bar>& bars) {
    for (size_t i = 1; i < bars.size(); ++i) {
        if (bars[i - 1].timeStartMs >= bars[i].timeStartMs) return false;
    }
    return true;
}

} // namespace

// The live forming bar can arrive before the history reply. History must merge
// in time order and must not advance the live seq, or later live updates are
// dropped as stale (the bug: history used seq 1..N and was appended after live).
TEST(CandleSeriesBuffer, HistoryAfterLiveStaysSortedAndLiveKeepsFlowing) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfMs, bar(100, 1.0, false), 1, false);

    std::vector<Bar> history;
    for (int m = 10; m < 100; ++m) history.push_back(bar(m, 0.5, true));
    buffer.applyHistory(kSym, kTfMs, history);

    auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 91u);
    EXPECT_TRUE(sortedByStart(bars));
    EXPECT_EQ(bars.back().timeStartMs, 100 * 60'000);

    buffer.applyUpdate(kSym, kTfMs, bar(100, 2.0, false), 2, false);
    bars = visible(buffer);
    EXPECT_DOUBLE_EQ(bars.back().close, 2.0);
}

TEST(CandleSeriesBuffer, RestFetchedBeforeBoundaryCannotCloseOrRegressNewerLiveBar) {
    CandleSeriesBuffer buffer;
    auto snapshot = bar(5, 11.0, true); // fetched while open, labelled closed after fetch
    snapshot.high = 12.0;
    snapshot.low = 9.0;
    snapshot.volume = 10.0;
    auto live = bar(5, 13.0, false);
    live.high = 15.0;
    live.low = 8.0;
    live.volume = 20.0;
    buffer.applyUpdate(kSym, kTfMs, bar(5, 10.0, false), 1, false);
    buffer.applyUpdate(kSym, kTfMs, live, 2, false); // arrives while REST is in flight
    buffer.applyHistory(kSym, kTfMs, {snapshot});
    auto actual = visible(buffer).back();
    EXPECT_FALSE(actual.isClosed);
    EXPECT_DOUBLE_EQ(actual.close, 13.0);
    EXPECT_DOUBLE_EQ(actual.high, 15.0);
    EXPECT_DOUBLE_EQ(actual.low, 8.0);
    EXPECT_DOUBLE_EQ(actual.volume, 20.0);
    EXPECT_EQ(actual.seq, 2);

    live.close = 14.0;
    live.high = 16.0;
    live.low = 7.0;
    live.volume = 25.0;
    buffer.applyUpdate(kSym, kTfMs, live, 3, true); // real live final still takes effect
    actual = visible(buffer).back();
    EXPECT_TRUE(actual.isClosed);
    EXPECT_DOUBLE_EQ(actual.close, 14.0);
    EXPECT_DOUBLE_EQ(actual.high, 16.0);
    EXPECT_DOUBLE_EQ(actual.low, 7.0);
    EXPECT_DOUBLE_EQ(actual.volume, 25.0);
    buffer.applyHistory(kSym, kTfMs, {snapshot});
    EXPECT_DOUBLE_EQ(visible(buffer).back().volume, 25.0);
}

TEST(CandleSeriesBuffer, LiveTakesOwnershipOfPreviouslyClosedHistorySnapshot) {
    CandleSeriesBuffer buffer;
    buffer.applyHistory(kSym, kTfMs, {bar(5, 3.0, true)});
    buffer.applyUpdate(kSym, kTfMs, bar(5, 9.0, false), 1, false);
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 9.0);
    EXPECT_FALSE(visible(buffer).back().isClosed);
    buffer.applyHistory(kSym, kTfMs, {bar(5, 4.0, true)});
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 9.0);
}

TEST(CandleSeriesBuffer, NewServerSessionSeqIsAcceptedAfterReset) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfMs, bar(1, 1.0, false), 50, false);
    buffer.applyUpdate(kSym, kTfMs, bar(2, 2.0, false), 3, false);  // stale within a session
    EXPECT_EQ(visible(buffer).size(), 1u);

    buffer.resetSequences();                                        // reconnect: seq restarts
    buffer.applyUpdate(kSym, kTfMs, bar(2, 2.0, false), 3, false);
    EXPECT_EQ(visible(buffer).size(), 2u);
}

TEST(CandleSeriesBuffer, LateLiveBarIsInsertedInOrder) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfMs, bar(10, 1.0, true), 1, true);
    buffer.applyUpdate(kSym, kTfMs, bar(12, 1.0, false), 2, false);
    buffer.applyUpdate(kSym, kTfMs, bar(11, 1.0, true), 3, true);
    const auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 3u);
    EXPECT_TRUE(sortedByStart(bars));
}

TEST(CandleSeriesBuffer, OverlappingUnsortedDuplicatePagesPreserveLiveAndOrder) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfMs, bar(100, 9.0, false), 1, false);
    buffer.applyHistory(kSym, kTfMs, {bar(99, 5.0, true), bar(98, 4.0, true), bar(100, 1.0, false)});
    buffer.applyHistory(kSym, kTfMs, {bar(98, 0.0, true), bar(96, 2.0, true),
                                     bar(97, 3.0, true), bar(96, 0.0, true), bar(0, 0.0, true)});
    auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 5u);
    EXPECT_TRUE(sortedByStart(bars));
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfMs), 96 * 60'000);
    EXPECT_DOUBLE_EQ(bars[0].close, 2.0);
    EXPECT_DOUBLE_EQ(bars[2].close, 4.0);
    EXPECT_DOUBLE_EQ(bars.back().close, 9.0);
    buffer.applyUpdate(kSym, kTfMs, bar(100, 10.0, false), 2, false);
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 10.0);
    EXPECT_FALSE(buffer.historyCapacityReached(kSym, kTfMs));
    EXPECT_EQ(buffer.oldestTimeMs("ETH-USD", kTfMs), 0);
}

TEST(CandleSeriesBuffer, CapacityGuardStopsBackfillThatWouldBeImmediatelyEvicted) {
    CandleSeriesBuffer buffer;
    std::vector<Bar> page;
    for (int m = 1; m <= 20000; ++m) page.push_back(bar(m, 1.0, true));
    buffer.applyHistory(kSym, kTfMs, page);
    EXPECT_TRUE(buffer.historyCapacityReached(kSym, kTfMs));
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfMs), 60'000);
    buffer.applyUpdate(kSym, kTfMs, bar(20001, 2.0, false), 1, false);
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfMs), 120'000);
}


TEST(CandleSeriesBuffer, ReconnectRevokesAllCachedOwnershipAndAllowsHistoryCorrection) {
    int argc = 1;
    char name[] = "candle-reconnect";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    ASSERT_NE(buffer, nullptr);
    buffer->applyUpdate(kSym, kTfMs, bar(4, 1.0, true), 49, true);
    buffer->applyUpdate(kSym, kTfMs, bar(5, 2.0, true), 50, true);
    // Exercise the actual queued reconnect hook without opening a connection.
    source.streamClient()->connected();
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    for (const auto& cached : visible(*buffer)) EXPECT_EQ(cached.seq, 0);
    buffer->applyHistory(kSym, kTfMs, {bar(4, 3.0, true), bar(5, 4.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).front().close, 3.0);
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 4.0);
    buffer->applyUpdate(kSym, kTfMs, bar(5, 5.0, false), 1, false);
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 5.0);
    EXPECT_EQ(visible(*buffer).back().seq, 1);
    buffer->applyHistory(kSym, kTfMs, {bar(5, 1.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 5.0); // current-session ownership restored
}

TEST(CandleSeriesBuffer, SelectionReentryRevokesCachedOwnershipButOrdinaryViewportChangesDoNot) {
    int argc = 1;
    char name[] = "candle-selection";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    ASSERT_NE(buffer, nullptr);
    source.setCandleHistoryViewport(kSym, kTfMs, 0, 0);
    buffer->applyUpdate(kSym, kTfMs, bar(5, 2.0, true), 50, true);
    source.setCandleHistoryViewport("ETH-USD", kTfMs, 0, 0);
    source.setCandleHistoryViewport(kSym, kTfMs, 0, 0);
    EXPECT_EQ(visible(*buffer).back().seq, 0);
    buffer->applyHistory(kSym, kTfMs, {bar(5, 3.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 3.0);
    buffer->applyUpdate(kSym, kTfMs, bar(5, 4.0, false), 1, false);
    source.setCandleHistoryViewport(kSym, kTfMs, 0, 1); // same selection, no valid history request
    EXPECT_EQ(visible(*buffer).back().seq, 1);
    buffer->applyHistory(kSym, kTfMs, {bar(5, 1.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 4.0);
    source.setCandleHistoryViewport(kSym, 300000, 0, 0);
    source.setCandleHistoryViewport(kSym, kTfMs, 0, 0);
    EXPECT_EQ(visible(*buffer).back().seq, 0); // timeframe reentry also revokes ownership
    buffer->applyUpdate(kSym, kTfMs, bar(5, 6.0, true), 1, true);
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 6.0);
}

TEST(CandleSeriesBuffer, NativeHistoryAndLiveAggregatorMeetWithoutGapOrOverlapInEitherArrivalOrder) {
    constexpr int64_t tfSec = 900, tfMs = tfSec * 1000, seam = 100 * tfMs;
    for (bool historyFirst : {false, true}) {
        CandleHistoryCache cache;
        const auto history = cache.fetch("BTC-USD", tfSec, (seam - 2 * tfMs) / 1000,
            (seam + tfMs) / 1000, (seam + 1000) / 1000,
            [](int64_t start, int64_t end, const std::string&) {
                CandleFetchResult result{true, {}, {}};
                for (auto t = start; t <= end; t += tfSec)
                    result.candles.push_back({t * 1000, 10, 10, 10, 10, 1});
                return result; // inclusive provider end must not leak a forming bar
            });
        ASSERT_TRUE(history.ok);
        ASSERT_EQ(history.candles.size(), 2u);
        CandleSeriesBuffer buffer;
        TimeframeAggregator aggregator({60'000, tfMs});
        uint64_t seq = 0;
        const auto convert = [](const OHLCVBar& b) {
            Bar out;
            out.timeStartMs = b.timestamp_ms;
            out.timeEndMs = b.timestamp_ms + tfMs;
            out.open = b.open; out.high = b.high; out.low = b.low;
            out.close = b.close; out.volume = b.volume; out.isClosed = b.is_closed;
            return out;
        };
        QObject::connect(&aggregator, &TimeframeAggregator::barUpdated, &buffer,
            [&](const QString& symbol, int64_t tf, const OHLCVBar& b) {
                if (tf == tfMs) buffer.applyUpdate(symbol, tfMs, convert(b), ++seq, false);
            });
        QObject::connect(&aggregator, &TimeframeAggregator::barClosed, &buffer,
            [&](const QString& symbol, int64_t tf, const OHLCVBar& b) {
                if (tf == tfMs) buffer.applyUpdate(symbol, tfMs, convert(b), ++seq, true);
            });
        const auto merge = [&] {
            std::vector<Bar> bars;
            for (const auto& b : history.candles) bars.push_back(convert(b));
            buffer.applyHistory(kSym, tfMs, bars);
        };
        if (historyFirst) merge();
        Trade trade{};
        trade.product_id = "BTC-USD"; trade.price = 20; trade.size = 2;
        trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(seam + 1000));
        aggregator.onTrade(trade);
        aggregator.advance("BTC-USD", seam + 60'000); // coarse live updates consume closed anchor minutes
        if (!historyFirst) merge();
        aggregator.advance("BTC-USD", seam + tfMs);
        trade.timestamp += std::chrono::milliseconds(tfMs);
        trade.price = 30;
        aggregator.onTrade(trade);
        aggregator.advance("BTC-USD", seam + tfMs + 60'000);
        merge(); // delayed native reply cannot duplicate or overwrite live ownership
        std::vector<Bar> bars;
        buffer.getVisibleSlice(kSym, tfMs, seam - 2 * tfMs, seam + 2 * tfMs, bars);
        ASSERT_EQ(bars.size(), 4u);
        for (size_t i = 1; i < bars.size(); ++i)
            EXPECT_EQ(bars[i - 1].timeEndMs, bars[i].timeStartMs);
        EXPECT_TRUE(bars[2].isClosed);
        EXPECT_EQ(bars[2].close, 20);
        EXPECT_FALSE(bars[3].isClosed);
        EXPECT_EQ(bars[3].close, 30);
    }
}

TEST(CandleSeriesBuffer, CoverageFollowsPagesLiveBarsAndEviction) {
    CandleSeriesBuffer buffer;
    EXPECT_EQ(buffer.coverage(kSym, kTfMs), nullptr);
    // A page proves its window, also where it has no bar; an empty page still proves its window.
    buffer.applyHistory(kSym, kTfMs, {bar(105, 1, true)}, 100 * 60'000, 110 * 60'000);
    buffer.applyHistory(kSym, kTfMs, {}, 90 * 60'000, 95 * 60'000);
    const CandleCoverage* coverage = buffer.coverage(kSym, kTfMs);
    ASSERT_NE(coverage, nullptr);
    EXPECT_TRUE(coverage->coversRange(100 * 60'000, 110 * 60'000));
    EXPECT_TRUE(coverage->coversRange(90 * 60'000, 95 * 60'000));
    EXPECT_FALSE(coverage->covers(97 * 60'000));
    // Without a window, a page proves the span between its first and last bar.
    buffer.applyHistory(kSym, kTfMs, {bar(120, 1, true), bar(130, 1, true)});
    EXPECT_TRUE(coverage->coversRange(120 * 60'000, 131 * 60'000));
    EXPECT_FALSE(coverage->covers(115 * 60'000));
    // Live bars cover from the first one on until the stream ends.
    buffer.applyUpdate(kSym, kTfMs, bar(140, 2, false), 1, false);
    EXPECT_TRUE(coverage->covers(10'000 * 60'000));
    buffer.endLiveCoverage(kSym, kTfMs);
    EXPECT_TRUE(coverage->covers(140 * 60'000));
    EXPECT_FALSE(coverage->covers(141 * 60'000));
    buffer.applyUpdate(kSym, kTfMs, bar(150, 2, false), 2, false);
    buffer.resetSeriesForSelection(kSym, kTfMs); // a new stream starts its own run
    EXPECT_FALSE(coverage->covers(151 * 60'000));
}

TEST(CandleSeriesBuffer, EvictedBarsTakeTheirCoverageAlong) {
    CandleSeriesBuffer buffer;
    constexpr qint64 capacity = 20'000; // 1m series
    std::vector<Bar> page;
    for (qint64 m = 0; m < capacity + 10; ++m) page.push_back(bar(1'000 + m, 1, true));
    buffer.applyHistory(kSym, kTfMs, page, 900 * 60'000, (1'000 + capacity + 10) * 60'000);
    const CandleCoverage* coverage = buffer.coverage(kSym, kTfMs);
    ASSERT_NE(coverage, nullptr);
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfMs), 1'010 * 60'000);
    EXPECT_FALSE(coverage->covers(1'009 * 60'000));
    EXPECT_TRUE(coverage->covers(1'010 * 60'000));
    buffer.applyUpdate(kSym, kTfMs, bar(1'000 + capacity + 10, 1, false), 1, false);
    EXPECT_FALSE(coverage->covers(1'010 * 60'000)) << "the ring wrapped over the oldest bar";
}

namespace {
// The paging loop of RemoteGridDataSource against the real buffer and a fake
// server that answers full REST pages (every closed bucket of the window).
struct PagingLoop {
    CandleSeriesBuffer& buffer;
    qint64 tf;
    qint64 clock;
    CandleBackfillState state{CandleBackfillState::kDefaultEmptyLookbackMs};
    std::vector<std::pair<qint64, qint64>> sent;

    // Pages until idle or `cap` requests; returns the number sent.
    int run(int cap) {
        const CandleCoverage none;
        for (int pages = 0; pages < cap; ++pages) {
            const CandleCoverage* coverage = buffer.coverage(kSym, tf);
            const CandleBackfillState::Loaded loaded{coverage ? *coverage : none, buffer.oldestTimeMs(kSym, tf),
                                                     buffer.historyCapacityReached(kSym, tf)};
            if (!state.needsHistory(loaded, clock)) return pages;
            clock += state.retryDelayMs(loaded, clock);
            const auto request = state.next(loaded, clock);
            if (!request) return pages;
            sent.emplace_back(request->startMs, request->endMs);
            std::vector<Bar> bars;
            for (qint64 t = request->startMs; t < std::min(request->endMs, clock / tf * tf); t += tf)
                bars.push_back({t, t + tf, 1, 1, 1, 1, 1, true, 0, false});
            clock += 50;
            EXPECT_TRUE(state.accept(kSym, tf, request->startMs, request->endMs, bars.empty() ? 0 : bars.front().timeStartMs,
                                     clock));
            qint64 start = 0, end = 0;
            if (!state.acceptedCoverage(bars.empty() ? 0 : bars.front().timeStartMs, start, end)) start = end = 0;
            buffer.applyHistory(kSym, tf, bars, start, end);
        }
        return cap;
    }
};
constexpr qint64 kFiveMin = 300'000;
} // namespace

// A full ring keeps its newest bars. A viewport before the oldest kept bar asks
// for nothing: any page there, or between it and the kept bars, is evicted at once.
TEST(CandleSeriesBuffer, FullRingAsksNothingOlderThanItsOldestBar) {
    CandleSeriesBuffer buffer;
    const qint64 nowMs = 2'000'000'100'000, liveEdge = nowMs / kFiveMin * kFiveMin;
    std::vector<Bar> page;
    for (qint64 i = 10'000; i > 0; --i) {
        const qint64 t = liveEdge - i * kFiveMin;
        page.push_back({t, t + kFiveMin, 1, 1, 1, 1, 1, true, 0, false});
    }
    buffer.applyHistory(kSym, kFiveMin, page, page.front().timeStartMs, liveEdge);
    buffer.openLiveCoverage(kSym, kFiveMin, liveEdge);
    ASSERT_TRUE(buffer.historyCapacityReached(kSym, kFiveMin));
    PagingLoop loop{buffer, kFiveMin, nowMs};
    const qint64 oldest = buffer.oldestTimeMs(kSym, kFiveMin);
    loop.state.setViewport(kSym, kFiveMin, oldest - 2'000 * kFiveMin, oldest - 1'000 * kFiveMin);
    EXPECT_EQ(loop.run(30), 0) << "first request ends at " << (loop.sent.empty() ? 0 : loop.sent.front().second);
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kFiveMin), oldest);
}

// Sixteen separate spans fill the coverage set; a page older than all of them must
// keep its coverage so paging moves on instead of asking for it again.
TEST(CandleSeriesBuffer, PageOlderThanAFullCoverageSetIsKeptAndPagingSettles) {
    CandleSeriesBuffer buffer;
    const qint64 nowMs = 2'000'000'100'000, liveEdge = nowMs / kFiveMin * kFiveMin;
    for (int i = 0; i < CandleCoverage::kMaxSpans; ++i) {
        const qint64 t = liveEdge - (CandleCoverage::kMaxSpans - i) * 20 * kFiveMin;
        buffer.applyHistory(kSym, kFiveMin, {{t, t + kFiveMin, 1, 1, 1, 1, 1, true, 0, false}}, t, t + 10 * kFiveMin);
    }
    buffer.openLiveCoverage(kSym, kFiveMin, liveEdge);
    ASSERT_EQ(buffer.coverage(kSym, kFiveMin)->spanCount(), CandleCoverage::kMaxSpans);
    PagingLoop loop{buffer, kFiveMin, nowMs};
    const qint64 viewEnd = liveEdge - 2'000 * kFiveMin;
    loop.state.setViewport(kSym, kFiveMin, viewEnd - 100 * kFiveMin, viewEnd);
    const int pages = loop.run(60);
    EXPECT_LT(pages, 60) << "paging repeats a page";
    EXPECT_TRUE(buffer.coverage(kSym, kFiveMin)->coversRange(viewEnd - 200 * kFiveMin, liveEdge));
}
