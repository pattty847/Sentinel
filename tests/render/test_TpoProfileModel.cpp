#include <gtest/gtest.h>

#include "render/TpoProfileModel.hpp"
#include "render/TpoHistoryPager.hpp"
#include "render/TpoStreamState.hpp"
#include "servermodel/SessionManager.hpp"

#include <QByteArray>

using namespace tpo;

namespace {
constexpr int64_t kMin = 60'000;
constexpr int64_t kDay = 86'400'000;
// 2026-09-29 00:00 UTC (a Tuesday).
constexpr int64_t kDayStart = 20'725LL * kDay;
} // namespace

TEST(TpoProfileModel, LettersRunUpperThenLowerThenRepeat) {
    EXPECT_EQ(letterForPeriod(0), 'A');
    EXPECT_EQ(letterForPeriod(25), 'Z');
    EXPECT_EQ(letterForPeriod(26), 'a');
    EXPECT_EQ(letterForPeriod(51), 'z');
    EXPECT_EQ(letterForPeriod(52), 'A');
}

TEST(TpoProfileModel, RowGroupIsSmallestOneTwoFiveStepThatReachesTarget) {
    std::vector<int> seq{1};
    while (seq.size() < 10) seq.push_back(nextRowGroup(seq.back()));
    EXPECT_EQ(seq, (std::vector<int>{1, 2, 5, 10, 20, 25, 50, 100, 200, 250}));
    EXPECT_EQ(rowGroupFor(20.0, 14.0), 1);
    EXPECT_EQ(rowGroupFor(8.0, 14.0), 2);
    EXPECT_EQ(rowGroupFor(3.0, 14.0), 5);
    EXPECT_EQ(rowGroupFor(1.0, 14.0), 20);
    EXPECT_EQ(rowGroupFor(0.1, 14.0), 200);
    EXPECT_EQ(rowGroupFor(0.6, 14.0), 25);
    EXPECT_EQ(rowGroupFor(0.0, 14.0), 1);
}

TEST(TpoProfileModel, ValueAreaStartsAtPocAndAddsLargerPairs) {
    // Rows ascending in price. POC = row 3 (count 6). Total 20, 70% = 14.
    const std::vector<int> counts{1, 2, 3, 6, 4, 2, 1, 1};
    const auto va = computeValueArea(counts);
    ASSERT_TRUE(va.valid());
    EXPECT_EQ(va.poc, 3);
    // Up pair (4+2=6) beats down pair (3+2=5): add rows 4,5 -> 12. Then down pair
    // (3+2=5) beats up (1+1=2): add rows 2,1 -> 17 >= 14.
    EXPECT_EQ(va.high, 5);
    EXPECT_EQ(va.low, 1);
    EXPECT_FALSE(va.contains(0));
    EXPECT_FALSE(va.contains(6));
}

TEST(TpoProfileModel, PocTieResolvesToProfileCentre) {
    const std::vector<int> counts{5, 1, 5, 1, 5};
    EXPECT_EQ(computeValueArea(counts).poc, 2);
    EXPECT_FALSE(computeValueArea(std::vector<int>{0, 0}).valid());
}

