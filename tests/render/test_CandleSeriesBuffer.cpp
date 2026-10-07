#include <gtest/gtest.h>

#include "datasources/CandleSeriesBuffer.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QCoreApplication>
#include <QEvent>

#include <vector>
#include "marketdata/rest/CoinbaseRestClient.hpp"

namespace {

using Bar = CandleSeriesBuffer::CandleBar;
const QString kSym = QStringLiteral("BTC-USD");
constexpr int64_t kTfSec = 60;

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
    buffer.getVisibleSlice(kSym, kTfSec, 0, 1'000 * 60'000, out);
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
    buffer.applyUpdate(kSym, kTfSec, bar(100, 1.0, false), 1, false);

    std::vector<Bar> history;
    for (int m = 10; m < 100; ++m) history.push_back(bar(m, 0.5, true));
    buffer.applyHistory(kSym, kTfSec, history);

    auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 91u);
    EXPECT_TRUE(sortedByStart(bars));
    EXPECT_EQ(bars.back().timeStartMs, 100 * 60'000);

    buffer.applyUpdate(kSym, kTfSec, bar(100, 2.0, false), 2, false);
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
    buffer.applyUpdate(kSym, kTfSec, bar(5, 10.0, false), 1, false);
    buffer.applyUpdate(kSym, kTfSec, live, 2, false); // arrives while REST is in flight
    buffer.applyHistory(kSym, kTfSec, {snapshot});
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
    buffer.applyUpdate(kSym, kTfSec, live, 3, true); // real live final still takes effect
    actual = visible(buffer).back();
    EXPECT_TRUE(actual.isClosed);
    EXPECT_DOUBLE_EQ(actual.close, 14.0);
    EXPECT_DOUBLE_EQ(actual.high, 16.0);
    EXPECT_DOUBLE_EQ(actual.low, 7.0);
    EXPECT_DOUBLE_EQ(actual.volume, 25.0);
    buffer.applyHistory(kSym, kTfSec, {snapshot});
    EXPECT_DOUBLE_EQ(visible(buffer).back().volume, 25.0);
}

TEST(CandleSeriesBuffer, LiveTakesOwnershipOfPreviouslyClosedHistorySnapshot) {
    CandleSeriesBuffer buffer;
    buffer.applyHistory(kSym, kTfSec, {bar(5, 3.0, true)});
    buffer.applyUpdate(kSym, kTfSec, bar(5, 9.0, false), 1, false);
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 9.0);
    EXPECT_FALSE(visible(buffer).back().isClosed);
    buffer.applyHistory(kSym, kTfSec, {bar(5, 4.0, true)});
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 9.0);
}

TEST(CandleSeriesBuffer, NewServerSessionSeqIsAcceptedAfterReset) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfSec, bar(1, 1.0, false), 50, false);
    buffer.applyUpdate(kSym, kTfSec, bar(2, 2.0, false), 3, false);  // stale within a session
    EXPECT_EQ(visible(buffer).size(), 1u);

    buffer.resetSequences();                                        // reconnect: seq restarts
    buffer.applyUpdate(kSym, kTfSec, bar(2, 2.0, false), 3, false);
    EXPECT_EQ(visible(buffer).size(), 2u);
}

TEST(CandleSeriesBuffer, LateLiveBarIsInsertedInOrder) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfSec, bar(10, 1.0, true), 1, true);
    buffer.applyUpdate(kSym, kTfSec, bar(12, 1.0, false), 2, false);
    buffer.applyUpdate(kSym, kTfSec, bar(11, 1.0, true), 3, true);
    const auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 3u);
    EXPECT_TRUE(sortedByStart(bars));
}

TEST(CandleSeriesBuffer, OverlappingUnsortedDuplicatePagesPreserveLiveAndOrder) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfSec, bar(100, 9.0, false), 1, false);
    buffer.applyHistory(kSym, kTfSec, {bar(99, 5.0, true), bar(98, 4.0, true), bar(100, 1.0, false)});
    buffer.applyHistory(kSym, kTfSec, {bar(98, 0.0, true), bar(96, 2.0, true),
                                     bar(97, 3.0, true), bar(96, 0.0, true), bar(0, 0.0, true)});
    auto bars = visible(buffer);
    ASSERT_EQ(bars.size(), 5u);
    EXPECT_TRUE(sortedByStart(bars));
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfSec), 96 * 60'000);
    EXPECT_DOUBLE_EQ(bars[0].close, 2.0);
    EXPECT_DOUBLE_EQ(bars[2].close, 4.0);
    EXPECT_DOUBLE_EQ(bars.back().close, 9.0);
    buffer.applyUpdate(kSym, kTfSec, bar(100, 10.0, false), 2, false);
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 10.0);
    EXPECT_FALSE(buffer.historyCapacityReached(kSym, kTfSec));
    EXPECT_EQ(buffer.oldestTimeMs("ETH-USD", kTfSec), 0);
}

