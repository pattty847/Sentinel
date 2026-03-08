#pragma once

#include <QByteArray>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// Forward-declared to avoid pulling <SessionManager.hpp> into every TU.
namespace SessionManager { enum class SessionType : int; }

class TpoStreamState {
public:
    // Display mode controls how m_columns are indexed.
    enum class DisplayMode : int {
        // Horizontal Market Profile: column[rank] holds all price levels visited
        // at least (rank+1) times within the session.  The horizontal width of
        // each row reflects how many unique time-brackets visited that level.
        // Letters are preserved as the ASCII char from the server.
        HorizontalProfile = 0,

        // Vertical timeline: column[periodIdx] mirrors the server bucket directly.
        // The renderer can align each column with its time position on the chart.
        VerticalTimeline  = 1,
    };

    struct PendingUpload {
        int x = 0;
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        QByteArray data;
    };

    struct Snapshot {
        int gridWidth = 0;
        int gridHeight = 0;
        int filledColumns = 0;
        int writeColumn = -1;
        int64_t lastSliceStartMs = 0;
        int64_t sessionStartMs = 0;
        int64_t sessionEndMs = 0;
        int64_t timeframeMs = 0;
        DisplayMode displayMode = DisplayMode::VerticalTimeline;
        int pendingUploads = 0;
    };

    void clear();
    void reset(int gridWidth, int gridHeight);

    // ── Session configuration ──────────────────────────────────────────────
    // Set a custom session duration (overrides SessionType).
    void setSessionMs(int64_t sessionMs);
    // Set a named session type; resolves duration via SessionManager.
    void setSessionType(int sessionType);  // SessionManager::SessionType cast to int

    // ── Display mode ──────────────────────────────────────────────────────
    void setDisplayMode(DisplayMode mode);
    DisplayMode displayMode() const;

    bool ingestSlice(int64_t bucketStartMs,
                     int64_t bucketEndMs,
                     int64_t timeframeMs,
                     int gridWidth,
                     int gridHeight,
                     const QByteArray& data);
    void takePendingUploads(std::vector<PendingUpload>& out);
    Snapshot snapshot() const;

private:
    struct LevelState {
        int row = -1;
        // Each entry is the periodIdx of one unique visit; the index of this
        // entry is the "rank" used when building HorizontalProfile columns.
        std::vector<int> periodIndices;
        // Parallel array: the actual letter byte for each visit.
        std::vector<char> letters;
    };

    void resetLocked(int gridWidth, int gridHeight);

    mutable std::mutex m_mutex;
    mutable std::mutex m_uploadMutex;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    int m_filledColumns = 0;
    int m_writeColumn = -1;
    int64_t m_sessionStartMs = 0;
    int64_t m_sessionEndMs   = 0;
    int64_t m_sessionMs = 86'400'000;  // default H24 session fallback
    int m_sessionType = 4;             // SessionManager::SessionType::H24 fallback
    int64_t m_lastSliceStartMs = 0;
    int64_t m_timeframeMs = 0;
    DisplayMode m_displayMode = DisplayMode::VerticalTimeline;
    std::vector<QByteArray> m_columns;
    std::unordered_map<int64_t, LevelState> m_levelsByTick;
    std::vector<PendingUpload> m_pending;
};
