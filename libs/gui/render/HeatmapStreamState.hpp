/*
Sentinel — HeatmapStreamState
Role: Owns ring cursor, pending uploads, and time alignment state for GPU heatmap.
Threading: Ingest on GUI thread; snapshot/pending uploads on render thread.
*/
#pragma once

#include <QByteArray>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

class HeatmapStreamState {
public:
    struct PendingColumn {
        int x = 0;
        QByteArray data;
    };

    struct PendingLabelColumn {
        int x = 0;
        QByteArray intensity;
        QByteArray liquidity;
        double liquidityScale = 1.0;
        bool haveLiquidity = false;
    };

    struct Snapshot {
        int gridWidth = 0;
        int gridHeight = 0;
        int appendMs = 0;
        int64_t lastSliceStartMs = std::numeric_limits<int64_t>::min();
        int64_t timeOriginMs = 0;
        int64_t streamBaseMs = std::numeric_limits<int64_t>::min();
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        float timeOffset = 0.0f;
        bool liquidityAvailable = false;
    };

    struct LabelSnapshot {
        Snapshot snapshot;
        std::vector<uint16_t> liquidityRing;
        std::vector<uint16_t> intensityRing;
        std::vector<double> liquidityScales;
    };

    HeatmapStreamState() = default;

    void reset(int gridWidth, int gridHeight, double minPrice, double maxPrice, double tickSize);
    void setGridDimensions(int gridWidth, int gridHeight);
    void setAppendMs(int appendMs);
    void updateRange(double minPrice, double maxPrice, double tickSize);

    void ingestSlice(int64_t sliceStartMs,
                     int timeframeMs,
                     const QByteArray& intensityColumn,
                     const QByteArray& liquidityColumn,
                     double liquidityScale,
                     qint64 nowMs);

    void setIntensityBytesPerCell(int bytesPerCell);
    int intensityBytesPerCell() const;

    void updateTimeOffset(float fractionalOffset);

    Snapshot snapshot() const;
    int writeColumn() const;
    qint64 lastAppendMs() const;
    int pendingUploadCount() const;
    bool copyLabelSnapshot(LabelSnapshot& out) const;

    void takePendingUploads(std::vector<PendingColumn>& out);
    void takePendingLabelUploads(std::vector<PendingLabelColumn>& out);
    void copyLiquiditySnapshot(std::vector<uint16_t>& liquidityRing,
                               std::vector<uint16_t>& intensityRing,
                               std::vector<double>& liquidityScales,
                               bool& liquidityAvailable) const;

private:
    void resetLocked(int gridWidth, int gridHeight);

    mutable std::mutex m_stateMutex;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    int m_appendMs = 0;
    int m_writeColumn = 0;
    qint64 m_lastAppendMs = 0;
    int64_t m_lastSliceStartMs = std::numeric_limits<int64_t>::min();
    int64_t m_timeOriginMs = 0;
    int64_t m_streamBaseMs = std::numeric_limits<int64_t>::min();
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    double m_tickSize = 0.0;
    QByteArray m_lastColumnData;
    bool m_haveLastColumn = false;
    QByteArray m_lastLiquidityColumn;
    double m_lastLiquidityScale = 1.0;
    bool m_haveLastLiquidity = false;

    mutable std::mutex m_uploadMutex;
    std::vector<PendingColumn> m_pendingUploads;

    mutable std::mutex m_labelUploadMutex;
    std::vector<PendingLabelColumn> m_pendingLabelUploads;

    mutable std::mutex m_ringMutex;
    std::vector<uint16_t> m_intensityRing;
    std::vector<uint16_t> m_liquidityRing;
    std::vector<double> m_liquidityScales;
    bool m_liquidityAvailable = false;
    int m_intensityBytesPerCell = 1;

    std::atomic<float> m_timeOffset{0.0f};
};
