#include <gtest/gtest.h>
#include "datasources/CandleBackfillState.hpp"
#include <vector>

namespace {
constexpr qint64 minute = 60'000;
const QString btc = QStringLiteral("BTC-USD");
constexpr qint64 now = 10'000 * minute;
void view(CandleBackfillState& state, qint64 start = 1000, qint64 end = 1100) {
    state.setViewport(btc, 60, start * minute, end * minute);
}
bool accept(CandleBackfillState& state, const CandleBackfillState::Request& request,
            qint64 oldestMs, qint64 at = now) {
    return state.accept(request.symbol, request.timeframeSec, request.startSec, request.endSec, oldestMs, at);
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
    EXPECT_FALSE(state.next(950 * minute, false, now + 1000)); // one flight, even after throttle
}

TEST(CandleBackfillState, CapsPagesAndContinuesUntilLatestViewportIsCovered) {
    CandleBackfillState state;
    view(state, 1000, 2000);
    auto first = state.next(2000 * minute, false, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->limit, 350);
    EXPECT_TRUE(accept(state, *first, first->startSec * 1000));
    auto second = state.next(first->startSec * 1000, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->endSec, first->startSec);
    view(state, 1900, 2000); // target changed, reply identity did not
    EXPECT_TRUE(accept(state, *second, second->startSec * 1000, now + 100));
    EXPECT_FALSE(state.next(second->startSec * 1000, false, now + 200));
}

TEST(CandleBackfillState, EmptyAndOverlapOnlyWindowsStepBackToOlderBars) {
    for (bool overlap : {false, true}) {
        CandleBackfillState state;
        view(state);
        auto first = state.next(950 * minute, false, now);
        ASSERT_TRUE(first);
        EXPECT_TRUE(accept(state, *first, overlap ? first->endSec * 1000 : 0));
        EXPECT_FALSE(state.floorReached());
        auto second = state.next(950 * minute, false, now + 100);
        ASSERT_TRUE(second); // probe beyond an empty window, even beyond the prefetch target
        EXPECT_EQ(second->endSec, first->startSec);
        EXPECT_EQ(second->limit, 350);
        EXPECT_TRUE(accept(state, *second, second->startSec * 1000, now + 100));
        EXPECT_FALSE(state.floorReached());
        EXPECT_FALSE(state.next(second->startSec * 1000, false, now + 200));
        view(state, 400, 700);
        EXPECT_TRUE(state.next(second->startSec * 1000, false, now + 200));
    }
}

TEST(CandleBackfillState, EmptyScanIsBoundedAndSoftFloorCanRetryAfterCooldown) {
    CandleBackfillState state;
    view(state, 4000, 4100);
    qint64 expectedEnd = 4050 * 60;
    for (int i = 0; i < CandleBackfillState::kMaxEmptyPages; ++i) {
        auto request = state.next(4050 * minute, false, now + i * 100);
        ASSERT_TRUE(request);
        EXPECT_EQ(request->endSec, expectedEnd);
        EXPECT_TRUE(accept(state, *request, 0, now + i * 100));
        expectedEnd = request->startSec;
    }
    EXPECT_TRUE(state.floorReached());
    EXPECT_FALSE(state.next(4050 * minute, false, now + 300));
    state.setViewport("ETH-USD", 60, 4000 * minute, 4100 * minute);
    view(state, 4000, 4100);
    EXPECT_FALSE(state.next(4050 * minute, false, now + 500)); // preserved per selection
    auto retry = state.next(4050 * minute, false, now + 200 + CandleBackfillState::kFloorRetryMs);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->endSec, 4050 * 60); // recheck transient empties from the loaded edge
}

TEST(CandleBackfillState, ErrorAfterEmptyPageRetriesSameWindowWithBackoff) {
    CandleBackfillState state;
    view(state);
    auto first = state.next(950 * minute, false, now);
    ASSERT_TRUE(first);
    ASSERT_TRUE(accept(state, *first, 0));
    auto second = state.next(950 * minute, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_FALSE(state.fail("ETH-USD", now + 100));
    EXPECT_TRUE(state.fail(btc, now + 100));
    EXPECT_FALSE(state.floorReached());
    EXPECT_FALSE(state.next(950 * minute, false, now + 2099));
    auto retry = state.next(950 * minute, false, now + 2100);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->startSec, second->startSec);
    EXPECT_EQ(retry->endSec, second->endSec);
    EXPECT_TRUE(accept(state, *retry, retry->startSec * 1000, now + 2100));
}

