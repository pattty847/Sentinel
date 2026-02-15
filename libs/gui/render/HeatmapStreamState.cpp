/*
Sentinel — HeatmapStreamState
Owns ring cursor, pending uploads, and time alignment state for GPU heatmap.
*/
#include "HeatmapStreamState.hpp"

#include <QtEndian>
#include <algorithm>
#include <limits>

void HeatmapStreamState::reset(int gridWidth, int gridHeight, double minPrice, double maxPrice, double tickSize) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    resetLocked(gridWidth, gridHeight);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
}

void HeatmapStreamState::setGridDimensions(int gridWidth, int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth == gridWidth && m_gridHeight == gridHeight) {
        return;
    }
    resetLocked(gridWidth, gridHeight);
}

void HeatmapStreamState::setAppendMs(int appendMs) {
    if (appendMs <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_appendMs = appendMs;
}

void HeatmapStreamState::updateRange(double minPrice, double maxPrice, double tickSize) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
}

void HeatmapStreamState::ingestSlice(int64_t sliceStartMs,
                                     int timeframeMs,
                                     const QByteArray& intensityColumn,
                                     const QByteArray& liquidityColumn,
                                     double liquidityScale,
                                     qint64 nowMs) {
    if (intensityColumn.isEmpty()) {
        return;
    }

    int gridWidth = 0;
    int gridHeight = 0;
    int appendMs = 0;
    int bytesPerCell = 1;
    int step = 1;
    bool sameBucketUpdate = false;
    bool haveLastColumn = false;
    bool haveLastLiquidity = false;
    QByteArray lastColumnData;
    QByteArray lastLiquidityColumn;
    double lastLiquidityScale = 1.0;
    int writeColumn = 0;
    int64_t lastSliceStartMs = std::numeric_limits<int64_t>::min();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (timeframeMs > 0) {
            m_appendMs = timeframeMs;
        }
        gridWidth = m_gridWidth;
        gridHeight = m_gridHeight;
        appendMs = m_appendMs;
        bytesPerCell = m_intensityBytesPerCell;
        if (gridWidth <= 0 || gridHeight <= 0 || appendMs <= 0) {
            return;
        }

        lastSliceStartMs = m_lastSliceStartMs;
        if (lastSliceStartMs != std::numeric_limits<int64_t>::min() &&
            sliceStartMs < lastSliceStartMs) {
            return;
        }
        sameBucketUpdate = (lastSliceStartMs != std::numeric_limits<int64_t>::min() &&
                            sliceStartMs == lastSliceStartMs);

        if (m_timeOriginMs == 0) {
            m_timeOriginMs = sliceStartMs;
        }
        if (m_streamBaseMs == std::numeric_limits<int64_t>::min()) {
            m_streamBaseMs = sliceStartMs - nowMs;
        } else if (appendMs > 0) {
            const int64_t desiredBase = sliceStartMs - nowMs;
            const int64_t drift = desiredBase - m_streamBaseMs;
            const int64_t driftAbs = std::llabs(drift);
            const int64_t driftThreshold = std::max<int64_t>(1, appendMs / 2);
            if (driftAbs > driftThreshold) {
                m_streamBaseMs = desiredBase;
            }
        }

        if (!sameBucketUpdate &&
            lastSliceStartMs != std::numeric_limits<int64_t>::min() &&
            appendMs > 0) {
            const int64_t dt = sliceStartMs - lastSliceStartMs;
            if (dt > 0) {
                const int64_t rawStep = dt / appendMs;
                step = static_cast<int>(std::max<int64_t>(1, rawStep));
            }
        }

        haveLastColumn = m_haveLastColumn;
        lastColumnData = m_lastColumnData;
        haveLastLiquidity = m_haveLastLiquidity;
        lastLiquidityColumn = m_lastLiquidityColumn;
        lastLiquidityScale = m_lastLiquidityScale;
        writeColumn = m_writeColumn;
    }

    const int expectedLiquidityBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
    const bool haveLiquidityColumn = (liquidityColumn.size() == expectedLiquidityBytes);
    const int expectedIntensityBytes = gridHeight * bytesPerCell;

    QByteArray fillColumn;
    if (!haveLastColumn) {
        fillColumn = QByteArray(intensityColumn.size(), 0);
    } else {
        fillColumn = lastColumnData;
    }

    QByteArray fillLiquidityColumn;
    double fillLiquidityScale = lastLiquidityScale;
    if (!haveLastLiquidity || expectedLiquidityBytes <= 0) {
        fillLiquidityColumn = QByteArray(expectedLiquidityBytes, 0);
        fillLiquidityScale = 1.0;
    } else {
        fillLiquidityColumn = lastLiquidityColumn;
    }

    auto upsertPendingColumn = [](std::vector<PendingColumn>& uploads, int x, const QByteArray& data) {
        for (auto& pending : uploads) {
            if (pending.x == x) {
                pending.data = data;
                return;
            }
        }
        uploads.push_back({x, data});
    };
    auto upsertPendingLabelColumn = [](std::vector<PendingLabelColumn>& uploads,
                                       int x,
                                       const QByteArray& intensity,
                                       const QByteArray& liquidity,
                                       double scale,
                                       bool haveLiquidity) {
        for (auto& pending : uploads) {
            if (pending.x == x) {
                pending.intensity = intensity;
                pending.liquidity = liquidity;
                pending.liquidityScale = scale;
                pending.haveLiquidity = haveLiquidity;
                return;
            }
        }
        PendingLabelColumn pending;
        pending.x = x;
        pending.intensity = intensity;
        pending.liquidity = liquidity;
        pending.liquidityScale = scale;
        pending.haveLiquidity = haveLiquidity;
        uploads.push_back(std::move(pending));
    };

    if (!sameBucketUpdate) {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        for (int i = 0; i < step - 1; ++i) {
            writeColumn = (writeColumn + 1) % gridWidth;
            upsertPendingColumn(m_pendingUploads, writeColumn, fillColumn);
        }

        writeColumn = (writeColumn + 1) % gridWidth;
        upsertPendingColumn(m_pendingUploads, writeColumn, intensityColumn);
    } else {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        upsertPendingColumn(m_pendingUploads, writeColumn, intensityColumn);
    }

    {
        std::lock_guard<std::mutex> lock(m_labelUploadMutex);
        if (!sameBucketUpdate) {
            for (int i = 0; i < step - 1; ++i) {
                const int columnIndex = (writeColumn - (step - 1 - i) + gridWidth) % gridWidth;
                upsertPendingLabelColumn(m_pendingLabelUploads,
                                         columnIndex,
                                         fillColumn,
                                         fillLiquidityColumn,
                                         fillLiquidityScale,
                                         fillLiquidityColumn.size() == expectedLiquidityBytes);
            }
        }

        QByteArray labelLiquidity = fillLiquidityColumn;
        double labelLiquidityScale = fillLiquidityScale;
        bool labelHaveLiquidity = false;
        if (haveLiquidityColumn) {
            labelLiquidity = liquidityColumn;
            labelLiquidityScale = (liquidityScale > 0.0) ? liquidityScale : 1.0;
            labelHaveLiquidity = true;
        }
        upsertPendingLabelColumn(m_pendingLabelUploads,
                                 writeColumn,
                                 intensityColumn,
                                 labelLiquidity,
                                 labelLiquidityScale,
                                 labelHaveLiquidity);
    }

    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const size_t expectedSize = static_cast<size_t>(gridWidth) * gridHeight;
        if (m_intensityRing.size() != expectedSize) {
            m_intensityRing.assign(expectedSize, 0);
        }
        if (m_liquidityRing.size() != expectedSize) {
            m_liquidityRing.assign(expectedSize, 0);
            m_liquidityScales.assign(gridWidth, 1.0);
        }

        for (int i = 0; i < step - 1; ++i) {
            const int columnIndex = (writeColumn - (step - 1 - i) + gridWidth) % gridWidth;
            if (fillColumn.size() == expectedIntensityBytes) {
                if (bytesPerCell == 1) {
                    const auto* src = reinterpret_cast<const uint8_t*>(fillColumn.constData());
                    for (int y = 0; y < gridHeight; ++y) {
                        m_intensityRing[static_cast<size_t>(y) * gridWidth + columnIndex] =
                            static_cast<uint16_t>(src[y]) * 257;
                    }
                } else if (bytesPerCell == 2) {
                    const auto* src = reinterpret_cast<const uint16_t*>(fillColumn.constData());
                    for (int y = 0; y < gridHeight; ++y) {
                        const uint16_t raw = qFromLittleEndian(src[y]);
                        m_intensityRing[static_cast<size_t>(y) * gridWidth + columnIndex] = raw;
                    }
                }
            }
            if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_liquidityRing[static_cast<size_t>(y) * gridWidth + columnIndex] = raw;
                }
                m_liquidityScales[columnIndex] = fillLiquidityScale;
            }
        }

        if (intensityColumn.size() == expectedIntensityBytes) {
            if (bytesPerCell == 1) {
                const auto* src = reinterpret_cast<const uint8_t*>(intensityColumn.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    m_intensityRing[static_cast<size_t>(y) * gridWidth + writeColumn] =
                        static_cast<uint16_t>(src[y]) * 257;
                }
            } else if (bytesPerCell == 2) {
                const auto* src = reinterpret_cast<const uint16_t*>(intensityColumn.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_intensityRing[static_cast<size_t>(y) * gridWidth + writeColumn] = raw;
                }
            }
        }
        if (haveLiquidityColumn) {
            const auto* src = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
            for (int y = 0; y < gridHeight; ++y) {
                const uint16_t raw = qFromLittleEndian(src[y]);
                m_liquidityRing[static_cast<size_t>(y) * gridWidth + writeColumn] = raw;
            }
            m_liquidityScales[writeColumn] = (liquidityScale > 0.0) ? liquidityScale : 1.0;
            m_liquidityAvailable = true;
        } else {
            if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_liquidityRing[static_cast<size_t>(y) * gridWidth + writeColumn] = raw;
                }
                m_liquidityScales[writeColumn] = fillLiquidityScale;
            }
            m_liquidityAvailable = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_writeColumn = writeColumn;
        if (!sameBucketUpdate && m_filledColumns < m_gridWidth) {
            const int remaining = m_gridWidth - m_filledColumns;
            const int addCount = std::min(remaining, step);
            m_filledColumns += addCount;
        }
        m_lastSliceStartMs = sliceStartMs;
        m_lastColumnData = intensityColumn;
        m_haveLastColumn = true;
        if (haveLiquidityColumn) {
            m_lastLiquidityColumn = liquidityColumn;
            m_lastLiquidityScale = (liquidityScale > 0.0) ? liquidityScale : 1.0;
            m_haveLastLiquidity = true;
        }
        m_lastAppendMs = nowMs;
    }
}

