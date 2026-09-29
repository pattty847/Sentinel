#include <gtest/gtest.h>

#include "servermodel/SessionManager.hpp"

#include <chrono>
#include <cstdint>

namespace {

int64_t utcMs(int yearValue, unsigned monthValue, unsigned dayValue,
              int hourValue, int minuteValue = 0) {
    using namespace std::chrono;
    const auto instant = sys_days{year{yearValue} / month{monthValue} / day{dayValue}}
                         + hours{hourValue} + minutes{minuteValue};
    return duration_cast<milliseconds>(instant.time_since_epoch()).count();
}

void expectBoundary(SessionManager::SessionType type, int64_t queryMs,
                    int64_t expectedStartMs, int64_t expectedEndMs) {
    const auto boundary = SessionManager::sessionContaining(queryMs, type);
    EXPECT_TRUE(boundary.valid);
    EXPECT_EQ(boundary.startMs, expectedStartMs);
    EXPECT_EQ(boundary.endMs, expectedEndMs);
    EXPECT_LE(boundary.startMs, queryMs);
}

} // namespace

TEST(SessionManagerTests, WeeklySessionIsSevenDaysFromMondayMidnightUtc) {
    // 2026-09-07 is a Monday.
    const auto open = utcMs(2026, 9, 7, 0);
    const auto close = utcMs(2026, 9, 14, 0);
    expectBoundary(SessionManager::SessionType::W1, utcMs(2026, 9, 7, 0), open, close);
    expectBoundary(SessionManager::SessionType::W1, utcMs(2026, 9, 11, 21), open, close);
    // Weekend trades belong to the same week.
    expectBoundary(SessionManager::SessionType::W1, utcMs(2026, 9, 12, 12), open, close);
    expectBoundary(SessionManager::SessionType::W1, utcMs(2026, 9, 13, 23, 59), open, close);
}

TEST(SessionManagerTests, WeeklySessionBeforeMondayUsesPreviousWeek) {
    expectBoundary(SessionManager::SessionType::W1, utcMs(2026, 9, 6, 23, 59),
                   utcMs(2026, 8, 31, 0), utcMs(2026, 9, 7, 0));
}

TEST(SessionManagerTests, AustraliaSessionRollsAtTwentyTwoUtc) {
    const auto previousOpen = utcMs(2026, 9, 5, 22);
    const auto previousClose = utcMs(2026, 9, 6, 7);
    const auto currentOpen = utcMs(2026, 9, 6, 22);
    const auto currentClose = utcMs(2026, 9, 7, 7);

    expectBoundary(SessionManager::SessionType::Australia,
                   utcMs(2026, 9, 6, 21, 59), previousOpen, previousClose);
    expectBoundary(SessionManager::SessionType::Australia,
                   utcMs(2026, 9, 6, 22), currentOpen, currentClose);
    expectBoundary(SessionManager::SessionType::Australia,
                   utcMs(2026, 9, 6, 23), currentOpen, currentClose);
    expectBoundary(SessionManager::SessionType::Australia,
                   utcMs(2026, 9, 7, 6, 59), currentOpen, currentClose);
}

TEST(SessionManagerTests, DailySessionsBeforeOpenUsePreviousDay) {
    expectBoundary(SessionManager::SessionType::NY,
                   utcMs(2026, 9, 6, 12, 59),
                   utcMs(2026, 9, 5, 13), utcMs(2026, 9, 5, 22));
    expectBoundary(SessionManager::SessionType::London,
                   utcMs(2026, 9, 6, 7, 59),
                   utcMs(2026, 9, 5, 8), utcMs(2026, 9, 5, 16));
    expectBoundary(SessionManager::SessionType::Asia,
                   utcMs(2026, 9, 6, 0),
                   utcMs(2026, 9, 6, 0), utcMs(2026, 9, 6, 9));
    expectBoundary(SessionManager::SessionType::H24,
                   utcMs(2026, 9, 6, 0),
                   utcMs(2026, 9, 6, 0), utcMs(2026, 9, 7, 0));
}

TEST(SessionManagerTests, AlignAndDurationMatchResolvedBoundary) {
    const auto query = utcMs(2026, 9, 6, 20, 59);
    const auto boundary = SessionManager::sessionContaining(query,
                                                             SessionManager::SessionType::W1);

    EXPECT_EQ(SessionManager::alignToSessionOpen(query, SessionManager::SessionType::W1),
              boundary.startMs);
    EXPECT_EQ(SessionManager::sessionDurationMs(SessionManager::SessionType::W1),
              boundary.endMs - boundary.startMs);
    EXPECT_EQ(SessionManager::sessionDurationMs(SessionManager::SessionType::Australia),
              9LL * 60LL * 60LL * 1000LL);
}

TEST(SessionManagerTests, MonthlySessionIsTheCalendarMonthInUtc) {
    using SessionManager::SessionType;
    expectBoundary(SessionType::M1, utcMs(2026, 9, 29, 23, 59),
                   utcMs(2026, 9, 1, 0), utcMs(2026, 10, 1, 0));
    expectBoundary(SessionType::M1, utcMs(2026, 9, 1, 0),
                   utcMs(2026, 9, 1, 0), utcMs(2026, 10, 1, 0));
    // December rolls into the next year; February follows leap years.
    expectBoundary(SessionType::M1, utcMs(2026, 12, 31, 23, 59),
                   utcMs(2026, 12, 1, 0), utcMs(2027, 1, 1, 0));
    expectBoundary(SessionType::M1, utcMs(2028, 2, 15, 12),
                   utcMs(2028, 2, 1, 0), utcMs(2028, 3, 1, 0));
    expectBoundary(SessionType::M1, utcMs(2027, 2, 28, 23, 59),
                   utcMs(2027, 2, 1, 0), utcMs(2027, 3, 1, 0));
    // The nominal duration is the longest month, so period budgets stay conservative.
    EXPECT_EQ(SessionManager::sessionDurationMs(SessionType::M1), 31LL * 86400000LL);
    for (int m = 1; m <= 12; ++m) {
        const auto b = SessionManager::sessionContaining(utcMs(2026, m, 10, 0), SessionType::M1);
        EXPECT_EQ((b.endMs - b.startMs) % 86400000LL, 0) << "month " << m;
        EXPECT_LE(b.endMs - b.startMs, SessionManager::sessionDurationMs(SessionType::M1));
    }
}
