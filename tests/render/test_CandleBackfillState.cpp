#include <gtest/gtest.h>
#include "datasources/CandleBackfillState.hpp"

namespace {
constexpr qint64 minute = 60'000;
const QString btc = QStringLiteral("BTC-USD");
constexpr qint64 now = 10'000 * minute;
void view(CandleBackfillState& state, qint64 start = 1000, qint64 end = 1100) {
    state.setViewport(btc, 60, start * minute, end * minute);
}
}

TEST(CandleBackfillState, PrefetchesOneScreenBeforeViewportAndStopsInsideCoverage) {
    CandleBackfillState state;
    view(state);
    EXPECT_FALSE(state.next(900 * minute, false, now));
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->startSec, 900 * 60);
    EXPECT_EQ(request->endSec, 950 * 60);
    EXPECT_EQ(request->limit, 50);
    EXPECT_FALSE(state.next(950 * minute, false, now));
}

TEST(CandleBackfillState, CapsPagesAndContinuesUntilLatestViewportIsCovered) {
    CandleBackfillState state;
    view(state, 1000, 2000);
    auto first = state.next(2000 * minute, false, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->limit, 350);
    EXPECT_TRUE(state.accept(btc, 60, first->startSec, first->endSec, first->startSec * 1000));
    auto second = state.next(first->startSec * 1000, false, now);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->endSec, first->startSec);
    // Panning forward while in flight changes the target, not the reply identity.
    view(state, 1900, 2000);
    EXPECT_TRUE(state.accept(btc, 60, second->startSec, second->endSec, second->startSec * 1000));
    EXPECT_FALSE(state.next(second->startSec * 1000, false, now));
}

TEST(CandleBackfillState, EmptyOrOverlapOnlyReplyRecordsFloorAcrossSelections) {
    for (bool overlap : {false, true}) {
        CandleBackfillState state;
        view(state);
        auto request = state.next(950 * minute, false, now);
        ASSERT_TRUE(request);
        EXPECT_TRUE(state.accept(btc, 60, request->startSec, request->endSec,
                                 overlap ? request->endSec * 1000 : 0));
        view(state, 800, 1100);
        EXPECT_FALSE(state.next(950 * minute, false, now));
        state.setViewport("ETH-USD", 60, 800 * minute, 1100 * minute);
        view(state, 800, 1100);
        EXPECT_FALSE(state.next(950 * minute, false, now));
    }
}

TEST(CandleBackfillState, EmptyBootstrapDoesNotRetryAsClockAdvances) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(0, false, now);
    ASSERT_TRUE(request);
    EXPECT_TRUE(state.accept(btc, 60, request->startSec, request->endSec, 0));
    view(state, 1001, 1101);
    EXPECT_FALSE(state.next(0, false, now + minute));
}

TEST(CandleBackfillState, SelectionChangeDropsStaleReplyEvenAfterReturningToSamePair) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport("ETH-USD", 300, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(state.next(0, false, now)); // old job still occupies transport
    view(state);
    EXPECT_FALSE(state.accept(btc, 300, request->startSec, request->endSec, 900 * minute));
    EXPECT_FALSE(state.next(950 * minute, false, now)); // wrong tag cannot release it
    EXPECT_FALSE(state.accept(btc, 60, request->startSec, request->endSec, 0));
    EXPECT_TRUE(state.next(950 * minute, false, now)); // stale empty reply set no floor
}

TEST(CandleBackfillState, TimeframeChangeDropsStaleReply) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport(btc, 300, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(state.accept(btc, 60, request->startSec, request->endSec, 900 * minute));
    auto next = state.next(0, false, now);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->timeframeSec, 300);
}

TEST(CandleBackfillState, InvalidViewportAndFullCacheDoNotRequest) {
    CandleBackfillState state;
    state.setViewport(btc, 60, 0, 0);
    EXPECT_FALSE(state.next(0, false, now));
    view(state);
    EXPECT_FALSE(state.next(950 * minute, true, now));
}

TEST(CandleBackfillState, ErrorsRetryWithBackoffWithoutSettingFloorAndDisconnectResets) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_FALSE(state.fail("ETH-USD", now));
    EXPECT_TRUE(state.fail(btc, now));
    EXPECT_FALSE(state.next(950 * minute, false, now + 1999));
    EXPECT_TRUE(state.next(950 * minute, false, now + 2000));
    state.disconnect();
    EXPECT_FALSE(state.accept(btc, 60, request->startSec, request->endSec, 0));
    view(state);
    EXPECT_TRUE(state.next(950 * minute, false, now));
}

TEST(CandleBackfillState, QuantizedViewportDoesNotRestartDebounceOnEveryFollowFrame) {
    CandleBackfillState state;
    EXPECT_TRUE(state.setViewport(btc, 60, 1000 * minute + 1, 1100 * minute + 1));
    EXPECT_FALSE(state.setViewport(btc, 60, 1000 * minute + 17, 1100 * minute + 17));
    EXPECT_TRUE(state.setViewport(btc, 60, 999 * minute, 1100 * minute));
}

TEST(CandleBackfillState, FutureViewportDoesNotEstablishFalseFloor) {
    CandleBackfillState state;
    view(state, 11000, 11100);
    EXPECT_FALSE(state.next(0, false, now));
    view(state, 9990, 10010);
    auto request = state.next(0, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->endSec, 10001 * 60);
}

TEST(CandleBackfillState, CoarsePagesBoundAnchorRestWorkAndReconnectRearmsSameViewport) {
    CandleBackfillState state;
    state.setViewport(btc, 300, 1000 * minute, 2000 * minute);
    auto request = state.next(2000 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->limit, 70);
    state.disconnect();
    EXPECT_TRUE(state.setViewport(btc, 300, 1000 * minute, 2000 * minute));
    EXPECT_TRUE(state.next(2000 * minute, false, now));
}