void HeatmapStreamState::updateTimeOffset(float fractionalOffset) {
    int gridWidth = 0;
    int writeColumn = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        gridWidth = m_gridWidth;
        writeColumn = m_writeColumn;
    }
    if (gridWidth <= 0) {
        return;
    }
    const int oldestColumn = (writeColumn + 1) % gridWidth;
    const float offset = (static_cast<float>(oldestColumn) + fractionalOffset) /
                         static_cast<float>(gridWidth);
    m_timeOffset.store(offset);
}

void HeatmapStreamState::setIntensityBytesPerCell(int bytesPerCell) {
    if (bytesPerCell != 1 && bytesPerCell != 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_intensityBytesPerCell = bytesPerCell;
}

int HeatmapStreamState::intensityBytesPerCell() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_intensityBytesPerCell;
}

HeatmapStreamState::Snapshot HeatmapStreamState::snapshot() const {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snap.gridWidth = m_gridWidth;
        snap.gridHeight = m_gridHeight;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
        snap.filledColumns = m_filledColumns;
        snap.minPrice = m_minPrice;
        snap.maxPrice = m_maxPrice;
        snap.tickSize = m_tickSize;
    }
    snap.timeOffset = m_timeOffset.load();
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        snap.liquidityAvailable = m_liquidityAvailable;
    }
    return snap;
}

