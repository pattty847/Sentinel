#pragma once

#include <QByteArray>
#include <cstdint>
#include <mutex>
#include <vector>

class TpoStreamState {
public:
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
        int64_t timeframeMs = 0;
        int pendingUploads = 0;
    };

    void clear();
    void reset(int gridWidth, int gridHeight);
    bool ingestSlice(int64_t bucketStartMs,
                     int64_t bucketEndMs,
                     int64_t timeframeMs,
                     int gridWidth,
                     int gridHeight,
                     const QByteArray& data);
    void takePendingUploads(std::vector<PendingUpload>& out);
    Snapshot snapshot() const;

private:
    void resetLocked(int gridWidth, int gridHeight);

    mutable std::mutex m_mutex;
    mutable std::mutex m_uploadMutex;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    int m_filledColumns = 0;
    int m_writeColumn = -1;
    int64_t m_lastSliceStartMs = 0;
    int64_t m_timeframeMs = 0;
    std::vector<QByteArray> m_columns;
    std::vector<PendingUpload> m_pending;
};

