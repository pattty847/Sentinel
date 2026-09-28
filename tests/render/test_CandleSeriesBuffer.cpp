#include <gtest/gtest.h>

#include "datasources/CandleSeriesBuffer.hpp"

#include <vector>

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

TEST(CandleSeriesBuffer, FormingLiveBarBeatsOpenHistoryCopyButFinalHistoryWins) {
    CandleSeriesBuffer buffer;
    buffer.applyUpdate(kSym, kTfSec, bar(5, 9.0, false), 1, false);
    buffer.applyHistory(kSym, kTfSec, {bar(5, 1.0, false)});
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 9.0);

    buffer.applyHistory(kSym, kTfSec, {bar(5, 3.0, true)});
    EXPECT_DOUBLE_EQ(visible(buffer).back().close, 3.0);
    EXPECT_TRUE(visible(buffer).back().isClosed);
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
