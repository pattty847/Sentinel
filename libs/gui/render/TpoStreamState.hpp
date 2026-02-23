#pragma once

#include <QByteArray>
#include <cstdint>
#include <mutex>
#include <vector>

class TpoStreamState {
public:
    struct PendingUpload {
        int x = 0;
        QByteArray data;
    };

    struct Snapshot {
        int gridWidth = 0;
        int gridHeight = 0;
        int filledColumns = 0;
        int writeColumn = 0;
        int64_t timeframeMs = 0;
    };

    void reset(int gridWidth, int gridHeight);
    bool ingestColumn(int x, int gridWidth, int gridHeight, int64_t timeframeMs, const QByteArray& data);
    void takePendingUploads(std::vector<PendingUpload>& out);
    Snapshot snapshot() const;

private:
    mutable std::mutex m_mutex;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    int m_filledColumns = 0;
    int m_writeColumn = 0;
    int64_t m_timeframeMs = 0;
    std::vector<QByteArray> m_columns;
    std::vector<PendingUpload> m_pending;
};

