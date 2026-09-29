/*
 * Sentinel – SessionManager
 * Pure UTC-based session boundary math. Header-only, no dependencies beyond <cstdint>.
 *
 * All times are UTC epoch milliseconds.  Exchange-local offsets are baked into the
 * kSessionOffsetMs / kSessionDurationMs tables – callers never need tz conversion.
 *
 * Session coverage (UTC, daily unless noted):
 *   NY         08:00–17:00 EST  → 13:00–22:00 UTC  (standard time, EST = UTC-5)
 *   London     08:00–16:00 GMT  → 08:00–16:00 UTC
 *   Asia       00:00–09:00 UTC  (Tokyo/Singapore core; SGT = UTC+8, opens 08:00 local)
 *   Australia  22:00–07:00 UTC  (Sydney core; AEDT ≈ UTC+11, opens 09:00 local)
 *   H24        00:00–23:59 UTC  rolling daily
 *   W1         Monday 00:00 UTC, 7 days (crypto trades through the weekend)
 *   M1         calendar month, 1st 00:00 UTC – 1st of next month 00:00 UTC
 */
#pragma once
#include <cstdint>

namespace SessionManager {

// ─── Session types ─────────────────────────────────────────────────────────
enum class SessionType : int {
    NY        = 0,
    London    = 1,
    Asia      = 2,
    Australia = 3,
    H24       = 4,
    W1        = 5,
    M1        = 6,
};

// ─── Session boundary ──────────────────────────────────────────────────────
struct SessionBoundary {
    int64_t startMs = 0;  // UTC epoch ms of session open
    int64_t endMs   = 0;  // UTC epoch ms of session close (exclusive)
    bool    valid   = false;
};

// ─── Internal constants ────────────────────────────────────────────────────
namespace detail {

// Offset from UTC midnight to session open, expressed in milliseconds.
static constexpr int64_t kMsPerDay  = 86'400'000LL;
static constexpr int64_t kMsPerWeek = 7LL * kMsPerDay;
static constexpr int64_t kMaxMonthMs = 31LL * kMsPerDay;

// [SessionType index] → {openOffsetMs, durationMs}
// Australia: open at 22:00 UTC previous day → openOffset = 22 * 3600000
struct SessionSpec {
    int64_t openOffsetMs;  // from UTC midnight
    int64_t durationMs;
};

static constexpr SessionSpec kSpecs[] = {
    // NY:        13:00 UTC, 9 h
    { 13LL * 3600'000LL, 9LL  * 3600'000LL },
    // London:    08:00 UTC, 8 h
    {  8LL * 3600'000LL, 8LL  * 3600'000LL },
    // Asia:      00:00 UTC, 9 h
    {  0LL * 3600'000LL, 9LL  * 3600'000LL },
    // Australia: 22:00 UTC, 9 h (closes on the following UTC day)
    { 22LL * 3600'000LL, 9LL  * 3600'000LL },
    // H24:       00:00 UTC, 24 h
    {  0LL * 3600'000LL, 24LL * 3600'000LL },
};

// Milliseconds from UTC epoch to the most recent UTC midnight on or before t.
inline int64_t utcMidnightBefore(int64_t epochMs) {
    // Guard against negative epoch (before 1970) – clamp to 0.
    if (epochMs < 0) return 0;
    return (epochMs / kMsPerDay) * kMsPerDay;
}

// Return the UTC day-of-week for epochMs: 0 = Sunday … 6 = Saturday.
// 1970-01-01 was a Thursday (4).
inline int utcWeekday(int64_t epochMs) {
    if (epochMs < 0) epochMs = 0;
    const int64_t days = epochMs / kMsPerDay;
    return static_cast<int>((days + 4) % 7);
}

// Proleptic Gregorian calendar conversions (H. Hinnant's civil algorithms).
// daysFromCivil(1970, 1, 1) == 0.
inline int64_t daysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline void civilFromDays(int64_t z, int64_t& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

} // namespace detail

// ─── Primary API ───────────────────────────────────────────────────────────

/*
 * sessionContaining(epochMs, type)
 *
 * Returns the session window [startMs, endMs) that either contains epochMs
 * or is the most-recent completed session before epochMs.
 *
 * For W1: returns the Monday 00:00 UTC week containing epochMs.
 * For M1: returns the UTC calendar month containing epochMs.
 * For all others: returns the daily session whose open is latest at or before epochMs.
 */
inline SessionBoundary sessionContaining(int64_t epochMs, SessionType type) {
    using namespace detail;

    if (type == SessionType::M1) {
        if (epochMs < 0) epochMs = 0;
        int64_t y = 0; int m = 0; int d = 0;
        civilFromDays(epochMs / kMsPerDay, y, m, d);
        const int64_t open = daysFromCivil(y, m, 1) * kMsPerDay;
        const int64_t close = (m == 12 ? daysFromCivil(y + 1, 1, 1) : daysFromCivil(y, m + 1, 1)) * kMsPerDay;
        return { open, close, true };
    }

    if (type == SessionType::W1) {
        // Crypto trades 24/7: the week is Monday 00:00 UTC to the next Monday.
        const int64_t midnight = utcMidnightBefore(epochMs);
        const int dow = utcWeekday(midnight);  // 0=Sun…6=Sat
        const int daysBack = (dow + 6) % 7;    // 0 on Monday
        const int64_t weekOpen = midnight - static_cast<int64_t>(daysBack) * kMsPerDay;
        return { weekOpen, weekOpen + kMsPerWeek, true };
    }

    const SessionSpec& spec = kSpecs[static_cast<int>(type)];

    // Daily sessions: if epochMs lands before today's open, resolve to the
    // previous day's session. This also handles sessions that cross midnight.
    const int64_t midnight = utcMidnightBefore(epochMs);
    int64_t sessionStart = midnight + spec.openOffsetMs;
    if (epochMs < sessionStart) {
        sessionStart -= kMsPerDay;
    }
    return { sessionStart, sessionStart + spec.durationMs, true };
}

/*
 * Convenience: return the nominal duration of a session type in ms.
 * W1 = 7 days. M1 = 31 days (the longest month; actual months use
 * sessionContaining). Every M1 session is a whole number of UTC days, so any
 * period that divides a day partitions every month.
 */
inline int64_t sessionDurationMs(SessionType type) {
    using namespace detail;
    if (type == SessionType::W1) return kMsPerWeek;
    if (type == SessionType::M1) return kMaxMonthMs;
    if (static_cast<int>(type) < 0 || static_cast<int>(type) > static_cast<int>(SessionType::H24)) return 0;
    return kSpecs[static_cast<int>(type)].durationMs;
}

/*
 * Clamp epochMs to the nearest session open on or before it, respecting the
 * configured session type.  Useful for aligning the first TPO letter.
 */
inline int64_t alignToSessionOpen(int64_t epochMs, SessionType type) {
    return sessionContaining(epochMs, type).startMs;
}

} // namespace SessionManager
