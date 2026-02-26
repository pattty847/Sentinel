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
 *   W1         Sunday 21:00 – Friday 21:00 UTC  (FX/crypto week boundary)
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
// Australia crosses midnight → handled specially below.
static constexpr int64_t kMsPerDay  = 86'400'000LL;
static constexpr int64_t kMsPerWeek = 7LL * kMsPerDay;

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
    // Australia: 22:00 UTC (prev day), 9 h
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

} // namespace detail

// ─── Primary API ───────────────────────────────────────────────────────────

/*
 * sessionContaining(epochMs, type)
 *
 * Returns the session window [startMs, endMs) that either contains epochMs
 * or is the most-recent completed session before epochMs.
 *
 * For W1: returns the Sunday–Friday week that contains epochMs.
 * For all others: returns the daily session on the UTC calendar day of epochMs,
 * or (for Australia) the session whose open most recently preceded epochMs.
 */
inline SessionBoundary sessionContaining(int64_t epochMs, SessionType type) {
    using namespace detail;

    if (type == SessionType::W1) {
        // Find most recent Sunday 21:00 UTC before epochMs.
        const int64_t midnight = utcMidnightBefore(epochMs);
        const int dow = utcWeekday(midnight);  // 0=Sun…6=Sat
        // How many days back to Sunday?
        const int daysBack = (dow + 7) % 7; // 0 if today is Sunday
        const int64_t sundayMidnight = midnight - static_cast<int64_t>(daysBack) * kMsPerDay;
        const int64_t weekOpen  = sundayMidnight + 21LL * 3600'000LL;
        const int64_t weekClose = weekOpen + 5LL * kMsPerDay; // Fri 21:00 UTC
        return { weekOpen, weekClose, true };
    }

    const SessionSpec& spec = kSpecs[static_cast<int>(type)];

    if (type == SessionType::Australia) {
        // Open at 22:00 UTC, so the session for calendar day D opens on day D-1.
        // Find the 22:00 UTC anchor most recently ≤ epochMs.
        const int64_t midnight = utcMidnightBefore(epochMs);
        // Session open on the current UTC day context: midnight - 1 day + 22h
        const int64_t openToday = midnight - kMsPerDay + spec.openOffsetMs;
        // If epochMs is before today's 22:00 window we are in the previous cycle.
        const int64_t openPrev  = openToday - kMsPerDay;

        int64_t sessionStart = openToday;
        if (epochMs < openToday) {
            sessionStart = openPrev;
        }
        return { sessionStart, sessionStart + spec.durationMs, true };
    }

    // Standard sessions: open offset relative to UTC midnight of epochMs.
    const int64_t midnight = utcMidnightBefore(epochMs);
    const int64_t sessionStart = midnight + spec.openOffsetMs;
    return { sessionStart, sessionStart + spec.durationMs, true };
}

/*
 * Convenience: return the nominal duration of a session type in ms.
 * W1 = 5 days.
 */
inline int64_t sessionDurationMs(SessionType type) {
    using namespace detail;
    if (type == SessionType::W1) return 5LL * kMsPerDay;
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