TEST(TpoProfileModel, ProfileRowsGroupBaseRowsOnAbsolutePriceAndCountPeriodsOnce) {
    // topAbs = 100: base row i covers absolute low index 99 - i.
    std::vector<std::vector<int>> rowsByPeriod(3);
    rowsByPeriod[0] = {0, 1, 2};   // L = 99, 98, 97
    rowsByPeriod[1] = {1, 2, 3};   // L = 98, 97, 96
    rowsByPeriod[2] = {3};         // L = 96
    ProfileRows profile;
    profile.build(rowsByPeriod, 100, 1);
    ASSERT_EQ(profile.rows(), 4);
    EXPECT_EQ(profile.minGroup(), 96);
    EXPECT_EQ(profile.count(0), 2);  // L=96: periods 1, 2
    EXPECT_EQ(profile.count(3), 1);  // L=99: period 0
    EXPECT_EQ(profile.totalTpos(), 7);
    EXPECT_TRUE(profile.has(0, 2));
    EXPECT_FALSE(profile.has(3, 1));

    // Group of 2: G = floor(L/2) -> 48 {96,97}, 49 {98,99}. Each period counts once per row.
    profile.build(rowsByPeriod, 100, 2);
    ASSERT_EQ(profile.rows(), 2);
    EXPECT_EQ(profile.minGroup(), 48);
    EXPECT_EQ(profile.count(0), 3);  // periods 0 (97), 1 (96,97), 2 (96)
    EXPECT_EQ(profile.count(1), 2);  // periods 0, 1
    std::vector<int> order;
    profile.forEachPeriod(0, [&](int p) { order.push_back(p); });
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(profile.valueArea().poc, 0);
}

TEST(TpoProfileModel, ProfileRowsHandleMoreThanSixtyFourPeriods) {
    std::vector<std::vector<int>> rowsByPeriod(200);
    rowsByPeriod[0] = {5};
    rowsByPeriod[130] = {5};
    rowsByPeriod[199] = {5, 6};
    ProfileRows profile;
    profile.build(rowsByPeriod, 50, 1);
    ASSERT_EQ(profile.rows(), 2);
    std::vector<int> order;
    profile.forEachPeriod(1, [&](int p) { order.push_back(p); });  // L = 44
    EXPECT_EQ(order, (std::vector<int>{0, 130, 199}));
    EXPECT_EQ(profile.maxCount(), 3);
}

TEST(TpoProfileModel, PalettesWalkTheSessionAndDimTails) {
    const auto first = cellColor(Theme::Rainbow, 0, 48, true, false);
    const auto last = cellColor(Theme::Rainbow, 47, 48, true, false);
    EXPECT_GT(first.r, first.b);  // red start
    EXPECT_GT(last.b, last.g);    // violet end
    for (Theme theme : {Theme::Rainbow, Theme::Calm, Theme::Sage}) {
        const auto va = cellColor(theme, 10, 48, true, false);
        const auto tail = cellColor(theme, 10, 48, false, false);
        EXPECT_GT(va.r + va.g + va.b, tail.r + tail.g + tail.b) << themeName(theme);
        EXPECT_TRUE(prefersDarkText(cellColor(theme, 10, 48, true, true))) << themeName(theme);
        EXPECT_EQ(parseTheme(themeName(theme), Theme::Calm), theme);
    }
    EXPECT_EQ(parseLayout("split", Layout::Collapsed), Layout::Split);
    EXPECT_EQ(parseLayout("bogus", Layout::Collapsed), Layout::Collapsed);
}

TEST(TpoProfileModel, PeriodResolutionKeepsServerBudgets) {
    const int h24 = parseSessionType("h24", -1);
    const int ny = parseSessionType("ny", -1);
    const int m1 = parseSessionType("m1", -1);
    EXPECT_EQ(h24, 4);
    EXPECT_EQ(m1, static_cast<int>(SessionManager::SessionType::M1));
    EXPECT_EQ(resolvePeriodMs(h24, 30 * kMin), 30 * kMin);
    EXPECT_EQ(resolvePeriodMs(h24, 1440 * kMin), 1440 * kMin);
    // NY is 9 hours: 2h does not divide it, fall back to 1h.
    EXPECT_EQ(resolvePeriodMs(ny, 120 * kMin), 60 * kMin);
    // A month of 15m periods exceeds 2048 columns: fall back to the smallest valid bracket.
    EXPECT_EQ(resolvePeriodMs(m1, 15 * kMin), 30 * kMin);
    EXPECT_EQ(resolvePeriodMs(m1, 7 * kMin), 30 * kMin);
    EXPECT_EQ(resolvePeriodMs(m1, 1440 * kMin), 1440 * kMin);
}