TEST(CandleBackfillState, SelectionChangeDropsStaleReplyEvenAfterReturningToSamePair) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport("ETH-USD", 300, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(state.next(0, false, now + 100));
    view(state);
    EXPECT_FALSE(state.accept(btc, 300, request->startSec, request->endSec, 900 * minute, now + 100));
    EXPECT_FALSE(state.next(950 * minute, false, now + 100)); // wrong tag cannot release flight
    EXPECT_FALSE(accept(state, *request, 0, now + 100));
    auto next = state.next(950 * minute, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endSec, request->endSec); // stale empty reply did not advance the cursor
}

TEST(CandleBackfillState, TimeframeChangeDropsStaleReply) {
    CandleBackfillState state;
    view(state);
    auto request = state.next(950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport(btc, 300, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(accept(state, *request, 900 * minute));
    auto next = state.next(0, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->timeframeSec, 300);
}

TEST(CandleBackfillState, InvalidViewportFullCacheAndFutureViewportDoNotRequest) {
    CandleBackfillState state;
    state.setViewport(btc, 60, 0, 0);
    EXPECT_FALSE(state.next(0, false, now));
    view(state);
    EXPECT_FALSE(state.next(950 * minute, true, now));
    view(state, 11000, 11100);
    EXPECT_FALSE(state.next(0, false, now));
    view(state, 9990, 10010);
    auto request = state.next(0, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->endSec, 10001 * 60);
}

TEST(CandleBackfillState, SustainedFastPanSendsLeadingAndPeriodicRequests) {
    CandleBackfillState state;
    qint64 oldest = 2000 * minute;
    std::vector<int> sentAt;
    for (int elapsed = 0; elapsed <= 240; elapsed += 20) {
        view(state, 1000 - elapsed, 2000 - elapsed); // movement never pauses for 100ms
        auto request = state.next(oldest, false, now + elapsed);
        if (!request) continue;
        sentAt.push_back(elapsed);
        EXPECT_TRUE(accept(state, *request, request->startSec * 1000, now + elapsed));
        oldest = request->startSec * 1000;
    }
    EXPECT_EQ(sentAt, (std::vector<int>{0, 100, 200}));
    // Trailing request consumes the final viewport after movement stops.
    EXPECT_EQ(state.retryDelayMs(now + 240), 60);
    EXPECT_FALSE(state.next(oldest, false, now + 299));
    EXPECT_TRUE(state.next(oldest, false, now + 300));
}

TEST(CandleBackfillState, SlowReplyPreventsOverlapButDoesNotAddAnotherDebounce) {
    CandleBackfillState state;
    view(state, 1000, 2000);
    auto request = state.next(2000 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_FALSE(state.next(2000 * minute, false, now + 500));
    EXPECT_TRUE(accept(state, *request, request->startSec * 1000, now + 500));
    EXPECT_TRUE(state.next(request->startSec * 1000, false, now + 500));
}

TEST(CandleBackfillState, OneSecondClientUsesInclusiveEndWithoutOverlapAndAcceptsSparseBars) {
    CandleBackfillState state;
    state.setViewport(btc, 1, 900'000, 1'000'000);
    auto request = state.next(950'000, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->endSec, 949);
    EXPECT_EQ(request->limit, 150);
    // Retained-bar pages can extend earlier than start_time_sec across a gap.
    EXPECT_TRUE(accept(state, *request, 750'000));
    state.setViewport(btc, 1, 700'000, 800'000);
    auto next = state.next(750'000, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endSec, 749);
}

TEST(CandleBackfillState, CoarsePagesBoundRestWorkAndReconnectRearmsSameViewport) {
    CandleBackfillState state;
    state.setViewport(btc, 300, 1000 * minute, 2000 * minute);
    auto request = state.next(2000 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->limit, 70);
    state.disconnect();
    EXPECT_FALSE(accept(state, *request, 0));
    EXPECT_TRUE(state.setViewport(btc, 300, 1000 * minute, 2000 * minute));
    EXPECT_TRUE(state.next(2000 * minute, false, now));
}
