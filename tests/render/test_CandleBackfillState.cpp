#include <gtest/gtest.h>
#include "datasources/CandleBackfillState.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

namespace {
constexpr qint64 minute = 60'000;
const QString btc = QStringLiteral("BTC-USD");
constexpr qint64 now = 10'000 * minute;
constexpr qint64 farFuture = std::numeric_limits<qint64>::max() / 4;
// The contiguous buffer the policy once assumed: covered from the oldest bar on.
CandleCoverage coveredFrom(qint64 oldestMs) {
    CandleCoverage coverage;
    if (oldestMs > 0) coverage.add(oldestMs, farFuture);
    return coverage;
}
std::optional<CandleBackfillState::Request> nextPage(CandleBackfillState& state, qint64 oldestMs, bool full, qint64 at) {
    const auto coverage = coveredFrom(oldestMs);
    return state.next({coverage, oldestMs, full}, at);
}
bool needs(CandleBackfillState& state, qint64 oldestMs, bool full, qint64 at) {
    const auto coverage = coveredFrom(oldestMs);
    return state.needsHistory({coverage, oldestMs, full}, at);
}
qint64 delay(CandleBackfillState& state, qint64 oldestMs, qint64 at) {
    const auto coverage = coveredFrom(oldestMs);
    return state.retryDelayMs({coverage, oldestMs, false}, at);
}
void view(CandleBackfillState& state, qint64 start = 1000, qint64 end = 1100) {
    state.setViewport(btc, 60000, start * minute, end * minute);
}
bool accept(CandleBackfillState& state, const CandleBackfillState::Request& request,
            qint64 oldestMs, qint64 at = now) {
    return state.accept(request.symbol, request.timeframeMs, request.startMs, request.endMs, oldestMs, at);
}
bool acceptWireReply(CandleBackfillState& state, const CandleBackfillState::Request& request,
                     qint64 oldestMs, qint64 at = now) {
    const qint64 endSec = request.endMs / 1000;
    const qint64 startSec = endSec - request.timeframeMs / 1000 * request.limit;
    return state.accept(request.symbol, request.timeframeMs, startSec * 1000, endSec * 1000, oldestMs, at);
}

}

TEST(CandleBackfillState, PrefetchesOneScreenBeforeViewportAndStopsInsideCoverage) {
    CandleBackfillState state;
    view(state);
    EXPECT_FALSE(nextPage(state, 900 * minute, false, now));
    auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->startMs, 900 * minute);
    EXPECT_EQ(request->endMs, 950 * minute);
    EXPECT_EQ(request->limit, 50);
    EXPECT_FALSE(nextPage(state, 950 * minute, false, now + 1000)); // one flight, even after throttle
}

TEST(CandleBackfillState, CapsPagesAndContinuesUntilLatestViewportIsCovered) {
    CandleBackfillState state;
    view(state, 1000, 2000);
    auto first = nextPage(state, 2000 * minute, false, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->limit, 350);
    EXPECT_TRUE(accept(state, *first, first->startMs));
    auto second = nextPage(state, first->startMs, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->endMs, first->startMs);
    view(state, 1900, 2000); // target changed, reply identity did not
    EXPECT_TRUE(accept(state, *second, second->startMs, now + 100));
    EXPECT_FALSE(nextPage(state, second->startMs, false, now + 200));
}

TEST(CandleBackfillState, EmptyAndOverlapOnlyWindowsStepBackToOlderBars) {
    for (bool overlap : {false, true}) {
        CandleBackfillState state;
        view(state);
        auto first = nextPage(state, 950 * minute, false, now);
        ASSERT_TRUE(first);
        EXPECT_TRUE(accept(state, *first, overlap ? first->endMs : 0));
        EXPECT_FALSE(state.scanPaused());
        auto second = nextPage(state, 950 * minute, false, now + 100);
        ASSERT_TRUE(second); // probe beyond an empty window, even beyond the prefetch target
        EXPECT_EQ(second->endMs, first->startMs);
        EXPECT_EQ(second->limit, 350);
        EXPECT_TRUE(accept(state, *second, second->startMs, now + 100));
        EXPECT_FALSE(state.scanPaused());
        EXPECT_FALSE(nextPage(state, second->startMs, false, now + 200));
        view(state, 400, 700);
        EXPECT_TRUE(nextPage(state, second->startMs, false, now + 200));
    }
}

TEST(CandleBackfillState, EmptyScanResumesFromSavedCursorAfterCooldown) {
    CandleBackfillState state;
    view(state, 4000, 4100);
    qint64 expectedEnd = 4050 * minute;
    for (int i = 0; i < CandleBackfillState::kMaxEmptyPages; ++i) {
        auto request = nextPage(state, 4050 * minute, false, now + i * 100);
        ASSERT_TRUE(request);
        EXPECT_EQ(request->endMs, expectedEnd);
        EXPECT_TRUE(accept(state, *request, 0, now + i * 100));
        expectedEnd = request->startMs;
    }
    EXPECT_TRUE(state.scanPaused());
    EXPECT_FALSE(nextPage(state, 4050 * minute, false, now + 300));
    state.setViewport("ETH-USD", 60000, 4000 * minute, 4100 * minute);
    view(state, 4000, 4100);
    EXPECT_FALSE(nextPage(state, 4050 * minute, false, now + 500)); // preserved per selection
    auto retry = nextPage(state, 4050 * minute, false, now + 200 + CandleBackfillState::kFloorRetryMs);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->endMs, expectedEnd); // resume beyond all three empty windows
}