TEST(TpoProfileModel, HistoryPagesCoverCurrentAndWholePreviousSessionsNewestFirst) {
    const int h24 = 4;
    const int64_t now = kDayStart + 5 * 3600'000 + 12 * kMin;  // 05:12 UTC
    auto pages = historyPages(h24, 30 * kMin, now, 3, 6);
    ASSERT_EQ(pages.size(), 3u);
    EXPECT_EQ(pages[0].endMs, kDayStart + 5 * 3600'000);  // completed periods only
    EXPECT_EQ(pages[0].count, 10);
    EXPECT_EQ(pages[1].endMs, kDayStart);
    EXPECT_EQ(pages[1].count, 48);
    EXPECT_EQ(pages[2].endMs, kDayStart - kDay);

    // A month at 30m is paged in <= 7-day windows (336 periods each).
    const int m1 = static_cast<int>(SessionManager::SessionType::M1);
    const auto month = SessionManager::sessionContaining(now, SessionManager::SessionType::M1);
    EXPECT_EQ(historyPageBudget(m1, 30 * kMin, 1), 5);  // 31 days / 7 days
    EXPECT_EQ(historyPageBudget(m1, 30 * kMin, 3), 15);
    EXPECT_EQ(historyPageBudget(m1, 15 * kMin, 8), 48);
    EXPECT_EQ(historyPageBudget(m1, kMin, 2), kMaxHistoryPages);
    EXPECT_EQ(historyPageBudget(h24, 30 * kMin, 5), 5);
    pages = historyPages(m1, 30 * kMin, now, 3, historyPageBudget(m1, 30 * kMin, 3));
    ASSERT_FALSE(pages.empty());
    int64_t covered = 0;
    int64_t oldestStart = pages.front().endMs;
    for (const auto& page : pages) {
        EXPECT_LE(page.count * 30 * kMin, 7 * kDay);
        EXPECT_LE(page.count, 512);
        covered += page.count * 30 * kMin;
        oldestStart = std::min(oldestStart, page.endMs - page.count * 30 * kMin);
    }
    // All three months are covered end to end: the current one up to now, then two whole months.
    const auto previous = SessionManager::sessionContaining(month.startMs - 1, SessionManager::SessionType::M1);
    const auto third = SessionManager::sessionContaining(previous.startMs - 1, SessionManager::SessionType::M1);
    EXPECT_EQ(oldestStart, third.startMs);
    EXPECT_EQ(covered, pages.front().endMs - third.startMs);
}

TEST(TpoHistoryPager, PacesOnePageAtATimeAndDedupesTheSameSelection) {
    HistoryPager pager;
    const HistoryPager::Selection sel{"BTC-USD", 30 * kMin, 4, 3};
    const std::vector<HistoryPage> pages{{3000, 10}, {2000, 48}, {1000, 48}};
    auto first = pager.start(sel, pages, 0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->endMs, 3000);
    EXPECT_EQ(first->requestId, "tpo-1-0");
    EXPECT_EQ(pager.pending(), 2);
    // Re-enabling the layer with the same selection does not restart or add requests.
    EXPECT_FALSE(pager.start(sel, pages, 10));
    EXPECT_EQ(pager.generation(), 1u);
    // A reply for another id (stale) does not advance; the matching one does.
    EXPECT_FALSE(pager.onChunk("BTC-USD", "tpo-0-3", 30 * kMin, 4, 20));
    auto second = pager.onChunk("BTC-USD", "tpo-1-0", 30 * kMin, 4, 20);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->endMs, 2000);
    // An error skips the page; a timeout abandons a silent one.
    auto third = pager.onError(second->requestId, 30);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->endMs, 1000);
    EXPECT_FALSE(pager.onTick(30 + HistoryPager::kTimeoutMs - 1));
    EXPECT_FALSE(pager.onTick(30 + HistoryPager::kTimeoutMs));  // queue empty: nothing next
    EXPECT_FALSE(pager.busy());
    EXPECT_EQ(pager.failures(), 2);
}