TEST(CandleSeriesBuffer, CapacityGuardStopsBackfillThatWouldBeImmediatelyEvicted) {
    CandleSeriesBuffer buffer;
    std::vector<Bar> page;
    for (int m = 1; m <= 20000; ++m) page.push_back(bar(m, 1.0, true));
    buffer.applyHistory(kSym, kTfSec, page);
    EXPECT_TRUE(buffer.historyCapacityReached(kSym, kTfSec));
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfSec), 60'000);
    buffer.applyUpdate(kSym, kTfSec, bar(20001, 2.0, false), 1, false);
    EXPECT_EQ(buffer.oldestTimeMs(kSym, kTfSec), 120'000);
}


TEST(CandleSeriesBuffer, ReconnectRevokesAllCachedOwnershipAndAllowsHistoryCorrection) {
    int argc = 1;
    char name[] = "candle-reconnect";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    ASSERT_NE(buffer, nullptr);
    buffer->applyUpdate(kSym, kTfSec, bar(4, 1.0, true), 49, true);
    buffer->applyUpdate(kSym, kTfSec, bar(5, 2.0, true), 50, true);
    // Exercise the actual queued reconnect hook without opening a connection.
    source.streamClient()->connected();
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    for (const auto& cached : visible(*buffer)) EXPECT_EQ(cached.seq, 0);
    buffer->applyHistory(kSym, kTfSec, {bar(4, 3.0, true), bar(5, 4.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).front().close, 3.0);
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 4.0);
    buffer->applyUpdate(kSym, kTfSec, bar(5, 5.0, false), 1, false);
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 5.0);
    EXPECT_EQ(visible(*buffer).back().seq, 1);
    buffer->applyHistory(kSym, kTfSec, {bar(5, 1.0, true)});
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
    source.setCandleHistoryViewport(kSym, kTfSec, 0, 0);
    buffer->applyUpdate(kSym, kTfSec, bar(5, 2.0, true), 50, true);
    source.setCandleHistoryViewport("ETH-USD", kTfSec, 0, 0);
    source.setCandleHistoryViewport(kSym, kTfSec, 0, 0);
    EXPECT_EQ(visible(*buffer).back().seq, 0);
    buffer->applyHistory(kSym, kTfSec, {bar(5, 3.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 3.0);
    buffer->applyUpdate(kSym, kTfSec, bar(5, 4.0, false), 1, false);
    source.setCandleHistoryViewport(kSym, kTfSec, 0, 1); // same selection, no valid history request
    EXPECT_EQ(visible(*buffer).back().seq, 1);
    buffer->applyHistory(kSym, kTfSec, {bar(5, 1.0, true)});
    EXPECT_DOUBLE_EQ(visible(*buffer).back().close, 4.0);
    source.setCandleHistoryViewport(kSym, 300, 0, 0);
    source.setCandleHistoryViewport(kSym, kTfSec, 0, 0);
    EXPECT_EQ(visible(*buffer).back().seq, 0); // timeframe reentry also revokes ownership
    buffer->applyUpdate(kSym, kTfSec, bar(5, 6.0, true), 1, true);
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
                if (tf == tfMs) buffer.applyUpdate(symbol, tfSec, convert(b), ++seq, false);
            });
        QObject::connect(&aggregator, &TimeframeAggregator::barClosed, &buffer,
            [&](const QString& symbol, int64_t tf, const OHLCVBar& b) {
                if (tf == tfMs) buffer.applyUpdate(symbol, tfSec, convert(b), ++seq, true);
            });
        const auto merge = [&] {
            std::vector<Bar> bars;
            for (const auto& b : history.candles) bars.push_back(convert(b));
            buffer.applyHistory(kSym, tfSec, bars);
        };
        if (historyFirst) merge();
        Trade trade{};
        trade.product_id = "BTC-USD"; trade.price = 20; trade.size = 2;
        trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(seam + 1000));
        aggregator.onTrade(trade);
        aggregator.tick(seam + 60'000); // coarse live updates consume closed anchor minutes
        if (!historyFirst) merge();
        aggregator.tick(seam + tfMs);
        trade.timestamp += std::chrono::milliseconds(tfMs);
        trade.price = 30;
        aggregator.onTrade(trade);
        aggregator.tick(seam + tfMs + 60'000);
        merge(); // delayed native reply cannot duplicate or overwrite live ownership
        std::vector<Bar> bars;
        buffer.getVisibleSlice(kSym, tfSec, seam - 2 * tfMs, seam + 2 * tfMs, bars);
        ASSERT_EQ(bars.size(), 4u);
        for (size_t i = 1; i < bars.size(); ++i)
            EXPECT_EQ(bars[i - 1].timeEndMs, bars[i].timeStartMs);
        EXPECT_TRUE(bars[2].isClosed);
        EXPECT_EQ(bars[2].close, 20);
        EXPECT_FALSE(bars[3].isClosed);
        EXPECT_EQ(bars[3].close, 30);
    }
}