int HeatmapStreamState::writeColumn() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_writeColumn;
}

qint64 HeatmapStreamState::lastAppendMs() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastAppendMs;
}

int HeatmapStreamState::pendingUploadCount() const {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    return static_cast<int>(m_pendingUploads.size());
}

bool HeatmapStreamState::copyLabelSnapshot(LabelSnapshot& out) const {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snap.gridWidth = m_gridWidth;
        snap.gridHeight = m_gridHeight;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
        snap.filledColumns = m_filledColumns;
        snap.minPrice = m_minPrice;
        snap.maxPrice = m_maxPrice;
        snap.tickSize = m_tickSize;
    }
    snap.timeOffset = m_timeOffset.load();

    std::lock_guard<std::mutex> lock(m_ringMutex);
    snap.liquidityAvailable = m_liquidityAvailable;
    out.snapshot = snap;
    out.liquidityRing = m_liquidityRing;
    out.intensityRing = m_intensityRing;
    out.liquidityScales = m_liquidityScales;
    return snap.liquidityAvailable;
}

void HeatmapStreamState::takePendingUploads(std::vector<PendingColumn>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

void HeatmapStreamState::takePendingLabelUploads(std::vector<PendingLabelColumn>& out) {
    std::lock_guard<std::mutex> lock(m_labelUploadMutex);
    if (!m_pendingLabelUploads.empty()) {
        out.swap(m_pendingLabelUploads);
    }
}

void HeatmapStreamState::copyLiquiditySnapshot(std::vector<uint16_t>& liquidityRing,
                                               std::vector<uint16_t>& intensityRing,
                                               std::vector<double>& liquidityScales,
                                               bool& liquidityAvailable) const {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    liquidityRing = m_liquidityRing;
    intensityRing = m_intensityRing;
    liquidityScales = m_liquidityScales;
    liquidityAvailable = m_liquidityAvailable;
}

void HeatmapStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_writeColumn = 0;
    m_filledColumns = 0;
    m_lastAppendMs = 0;
    m_lastSliceStartMs = std::numeric_limits<int64_t>::min();
    m_timeOriginMs = 0;
    m_streamBaseMs = std::numeric_limits<int64_t>::min();
    m_lastColumnData.clear();
    m_haveLastColumn = false;
    m_lastLiquidityColumn.clear();
    m_lastLiquidityScale = 1.0;
    m_haveLastLiquidity = false;
    m_minPrice = 0.0;
    m_maxPrice = 0.0;
    m_tickSize = 0.0;

    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        m_pendingUploads.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_labelUploadMutex);
        m_pendingLabelUploads.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const size_t expectedSize = static_cast<size_t>(gridWidth) * gridHeight;
        m_intensityRing.assign(expectedSize, 0);
        m_liquidityRing.assign(expectedSize, 0);
        m_liquidityScales.assign(gridWidth, 1.0);
        m_liquidityAvailable = false;
    }
    m_timeOffset.store(0.0f);
}