TEST(TpoHistoryPager, NewSelectionStartsNewGenerationCancelsTheOldPageAndRequiresIds) {
    HistoryPager pager;
    auto a = pager.start({"BTC-USD", 30 * kMin, 4, 3}, {{3000, 10}, {2000, 48}}, 0);
    ASSERT_TRUE(a);
    EXPECT_TRUE(pager.takeAbandoned().empty());
    auto b = pager.start({"ETH-USD", 30 * kMin, 5, 3}, {{9000, 336}}, 5);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->requestId, "tpo-2-0");
    // The superseded page is reported (with its own symbol) so the server job is cancelled.
    const auto abandoned = pager.takeAbandoned();
    ASSERT_EQ(abandoned.size(), 1u);
    EXPECT_EQ(abandoned[0].requestId, a->requestId);
    EXPECT_EQ(abandoned[0].symbol, "BTC-USD");
    EXPECT_FALSE(pager.onChunk("BTC-USD", a->requestId, 30 * kMin, 4, 6));
    // No id, or the right id for the wrong selection, never completes a page.
    EXPECT_FALSE(pager.onChunk("ETH-USD", "", 30 * kMin, 5, 7));
    EXPECT_FALSE(pager.onChunk("ETH-USD", b->requestId, 30 * kMin, 4, 7));
    EXPECT_TRUE(pager.busy());
    EXPECT_FALSE(pager.onChunk("ETH-USD", b->requestId, 30 * kMin, 5, 8));  // last page: nothing next
    EXPECT_FALSE(pager.busy());
    // A timed-out page is also reported for cancellation.
    pager.start({"ETH-USD", 30 * kMin, 5, 3}, {{9000, 336}}, 9);
    EXPECT_FALSE(pager.onTick(9 + HistoryPager::kTimeoutMs));
    ASSERT_EQ(pager.takeAbandoned().size(), 1u);
    pager.cancel();
    EXPECT_FALSE(pager.busy());
}

TEST(TpoStreamStateSessions, KeepsRecentSessionsAndTagsUploads) {
    TpoStreamState state;
    state.setSessionType(4);
    state.setMaxSessions(2);
    const QByteArray col(8, 'A');
    ASSERT_TRUE(state.ingestSlice(kDayStart - kDay, kDayStart - kDay + 30 * kMin, 30 * kMin, 48, 8, col));
    ASSERT_TRUE(state.ingestSlice(kDayStart + 60 * kMin, kDayStart + 90 * kMin, 30 * kMin, 48, 8, col));
    std::vector<TpoStreamState::PendingUpload> uploads;
    state.takePendingUploads(uploads);
    ASSERT_EQ(uploads.size(), 2u);
    EXPECT_EQ(uploads[1].sessionStartMs, kDayStart);
    EXPECT_EQ(uploads[1].x, 2);
    EXPECT_EQ(uploads[1].periods, 48);
    EXPECT_EQ(state.snapshot().sessions, 2);
    // A third, newer session evicts the oldest; an older one is refused while full.
    ASSERT_TRUE(state.ingestSlice(kDayStart + kDay, kDayStart + kDay + 30 * kMin, 30 * kMin, 48, 8, col));
    EXPECT_EQ(state.snapshot().sessions, 2);
    EXPECT_FALSE(state.ingestSlice(kDayStart - kDay, kDayStart - kDay + 30 * kMin, 30 * kMin, 48, 8, col));
    // Identical data is not re-uploaded; an empty refresh never erases rows.
    state.takePendingUploads(uploads);
    EXPECT_TRUE(state.ingestSlice(kDayStart + kDay, kDayStart + kDay + 30 * kMin, 30 * kMin, 48, 8, col));
    EXPECT_FALSE(state.ingestSlice(kDayStart + kDay, kDayStart + kDay + 30 * kMin, 30 * kMin, 48, 8, QByteArray(8, '\0')));
    state.takePendingUploads(uploads);
    EXPECT_TRUE(uploads.empty());
}
