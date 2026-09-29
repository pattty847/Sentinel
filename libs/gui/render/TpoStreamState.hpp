#pragma once

#include <QByteArray>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

/*
 * TpoStreamState
 *
 * GUI-side store of TPO letter columns, one column per session period
 * (column x = period index since the session open, row 0 = highest price).
 * Keeps the most recent `maxSessions` sessions so collapsed profiles of earlier
 * sessions stay on the chart next to the live one. Changed columns are queued
 * as pending uploads for the render thread, each tagged with its session.
 *
 * Threading: ingest on the data thread; every accessor takes the internal lock.
 */
class TpoStreamState {
public:
    struct PendingUpload {
        int x = 0;                  // period index within the session
        int periods = 0;            // session duration / period duration
        int64_t sessionStartMs = 0;
        int64_t sessionEndMs = 0;
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        QByteArray data;            // one letter byte per row, '\0' = not visited
    };

    struct Snapshot {
        int gridHeight = 0;
        int sessions = 0;
        int64_t latestSessionStartMs = 0;
        int64_t latestSessionEndMs = 0;
        int64_t lastSliceStartMs = 0;
        int64_t timeframeMs = 0;
        int pendingUploads = 0;
    };

    static constexpr int kDefaultMaxSessions = 8;  // the renderer trims to tpo.sessions
    static constexpr int kMaxSessionsLimit = 8;

    void clear();
    // gridWidth is ignored: the period count comes from the session and timeframe.
    void reset(int gridWidth, int gridHeight);

    // Named session type (SessionManager::SessionType cast to int).
    void setSessionType(int sessionType);
    // Number of sessions retained (1..kMaxSessionsLimit). Older sessions are evicted.
    void setMaxSessions(int sessions);
    int maxSessions() const;

    // Returns false when the slice is malformed, outside its session, or older
    // than every retained session while the store is full.
    bool ingestSlice(int64_t bucketStartMs,
                     int64_t bucketEndMs,
                     int64_t timeframeMs,
                     int gridWidth,
                     int gridHeight,
                     const QByteArray& data);
    void takePendingUploads(std::vector<PendingUpload>& out);
    Snapshot snapshot() const;

private:
    struct Session {
        int64_t endMs = 0;
        std::vector<QByteArray> columns;
    };

    void clearLocked();

    mutable std::mutex m_mutex;
    int m_gridHeight = 0;
    int m_sessionType = 4;  // SessionManager::SessionType::H24
    int m_maxSessions = kDefaultMaxSessions;
    int64_t m_timeframeMs = 0;
    int64_t m_lastSliceStartMs = 0;
    std::map<int64_t, Session> m_sessions;  // keyed by session start
    std::vector<PendingUpload> m_pending;
};