TEST(CandleBackfillState, ErrorAfterEmptyPageRetriesSameWindowWithBackoff) {
    CandleBackfillState state;
    view(state);
    auto first = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(first);
    ASSERT_TRUE(accept(state, *first, 0));
    auto second = nextPage(state, 950 * minute, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_FALSE(state.fail("ETH-USD", now + 100));
    EXPECT_TRUE(state.fail(btc, now + 100));
    EXPECT_FALSE(state.scanPaused());
    EXPECT_FALSE(nextPage(state, 950 * minute, false, now + 2099));
    auto retry = nextPage(state, 950 * minute, false, now + 2100);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->startMs, second->startMs);
    EXPECT_EQ(retry->endMs, second->endMs);
    EXPECT_TRUE(accept(state, *retry, retry->startMs, now + 2100));
}

TEST(CandleBackfillState, SelectionChangeDropsStaleReplyEvenAfterReturningToSamePair) {
    CandleBackfillState state;
    view(state);
    auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport("ETH-USD", 300000, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(nextPage(state, 0, false, now + 100));
    view(state);
    EXPECT_FALSE(state.accept(btc, 300000, request->startMs, request->endMs, 900 * minute, now + 100));
    EXPECT_FALSE(nextPage(state, 950 * minute, false, now + 100)); // wrong tag cannot release flight
    EXPECT_FALSE(accept(state, *request, 0, now + 100));
    auto next = nextPage(state, 950 * minute, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endMs, request->endMs); // stale empty reply did not advance the cursor
}

TEST(CandleBackfillState, TimeframeChangeDropsStaleReply) {
    CandleBackfillState state;
    view(state);
    auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport(btc, 300000, 1000 * minute, 1100 * minute);
    EXPECT_FALSE(accept(state, *request, 900 * minute));
    auto next = nextPage(state, 0, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->timeframeMs, 300000);
}

TEST(CandleBackfillState, InvalidViewportFullCacheAndFutureViewportDoNotRequest) {
    CandleBackfillState state;
    state.setViewport(btc, 60000, 0, 0);
    EXPECT_FALSE(nextPage(state, 0, false, now));
    view(state);
    EXPECT_FALSE(nextPage(state, 950 * minute, true, now));
    view(state, 11000, 11100);
    EXPECT_FALSE(nextPage(state, 0, false, now));
    view(state, 9990, 10010);
    auto request = nextPage(state, 0, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->endMs, 10001 * minute);
}

TEST(CandleBackfillState, SustainedFastPanSendsLeadingAndPeriodicRequests) {
    CandleBackfillState state;
    qint64 oldest = 2000 * minute;
    std::vector<int> sentAt;
    for (int elapsed = 0; elapsed <= 240; elapsed += 20) {
        view(state, 1000 - elapsed, 2000 - elapsed); // movement never pauses for 100ms
        auto request = nextPage(state, oldest, false, now + elapsed);
        if (!request) continue;
        sentAt.push_back(elapsed);
        EXPECT_TRUE(accept(state, *request, request->startMs, now + elapsed));
        oldest = request->startMs;
    }
    EXPECT_EQ(sentAt, (std::vector<int>{0, 100, 200}));
    // Trailing request consumes the final viewport after movement stops.
    EXPECT_EQ(delay(state, oldest, now + 240), 60);
    EXPECT_FALSE(nextPage(state, oldest, false, now + 299));
    EXPECT_TRUE(nextPage(state, oldest, false, now + 300));
}

TEST(CandleBackfillState, SlowReplyPreventsOverlapButDoesNotAddAnotherDebounce) {
    CandleBackfillState state;
    view(state, 1000, 2000);
    auto request = nextPage(state, 2000 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_FALSE(nextPage(state, 2000 * minute, false, now + 500));
    EXPECT_TRUE(accept(state, *request, request->startMs, now + 500));
    EXPECT_TRUE(nextPage(state, request->startMs, false, now + 500));
}

TEST(CandleBackfillState, OneSecondClientUsesInclusiveEndWithoutOverlapAndAcceptsSparseBars) {
    CandleBackfillState state;
    state.setViewport(btc, 1000, 900'000, 1'000'000);
    auto request = nextPage(state, 950'000, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->endMs, 949'000);
    EXPECT_EQ(request->limit, 150); // visible remainder and prefetch fit one page
    // Retained-bar pages can extend earlier than start_time_ms across a gap.
    EXPECT_TRUE(accept(state, *request, 750'000));
    state.setViewport(btc, 1000, 700'000, 800'000);
    auto next = nextPage(state, 750'000, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endMs, 749'000);
}

TEST(CandleBackfillState, CoarsePagesCountOutputBarsAndReconnectRearmsSameViewport) {
    CandleBackfillState state;
    state.setViewport(btc, 300000, 1000 * minute, 2000 * minute);
    auto request = nextPage(state, 2000 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->limit, 350); // native page crosses into prefetch
    state.disconnect();
    EXPECT_FALSE(accept(state, *request, 0));
    EXPECT_TRUE(state.setViewport(btc, 300000, 1000 * minute, 2000 * minute));
    EXPECT_TRUE(nextPage(state, 2000 * minute, false, now));
}


TEST(CandleBackfillState, StationaryViewportCrossesLongGapWithTimedPausesAndNoStorm) {
    for (qint64 tfSec : {1, 60}) {
        CandleBackfillState state;
        const qint64 oldest = 400'000'000;
        state.setViewport(btc, tfSec * 1000, 360'000'000, 440'000'000);
        qint64 clock = now;
        qint64 previousStart = 0;
        int pauses = 0;
        for (int page = 0; page < 8; ++page) {
            const qint64 wait = delay(state, oldest, clock);
            if (wait == CandleBackfillState::kFloorRetryMs) ++pauses;
            if (wait > 0) {
                EXPECT_FALSE(nextPage(state, oldest, false, clock + wait - 1));
                clock += wait; // the RemoteGridDataSource timer uses this deadline
            }
            auto request = nextPage(state, oldest, false, clock);
            ASSERT_TRUE(request) << "page=" << page << " tf=" << tfSec;
            if (previousStart > 0) EXPECT_LT(request->endMs, previousStart + (tfSec == 1 ? 0 : 1000));
            previousStart = request->startMs;
            EXPECT_FALSE(nextPage(state, oldest, false, clock + 500)); // no second flight
            const qint64 replyOldest = page == 7 ? request->startMs : 0;
            ASSERT_TRUE(accept(state, *request, replyOldest, clock));
        }
        EXPECT_EQ(pauses, 2); // eight pages require two full 60s pauses, with no viewport updates
        EXPECT_FALSE(state.scanPaused());
    }
}

TEST(CandleBackfillState, OneSecondEmptyLookbackIsBoundedAcrossPauses) {
    constexpr qint64 budgetSec = 2000;
    CandleBackfillState state(budgetSec * 1000);
    constexpr qint64 oldest = 100'000'000;
    state.setViewport(btc, 1000, 90'000'000, 110'000'000);
    qint64 clock = now;
    qint64 scannedStart = 0;
    int pages = 0;
    while (needs(state, oldest, false, clock) && pages < 20) {
        clock += delay(state, oldest, clock);
        auto request = nextPage(state, oldest, false, clock);
        ASSERT_TRUE(request);
        scannedStart = request->startMs;
        EXPECT_GE(scannedStart, oldest - budgetSec * 1000);
        ASSERT_TRUE(accept(state, *request, 0, clock));
        ++pages;
    }
    EXPECT_EQ(scannedStart, oldest - budgetSec * 1000);
    EXPECT_EQ(pages, 6);
    EXPECT_FALSE(needs(state, oldest, false, clock + 10 * CandleBackfillState::kFloorRetryMs));
    EXPECT_FALSE(nextPage(state, oldest, false, clock + 10 * CandleBackfillState::kFloorRetryMs));
}

TEST(CandleBackfillState, PausedScanDoesNotNeedTimerWhenViewportReturnsToLoadedCoverage) {
    CandleBackfillState state;
    view(state, 4000, 4100);
    for (int i = 0; i < 3; ++i) {
        auto request = nextPage(state, 4050 * minute, false, now + i * 100);
        ASSERT_TRUE(request);
        ASSERT_TRUE(accept(state, *request, 0, now + i * 100));
    }
    EXPECT_EQ(delay(state, 4050 * minute, now + 200), CandleBackfillState::kFloorRetryMs);
    view(state, 4100, 4150); // prefetch now starts at the loaded edge
    EXPECT_FALSE(needs(state, 4050 * minute, false, now + 200));
    EXPECT_FALSE(nextPage(state, 4050 * minute, false, now + 200 + CandleBackfillState::kFloorRetryMs));
}


TEST(CandleBackfillState, ExplicitRefreshBypassesCoveredFullCacheThenStops) {
    CandleBackfillState state;
    view(state);
    EXPECT_FALSE(nextPage(state, 800 * minute, true, now));
    state.requestRefresh();
    auto refresh = nextPage(state, 800 * minute, true, now);
    ASSERT_TRUE(refresh);
    EXPECT_TRUE(refresh->refresh);
    EXPECT_EQ(refresh->endMs, 1100 * minute);
    EXPECT_EQ(refresh->startMs, 900 * minute);
    EXPECT_FALSE(nextPage(state, 800 * minute, true, now + 1000));
    ASSERT_TRUE(accept(state, *refresh, 900 * minute, now + 1000));
    EXPECT_FALSE(nextPage(state, 800 * minute, true, now + 1100));
}

TEST(CandleBackfillState, NativePagesCrossVisibleBoundaryWithoutAnExtraRoundTrip) {
    constexpr qint64 tfSec = 900, end = 2'000'000'700, span = 14 * 86400;
    for (bool refresh : {false, true}) {
        CandleBackfillState state;
        state.setViewport(btc, tfSec * 1000, (end - span) * 1000, end * 1000);
        if (refresh) state.requestRefresh();
        qint64 oldest = refresh ? (end - 2 * span) * 1000 : 0;
        qint64 clock = end * 1000;
        for (int page = 0; page < 4; ++page) {
            auto request = nextPage(state, oldest, refresh, clock);
            ASSERT_TRUE(request);
            EXPECT_EQ(request->limit, 350);
            EXPECT_EQ(request->endMs, (end - page * 350 * tfSec) * 1000);
            EXPECT_EQ(request->startMs, (end - (page + 1) * 350 * tfSec) * 1000);
            ASSERT_TRUE(accept(state, *request, request->startMs, clock));
            oldest = request->startMs;
            clock += 100;
        }
        auto prefetch = nextPage(state, oldest, refresh, clock);
        ASSERT_TRUE(prefetch);
        EXPECT_EQ(prefetch->endMs, (end - 1400 * tfSec) * 1000);
        EXPECT_EQ(prefetch->limit, 350);
    }
}

TEST(CandleBackfillState, UsesFullPagesAcrossVisibleAndPrefetchRanges) {
    for (bool refresh : {false, true}) {
        for (qint64 visibleBars : {100, 175, 176}) {
            CandleBackfillState state;
            view(state, 1000, 1000 + visibleBars);
            if (refresh) state.requestRefresh();
            const auto request = nextPage(state, 0, false, now);
            ASSERT_TRUE(request);
            const qint64 expected = std::min<qint64>(350, visibleBars * 2);
            EXPECT_EQ(request->limit, expected);
            EXPECT_EQ(request->startMs, (1000 + visibleBars - expected) * minute);
        }
    }
}

TEST(CandleBackfillState, RefusedIntervalsNeverPageRefreshOrRetry) {
    for (qint64 tfMs : {0, -1000}) {
        CandleBackfillState state;
        state.setViewport(btc, tfMs, 900'000, 1'000'000);
        EXPECT_FALSE(needs(state, 0, false, now));
        state.requestRefresh();
        EXPECT_FALSE(nextPage(state, 0, false, now));
        EXPECT_FALSE(state.fail(btc, now));
        EXPECT_EQ(delay(state, 0, now), 0);
        EXPECT_FALSE(nextPage(state, 0, false, now + 2000));
    }
}

TEST(CandleBackfillState, OldFlightFailureDoesNotRetryRefusedSelection) {
    for (qint64 tfMs : {0, -1000}) {
        CandleBackfillState state;
        state.setViewport(btc, 1000, 900'000, 1'000'000);
        ASSERT_TRUE(nextPage(state, 0, false, now));
        state.setViewport(btc, tfMs, 900'000, 1'000'000);
        ASSERT_TRUE(state.fail(btc, now + 1));
        EXPECT_EQ(delay(state, 0, now + 1), 0);
        EXPECT_FALSE(nextPage(state, 0, false, now + 2001));
    }
}

class CandleBackfillUnaligned : public testing::TestWithParam<qint64> {};

TEST_P(CandleBackfillUnaligned, OldestBarPagesThroughExactWireReplies) {
    const qint64 tfMs = GetParam();
    CandleBackfillState state;
    state.setViewport(btc, tfMs, 900'000, 1'000'000);
    const auto first = nextPage(state, 949'500, false, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->boundaryMs, tfMs == 1000 ? 949'000 : 900'000);
    EXPECT_EQ(first->endMs, tfMs == 1000 ? 948'000 : 900'000);
    EXPECT_EQ(first->startMs, tfMs == 1000 ? 799'000 : 780'000);
    EXPECT_TRUE(acceptWireReply(state, *first, first->startMs + 500));
    state.setViewport(btc, tfMs, 700'000, 800'000);
    const auto second = nextPage(state, first->startMs + 500, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->boundaryMs, tfMs == 1000 ? 799'000 : 780'000);
    EXPECT_EQ(second->endMs, tfMs == 1000 ? 798'000 : 780'000);
    EXPECT_EQ(second->startMs, tfMs == 1000 ? 599'000 : 600'000);
    EXPECT_TRUE(acceptWireReply(state, *second, second->startMs, now + 100));
    EXPECT_FALSE(nextPage(state, second->startMs, false, now + 200));
}

INSTANTIATE_TEST_SUITE_P(WholeSeconds, CandleBackfillUnaligned, testing::Values(1000LL, 60000LL));

TEST(CandleBackfillState, UnalignedRefreshReplyKeepsNextPageIdentityExact) {
    CandleBackfillState state;
    state.setViewport(btc, 1000, 900'000, 1'000'000);
    state.requestRefresh();
    const auto first = nextPage(state, 949'500, false, now);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->refresh);
    ASSERT_TRUE(acceptWireReply(state, *first, 949'500));
    const auto second = nextPage(state, 949'500, false, now + 100);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->boundaryMs, 949'000);
    EXPECT_EQ(second->endMs, 948'000);
    EXPECT_EQ(second->startMs, 799'000);
    EXPECT_TRUE(acceptWireReply(state, *second, 800'500, now + 100));
    EXPECT_FALSE(nextPage(state, 800'500, false, now + 200));
}

TEST(CandleBackfillState, HundredMillisecondsPagesRetainedBars) {
    CandleBackfillState state;
    state.setViewport(btc, 100, 60000, 61001);
    const auto first = nextPage(state, 0, false, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->endMs, 61000);
    EXPECT_EQ(first->limit, 22);
    EXPECT_TRUE(state.accept(btc, 100, first->startMs, first->endMs, 60000, now));
    const auto next = nextPage(state, 60000, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endMs, 59900);
    EXPECT_EQ(next->limit, 11);
}

// A "warming" reply (the server is still rebuilding 100 ms / 1 s history) is
// not a page: the request is held and resent verbatim after 2 s.
TEST(CandleBackfillState, WarmingHoldsTheRequestAndResendsItVerbatim) {
    CandleBackfillState state;
    view(state);
    const auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    EXPECT_FALSE(state.warming(btc, 60000, request->startMs + 1, request->endMs, now)); // not this request
    EXPECT_TRUE(state.warming(btc, 60000, request->startMs, request->endMs, now));
    EXPECT_FALSE(state.warming(btc, 60000, request->startMs, request->endMs, now)); // held, not in flight
    EXPECT_EQ(delay(state, 950 * minute, now), CandleBackfillState::kWarmingRetryMs);
    EXPECT_FALSE(state.scanPaused());
    EXPECT_TRUE(needs(state, 950 * minute, false, now));
    EXPECT_FALSE(nextPage(state, 950 * minute, false, now + 1999));
    // The viewport moved and live bars arrived meanwhile: the same request.
    view(state, 1001, 1101);
    const auto again = nextPage(state, 960 * minute, false, now + 2000);
    ASSERT_TRUE(again);
    EXPECT_EQ(std::tie(again->symbol, again->timeframeMs, again->startMs, again->endMs, again->limit,
                       again->generation, again->boundaryMs, again->refresh, again->held, again->retryAtMs),
              std::tie(request->symbol, request->timeframeMs, request->startMs, request->endMs, request->limit,
                       request->generation, request->boundaryMs, request->refresh, request->held, request->retryAtMs));
    EXPECT_FALSE(nextPage(state, 960 * minute, false, now + 2200)); // in flight again
    // Repeated warming never counts as an empty page.
    for (int i = 0; i < CandleBackfillState::kMaxEmptyPages + 1; ++i) {
        EXPECT_TRUE(state.warming(btc, 60000, request->startMs, request->endMs, now + 2200 + i * 2000));
        EXPECT_FALSE(state.scanPaused());
        const auto resent = nextPage(state, 960 * minute, false, now + 4200 + i * 2000);
        ASSERT_TRUE(resent);
        EXPECT_EQ(resent->endMs, request->endMs);
    }
    EXPECT_TRUE(accept(state, *request, 920 * minute, now + 20000));
    // The answered window [900, 950) is known; paging goes on once the view needs older bars.
    view(state, 850, 950);
    const auto older = nextPage(state, 920 * minute, false, now + 20100);
    ASSERT_TRUE(older);
    EXPECT_LT(older->endMs, request->endMs);
}

TEST(CandleBackfillState, SelectionChangeDropsTheHeldRequestAndItsDeadline) {
    CandleBackfillState state;
    view(state);
    const auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    ASSERT_TRUE(state.warming(btc, 60000, request->startMs, request->endMs, now));
    state.setViewport("ETH-USD", 60000, 1000 * minute, 1100 * minute);
    EXPECT_EQ(delay(state, 0, now + 100), 0);
    const auto eth = nextPage(state, 0, false, now + 100);
    ASSERT_TRUE(eth);
    EXPECT_EQ(eth->symbol, "ETH-USD");
    EXPECT_TRUE(accept(state, *eth, 0, now + 150));
    // Back to BTC: a fresh request for the same window, no 2 s wait.
    view(state);
    EXPECT_EQ(delay(state, 950 * minute, now + 250), 0);
    const auto fresh = nextPage(state, 950 * minute, false, now + 250);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->endMs, request->endMs);
    EXPECT_GT(fresh->generation, request->generation);
}

// A -> B -> A: a warming reply to the old A request only releases the flight;
// the new A selection pages at once and its history is untouched.
TEST(CandleBackfillState, StaleWarmingReplyReleasesWithoutDelayOrHistoryChange) {
    CandleBackfillState state;
    view(state);
    const auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    state.setViewport("ETH-USD", 300000, 1000 * minute, 1100 * minute);
    view(state);
    EXPECT_FALSE(nextPage(state, 950 * minute, false, now + 100)); // the old request is still in flight
    EXPECT_TRUE(state.warming(btc, 60000, request->startMs, request->endMs, now + 100));
    EXPECT_EQ(delay(state, 950 * minute, now + 100), 0);
    EXPECT_FALSE(state.scanPaused());
    const auto next = nextPage(state, 950 * minute, false, now + 100);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->endMs, request->endMs); // no cursor moved
    EXPECT_FALSE(next->held);
}

TEST(CandleBackfillState, DisconnectDropsTheHeldRequest) {
    CandleBackfillState state;
    view(state);
    const auto request = nextPage(state, 950 * minute, false, now);
    ASSERT_TRUE(request);
    ASSERT_TRUE(state.warming(btc, 60000, request->startMs, request->endMs, now));
    state.disconnect();
    view(state);
    EXPECT_EQ(delay(state, 950 * minute, now + 100), 0);
    const auto fresh = nextPage(state, 950 * minute, false, now + 100);
    ASSERT_TRUE(fresh);
    EXPECT_GT(fresh->generation, request->generation);
}

TEST(CandleCoverage, MergesAdjacentSpansAndReportsTheNewestGap) {
    CandleCoverage coverage;
    coverage.add(100, 200);
    coverage.add(300, 400);
    coverage.add(200, 250); // touches the first span
    ASSERT_EQ(coverage.spanCount(), 2);
    EXPECT_EQ(coverage.span(0).endMs, 250);
    qint64 lo = 0, hi = 0;
    ASSERT_TRUE(coverage.newestGap(0, 500, lo, hi));
    EXPECT_EQ(std::make_pair(lo, hi), (std::pair<qint64, qint64>{400, 500}));
    ASSERT_TRUE(coverage.newestGap(0, 400, lo, hi));
    EXPECT_EQ(std::make_pair(lo, hi), (std::pair<qint64, qint64>{250, 300}));
    ASSERT_TRUE(coverage.newestGap(0, 150, lo, hi));
    EXPECT_EQ(std::make_pair(lo, hi), (std::pair<qint64, qint64>{0, 100}));
    EXPECT_FALSE(coverage.newestGap(120, 240, lo, hi));
    coverage.add(240, 310); // bridges both
    ASSERT_EQ(coverage.spanCount(), 1);
    EXPECT_TRUE(coverage.coversRange(100, 400));
    EXPECT_EQ(coverage.runStart(399), 100);
    EXPECT_EQ(coverage.nextCovered(50), 100);
    EXPECT_EQ(coverage.nextCovered(400), std::numeric_limits<int64_t>::max());
}

TEST(CandleCoverage, AFullSetKeepsANewOlderSpanAndForgetsTheShortestOther) {
    CandleCoverage coverage;
    for (int i = 0; i < CandleCoverage::kMaxSpans; ++i) coverage.add(1000 + i * 100, 1000 + i * 100 + (i == 5 ? 10 : 50));
    coverage.add(100, 200); // older than every span, disjoint
    EXPECT_EQ(coverage.spanCount(), CandleCoverage::kMaxSpans);
    EXPECT_TRUE(coverage.coversRange(100, 200)) << "the page just accepted must stay";
    EXPECT_FALSE(coverage.covers(1505)) << "the shortest span went";
    EXPECT_TRUE(coverage.covers(1005));
}

TEST(CandleCoverage, LiveRunIsOpenUntilClosedAndCapacityDropsTheOldestSpan) {
    CandleCoverage coverage;
    coverage.add(0 + 10, 20);
    coverage.extendLive(50, 60);
    EXPECT_TRUE(coverage.covers(1'000'000));
    EXPECT_FALSE(coverage.covers(30));
    coverage.closeLive();
    EXPECT_FALSE(coverage.covers(60));
    EXPECT_TRUE(coverage.covers(55));
    coverage.clear();
    for (int i = 0; i < CandleCoverage::kMaxSpans + 2; ++i) coverage.add(i * 10, i * 10 + 5);
    EXPECT_EQ(coverage.spanCount(), CandleCoverage::kMaxSpans);
    EXPECT_FALSE(coverage.covers(0)); // the oldest spans went first
    EXPECT_TRUE(coverage.covers((CandleCoverage::kMaxSpans + 1) * 10));
    coverage.trimBefore(100);
    EXPECT_FALSE(coverage.covers(95));
    EXPECT_TRUE(coverage.covers(100));
}

namespace {
// The GUI loop around the policy: a fake server that answers every request with a
// full page, and the buffer's coverage as RemoteGridDataSource records it.
struct Replay {
    struct Series {
        CandleCoverage coverage;
        qint64 oldestBarMs = 0;
        bool full = false;
    };
    CandleBackfillState state;
    std::map<qint64, Series> series;
    qint64 clock = 0;
    qint64 tf = 0;
    std::vector<CandleBackfillState::Request> sent;

    explicit Replay(qint64 nowMs) : clock(nowMs) {}
    qint64 liveEdge() const { return clock / tf * tf; }
    CandleBackfillState::Loaded loaded() {
        auto& s = series[tf];
        return {s.coverage, s.oldestBarMs, s.full};
    }
    // RemoteGridDataSource::setCandleHistoryViewport.
    void select(qint64 timeframeMs, qint64 startMs, qint64 endMs) {
        const bool changed = timeframeMs != tf;
        if (changed && tf > 0) series[tf].coverage.closeLive();
        const bool refresh = changed && tf > 0 && series[timeframeMs].oldestBarMs > 0;
        if (refresh) series[timeframeMs].coverage.closeLive();
        tf = timeframeMs;
        state.setViewport(btc, tf, startMs, endMs);
        if (refresh) state.requestRefresh();
    }
    // The stream delivers the open bucket of the selected interval.
    void liveBar() {
        auto& s = series[tf];
        s.coverage.extendLive(liveEdge(), liveEdge() + tf);
        if (s.oldestBarMs == 0 || liveEdge() < s.oldestBarMs) s.oldestBarMs = liveEdge();
    }
    void answer(const CandleBackfillState::Request& request) {
        const bool memory = chart::candleHistoryFromMemory(request.timeframeMs);
        // REST: every closed bucket of the window. Memory: the newest `limit` buckets up to the inclusive end.
        const qint64 newest = std::min(memory ? request.endMs : request.endMs - tf, liveEdge() - tf);
        const qint64 oldest = memory ? request.endMs - (request.limit - 1) * tf : request.startMs;
        const int count = newest >= oldest ? static_cast<int>((newest - oldest) / tf + 1) : 0;
        ASSERT_TRUE(state.accept(request.symbol, request.timeframeMs, request.startMs, request.endMs,
                                 count > 0 ? oldest : 0, clock));
        qint64 coverStart = 0, coverEnd = 0;
        auto& s = series[tf];
        if (state.acceptedCoverage(count > 0 ? oldest : 0, coverStart, coverEnd))
            s.coverage.add(coverStart, coverEnd);
        if (count > 0) {
            s.coverage.add(oldest, newest + tf);
            if (s.oldestBarMs == 0 || oldest < s.oldestBarMs) s.oldestBarMs = oldest;
        }
    }
    // Pages until the policy is idle; the timer waits out throttle and pauses.
    void run(int maxPages = 200) {
        for (int pages = 0; pages < maxPages; ++pages) {
            const qint64 wait = state.retryDelayMs(loaded(), clock);
            if (!state.needsHistory(loaded(), clock)) return;
            clock += wait;
            const auto request = state.next(loaded(), clock);
            if (!request) return;
            sent.push_back(*request);
            clock += 50; // round trip; the stream keeps delivering meanwhile
            liveBar();
            answer(*request);
        }
        FAIL() << "paging did not settle";
    }
    // The newest fetched time of the selection that the policy left unrequested.
    bool contiguousToLiveEdge() {
        const auto& s = series[tf];
        return s.oldestBarMs > 0 && s.coverage.coversRange(s.oldestBarMs, liveEdge());
    }
};
constexpr qint64 hour = 60 * minute, day = 24 * hour;
} // namespace

// An empty series opened at a past viewport also loads the span up to the live edge.
TEST(CandleBackfillState, EmptyPastViewportRequestsUpToTheLiveEdge) {
    for (const qint64 tfMs : {qint64(1000), minute, 5 * minute}) {
        const qint64 nowMs = 1'792'640'500'000;
        const qint64 viewEnd = nowMs - std::min<qint64>(2 * day, 200 * tfMs);
        Replay replay(nowMs);
        replay.select(tfMs, nowMs - std::min<qint64>(3 * day, 300 * tfMs), viewEnd);
        replay.run();
        ASSERT_FALSE(replay.sent.empty());
        // The visible window comes first; then the span above it, newest first.
        EXPECT_LE(replay.sent.front().endMs, viewEnd + tfMs) << "tf=" << tfMs;
        replay.liveBar();
        replay.run();
        EXPECT_TRUE(replay.series[tfMs].coverage.coversRange(viewEnd, nowMs / tfMs * tfMs)) << "tf=" << tfMs;
        EXPECT_TRUE(replay.contiguousToLiveEdge()) << "tf=" << tfMs;
    }
}

// Coverage [A, now) exists; a return to a viewport ending at B < A fills [B, A).
TEST(CandleBackfillState, ReturnToAnOlderViewportFillsTheSpanUpToLoadedBars) {
    const qint64 nowMs = 1'792'640'400'000 + 20'000, tf = 5 * minute; // A and B on bucket starts, as bars are
    const qint64 a = nowMs / tf * tf - 2 * day, b = nowMs / tf * tf - 4 * day;
    CandleBackfillState state;
    CandleCoverage coverage;
    coverage.add(a, nowMs / tf * tf);
    coverage.extendLive(nowMs / tf * tf, nowMs / tf * tf + tf);
    state.setViewport(btc, minute, nowMs - hour, nowMs); // another interval first
    state.setViewport(btc, tf, b - 6 * hour, b);
    state.requestRefresh();
    qint64 clock = nowMs;
    std::vector<std::pair<qint64, qint64>> requested;
    for (int page = 0; page < 50; ++page) {
        clock += state.retryDelayMs({coverage, a, false}, clock);
        const auto request = state.next({coverage, a, false}, clock);
        if (!request) break;
        requested.emplace_back(request->startMs, request->endMs);
        ASSERT_TRUE(state.accept(btc, tf, request->startMs, request->endMs, request->startMs, clock));
        qint64 start = 0, end = 0;
        ASSERT_TRUE(state.acceptedCoverage(request->startMs, start, end));
        coverage.add(start, end);
    }
    ASSERT_FALSE(requested.empty());
    EXPECT_TRUE(coverage.coversRange(b - 12 * hour, nowMs / tf * tf)) << "the hole [B, A) stays unrequested";
    EXPECT_EQ(coverage.spanCount(), 1);
    EXPECT_FALSE(state.needsHistory({coverage, b - 12 * hour, false}, clock));
}

// The owner's 2026-10-21 switch sequence (GUI log 20261021-234142): each interval,
// when left, holds one covered run from its oldest bar to the live edge.
TEST(CandleBackfillState, OwnerTimeframeSequenceLeavesNoHoles) {
    const qint64 start = 1'792'640'500'000; // 22 Oct 03:41:40Z
    Replay replay(start);
    struct Visit {
        qint64 tf, endMs, span;
        std::vector<qint64> panEnds;
    };
    const qint64 now = start;
    const std::vector<Visit> visits = {
        {minute, now, hour, {}},
        {5 * minute, 1'792'534'800'000, 6 * hour, {}},                     // 20 Oct 22:20Z
        {15 * minute, now, day, {now - day, now - 2 * day, now - 4 * day}}, // panned back
        {5 * minute, 1'792'279'500'000, 6 * hour, {}},                     // 17 Oct 23:25Z
        {hour, 1'792'486'800'000, 2 * day, {}},                            // 20 Oct 09:00Z
        {4 * hour, now, 20 * day, {}},
        {day, now, 120 * day, {}},
        {hour, now, 2 * day, {}},
    };
    for (const auto& visit : visits) {
        replay.clock += 47'000; // the owner looks before switching
        replay.select(visit.tf, visit.endMs - visit.span, visit.endMs);
        replay.run();
        for (const qint64 end : visit.panEnds) {
            replay.clock += 3'000;
            replay.state.setViewport(btc, visit.tf, end - visit.span, end);
            replay.run();
        }
        replay.liveBar();
        EXPECT_TRUE(replay.contiguousToLiveEdge())
            << "tf=" << visit.tf << " oldest=" << replay.series[visit.tf].oldestBarMs;
    }
}

// 100 ms / 1 s history returns retained bars only: it proves nothing before its
// oldest bar (a restart or retention hole is not quiet time), and an answered
// window is still not asked again in this selection.
TEST(CandleBackfillState, MemoryRepliesProveOnlyFromTheirOldestBarAndAreNotAskedAgain) {
    CandleBackfillState state;
    constexpr qint64 tf = 1000, nowMs = 2'000'000'000;
    CandleCoverage coverage;
    coverage.add(nowMs - 1000 * tf, nowMs - 600 * tf);
    coverage.extendLive(nowMs - 10 * tf, nowMs - 9 * tf);
    state.setViewport(btc, tf, nowMs - 300 * tf, nowMs - 200 * tf);
    qint64 clock = nowMs;
    std::vector<qint64> ends;
    for (int page = 0; page < 20; ++page) {
        const CandleBackfillState::Loaded loaded{coverage, nowMs - 1000 * tf, false};
        clock += state.retryDelayMs(loaded, clock);
        const auto request = state.next(loaded, clock);
        if (!request) break;
        ends.push_back(request->endMs);
        // The first page holds one retained bar in the middle of its window; the rest hold none.
        const qint64 oldest = page == 0 ? request->endMs - 20 * tf : 0;
        ASSERT_TRUE(state.accept(btc, tf, request->startMs, request->endMs, oldest, clock));
        qint64 start = 0, end = 0;
        if (page == 0) {
            ASSERT_TRUE(state.acceptedCoverage(oldest, start, end));
            EXPECT_EQ(start, oldest) << "a short page does not prove its window before its oldest bar";
            coverage.add(start, end);
        } else {
            EXPECT_FALSE(state.acceptedCoverage(0, start, end)) << "an empty page proves nothing";
        }
    }
    ASSERT_FALSE(ends.empty());
    EXPECT_LT(ends.size(), 20u) << "paging settles";
    std::sort(ends.begin(), ends.end());
    EXPECT_EQ(std::adjacent_find(ends.begin(), ends.end()), ends.end()) << "no window is asked twice";
    EXPECT_FALSE(coverage.covers(nowMs - 400 * tf)) << "the unproven span stays a hole";
}
