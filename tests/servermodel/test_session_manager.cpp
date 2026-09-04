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

TEST(SessionManagerTests, WeeklySessionBeforeSundayOpenUsesPreviousWeek) {
    const auto previousOpen = utcMs(2026, 8, 30, 21);
    const auto previousClose = utcMs(2026, 9, 4, 21);

    expectBoundary(SessionManager::SessionType::W1,
                   utcMs(2026, 9, 6, 20, 59), previousOpen, previousClose);
}

TEST(SessionManagerTests, WeeklySessionRollsAtSundayOpen) {
    const auto currentOpen = utcMs(2026, 9, 6, 21);
    const auto currentClose = utcMs(2026, 9, 11, 21);

    expectBoundary(SessionManager::SessionType::W1,
                   utcMs(2026, 9, 6, 21), currentOpen, currentClose);
    expectBoundary(SessionManager::SessionType::W1,
                   utcMs(2026, 9, 12, 12), currentOpen, currentClose);
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
