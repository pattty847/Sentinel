/*
Sentinel - FootprintStreamState
Role: CPU-side ring storage for footprint columns before GPU integration.
Threading: Ingest on data thread; snapshot/read must take internal lock.
*/
#pragma once

#include <QByteArray>
#include <cstdint>
#include <mutex>
#include <vector>

class FootprintStreamState {
public:
    struct ColumnSlot {
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        int64_t timeframeMs = 0;
        std::vector<uint16_t> deltaQ16;
        bool valid = false;
    };

    struct PendingUpload {
        int x = 0;
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
    };

    struct Snapshot {
        int gridWidth = 0;
        int gridHeight = 0;
        int writeColumn = 0;
        int filledColumns = 0;
        int64_t lastSliceStartMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        bool rangeValid = false;
        uint64_t resetGeneration = 0;
        int pendingUploads = 0;
    };

    FootprintStreamState() = default;

    static int expectedBytesForGridHeight(int gridHeight);

    void setGridDimensions(int gridWidth, int gridHeight);
    void updateRange(double minPrice, double maxPrice, double tickSize);
    void clear();

    bool ingestSlice(int64_t bucketStartMs,
                     int64_t bucketEndMs,
                     int64_t timeframeMs,
                     int gridWidth,
                     int gridHeight,
                     double minPrice,
                     double maxPrice,
                     double tickSize,
                     const QByteArray& deltaLevelsQ16);

    Snapshot snapshot() const;
    int pendingUploadCount() const;
    void takePendingUploads(std::vector<PendingUpload>& out);
    bool copyColumnForUpload(int x, QByteArray& out) const;

private:
    void resetLocked(int gridWidth, int gridHeight);

    mutable std::mutex m_stateMutex;
    mutable std::mutex m_uploadMutex;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    int m_writeColumn = 0;
    int m_filledColumns = 0;
    int64_t m_lastSliceStartMs = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    double m_tickSize = 0.0;
    bool m_rangeValid = false;
    uint64_t m_resetGeneration = 0;
    std::vector<ColumnSlot> m_columns;
    std::vector<PendingUpload> m_pendingUploads;
};
