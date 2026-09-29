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
        EXPECT_FALSE(state.scanPaused());
        auto second = state.next(950 * minute, false, now + 100);
        ASSERT_TRUE(second); // probe beyond an empty window, even beyond the prefetch target
        EXPECT_EQ(second->endSec, first->startSec);
        EXPECT_EQ(second->limit, 350);
        EXPECT_TRUE(accept(state, *second, second->startSec * 1000, now + 100));
        EXPECT_FALSE(state.scanPaused());
        EXPECT_FALSE(state.next(second->startSec * 1000, false, now + 200));
        view(state, 400, 700);
        EXPECT_TRUE(state.next(second->startSec * 1000, false, now + 200));
    }
}

TEST(CandleBackfillState, EmptyScanResumesFromSavedCursorAfterCooldown) {
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
    EXPECT_TRUE(state.scanPaused());
    EXPECT_FALSE(state.next(4050 * minute, false, now + 300));
    state.setViewport("ETH-USD", 60, 4000 * minute, 4100 * minute);
    view(state, 4000, 4100);
    EXPECT_FALSE(state.next(4050 * minute, false, now + 500)); // preserved per selection
    auto retry = state.next(4050 * minute, false, now + 200 + CandleBackfillState::kFloorRetryMs);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->endSec, expectedEnd); // resume beyond all three empty windows
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
    EXPECT_FALSE(state.scanPaused());
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


TEST(CandleBackfillState, StationaryViewportCrossesLongGapWithTimedPausesAndNoStorm) {
    for (qint64 tfSec : {1, 60}) {
        CandleBackfillState state;
        const qint64 oldest = 400'000'000;
        state.setViewport(btc, tfSec, 360'000'000, 440'000'000);
        qint64 clock = now;
        qint64 previousStart = 0;
        int pauses = 0;
        for (int page = 0; page < 8; ++page) {
            const qint64 delay = state.retryDelayMs(clock);
            if (delay == CandleBackfillState::kFloorRetryMs) ++pauses;
            if (delay > 0) {
                EXPECT_FALSE(state.next(oldest, false, clock + delay - 1));
                clock += delay; // the RemoteGridDataSource timer uses this deadline
            }
            auto request = state.next(oldest, false, clock);
            ASSERT_TRUE(request) << "page=" << page << " tf=" << tfSec;
            if (previousStart > 0) EXPECT_LT(request->endSec, previousStart + (tfSec == 1 ? 0 : 1));
            previousStart = request->startSec;
            EXPECT_FALSE(state.next(oldest, false, clock + 500)); // no second flight
            const qint64 replyOldest = page == 7 ? request->startSec * 1000 : 0;
            ASSERT_TRUE(accept(state, *request, replyOldest, clock));
        }
        EXPECT_EQ(pauses, 2); // eight pages require two full 60s pauses, with no viewport updates
        EXPECT_FALSE(state.scanPaused());
    }
}

TEST(CandleBackfillState, OneSecondEmptyLookbackIsBoundedAcrossPauses) {
    constexpr qint64 budgetSec = 2000;
    CandleBackfillState state(budgetSec);
    constexpr qint64 oldest = 100'000'000;
    state.setViewport(btc, 1, 90'000'000, 110'000'000);
    qint64 clock = now;
    qint64 scannedStart = 0;
    int pages = 0;
    while (state.needsOlderData(oldest, false, clock) && pages < 20) {
        clock += state.retryDelayMs(clock);
        auto request = state.next(oldest, false, clock);
        ASSERT_TRUE(request);
        scannedStart = request->startSec;
        EXPECT_GE(scannedStart, oldest / 1000 - budgetSec);
        ASSERT_TRUE(accept(state, *request, 0, clock));
        ++pages;
    }
    EXPECT_EQ(scannedStart, oldest / 1000 - budgetSec);
    EXPECT_EQ(pages, 6);
    EXPECT_FALSE(state.needsOlderData(oldest, false, clock + 10 * CandleBackfillState::kFloorRetryMs));
    EXPECT_FALSE(state.next(oldest, false, clock + 10 * CandleBackfillState::kFloorRetryMs));
}

TEST(CandleBackfillState, PausedScanDoesNotNeedTimerWhenViewportReturnsToLoadedCoverage) {
    CandleBackfillState state;
    view(state, 4000, 4100);
    for (int i = 0; i < 3; ++i) {
        auto request = state.next(4050 * minute, false, now + i * 100);
        ASSERT_TRUE(request);
        ASSERT_TRUE(accept(state, *request, 0, now + i * 100));
    }
    EXPECT_EQ(state.retryDelayMs(now + 200), CandleBackfillState::kFloorRetryMs);
    view(state, 4100, 4150); // prefetch now starts at the loaded edge
    EXPECT_FALSE(state.needsOlderData(4050 * minute, false, now + 200));
    EXPECT_FALSE(state.next(4050 * minute, false, now + 200 + CandleBackfillState::kFloorRetryMs));
}
