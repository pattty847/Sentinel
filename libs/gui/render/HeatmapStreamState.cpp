/*
Sentinel — HeatmapStreamState
Owns ring cursor, pending uploads, and time alignment state for GPU heatmap.
*/
#include "HeatmapStreamState.hpp"

#include <QtEndian>
#include <algorithm>
#include <limits>

void HeatmapStreamState::reset(int gridSize, double minPrice, double maxPrice, double tickSize) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    resetLocked(gridSize);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
}

void HeatmapStreamState::setGridSize(int gridSize) {
    if (gridSize <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridSize == gridSize) {
        return;
    }
    resetLocked(gridSize);
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

    int gridSize = 0;
    int appendMs = 0;
    int bytesPerCell = 1;
    int step = 1;
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
        gridSize = m_gridSize;
        appendMs = m_appendMs;
        bytesPerCell = m_intensityBytesPerCell;
        if (gridSize <= 0 || appendMs <= 0) {
            return;
        }

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

        lastSliceStartMs = m_lastSliceStartMs;
        if (lastSliceStartMs != std::numeric_limits<int64_t>::min() && appendMs > 0) {
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

    const int expectedLiquidityBytes = gridSize * static_cast<int>(sizeof(uint16_t));
    const bool haveLiquidityColumn = (liquidityColumn.size() == expectedLiquidityBytes);
    const int expectedIntensityBytes = gridSize * bytesPerCell;

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

    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        for (int i = 0; i < step - 1; ++i) {
            writeColumn = (writeColumn + 1) % gridSize;
            m_pendingUploads.push_back({writeColumn, fillColumn});
        }

        writeColumn = (writeColumn + 1) % gridSize;
        m_pendingUploads.push_back({writeColumn, intensityColumn});
    }

    {
        std::lock_guard<std::mutex> lock(m_labelUploadMutex);
        for (int i = 0; i < step - 1; ++i) {
            const int columnIndex = (writeColumn - (step - 1 - i) + gridSize) % gridSize;
            PendingLabelColumn pending;
            pending.x = columnIndex;
            pending.intensity = fillColumn;
            pending.liquidity = fillLiquidityColumn;
            pending.liquidityScale = fillLiquidityScale;
            pending.haveLiquidity = (fillLiquidityColumn.size() == expectedLiquidityBytes);
            m_pendingLabelUploads.push_back(std::move(pending));
        }

        PendingLabelColumn pending;
        pending.x = writeColumn;
        pending.intensity = intensityColumn;
        if (haveLiquidityColumn) {
            pending.liquidity = liquidityColumn;
            pending.liquidityScale = (liquidityScale > 0.0) ? liquidityScale : 1.0;
            pending.haveLiquidity = true;
        } else {
            pending.liquidity = fillLiquidityColumn;
            pending.liquidityScale = fillLiquidityScale;
            pending.haveLiquidity = false;
        }
        m_pendingLabelUploads.push_back(std::move(pending));
    }

    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const size_t expectedSize = static_cast<size_t>(gridSize) * gridSize;
        if (m_intensityRing.size() != expectedSize) {
            m_intensityRing.assign(expectedSize, 0);
        }
        if (m_liquidityRing.size() != expectedSize) {
            m_liquidityRing.assign(expectedSize, 0);
            m_liquidityScales.assign(gridSize, 1.0);
        }

        for (int i = 0; i < step - 1; ++i) {
            const int columnIndex = (writeColumn - (step - 1 - i) + gridSize) % gridSize;
            if (fillColumn.size() == expectedIntensityBytes) {
                if (bytesPerCell == 1) {
                    const auto* src = reinterpret_cast<const uint8_t*>(fillColumn.constData());
                    for (int y = 0; y < gridSize; ++y) {
                        m_intensityRing[static_cast<size_t>(y) * gridSize + columnIndex] =
                            static_cast<uint16_t>(src[y]) * 257;
                    }
                } else if (bytesPerCell == 2) {
                    const auto* src = reinterpret_cast<const uint16_t*>(fillColumn.constData());
                    for (int y = 0; y < gridSize; ++y) {
                        const uint16_t raw = qFromLittleEndian(src[y]);
                        m_intensityRing[static_cast<size_t>(y) * gridSize + columnIndex] = raw;
                    }
                }
            }
            if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                for (int y = 0; y < gridSize; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_liquidityRing[static_cast<size_t>(y) * gridSize + columnIndex] = raw;
                }
                m_liquidityScales[columnIndex] = fillLiquidityScale;
            }
        }

        if (intensityColumn.size() == expectedIntensityBytes) {
            if (bytesPerCell == 1) {
                const auto* src = reinterpret_cast<const uint8_t*>(intensityColumn.constData());
                for (int y = 0; y < gridSize; ++y) {
                    m_intensityRing[static_cast<size_t>(y) * gridSize + writeColumn] =
                        static_cast<uint16_t>(src[y]) * 257;
                }
            } else if (bytesPerCell == 2) {
                const auto* src = reinterpret_cast<const uint16_t*>(intensityColumn.constData());
                for (int y = 0; y < gridSize; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_intensityRing[static_cast<size_t>(y) * gridSize + writeColumn] = raw;
                }
            }
        }
        if (haveLiquidityColumn) {
            const auto* src = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
            for (int y = 0; y < gridSize; ++y) {
                const uint16_t raw = qFromLittleEndian(src[y]);
                m_liquidityRing[static_cast<size_t>(y) * gridSize + writeColumn] = raw;
            }
            m_liquidityScales[writeColumn] = (liquidityScale > 0.0) ? liquidityScale : 1.0;
            m_liquidityAvailable = true;
        } else {
            if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                for (int y = 0; y < gridSize; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_liquidityRing[static_cast<size_t>(y) * gridSize + writeColumn] = raw;
                }
                m_liquidityScales[writeColumn] = fillLiquidityScale;
            }
            m_liquidityAvailable = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_writeColumn = writeColumn;
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
    int gridSize = 0;
    int writeColumn = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        gridSize = m_gridSize;
        writeColumn = m_writeColumn;
    }
    if (gridSize <= 0) {
        return;
    }
    const int oldestColumn = (writeColumn + 1) % gridSize;
    const float offset = (static_cast<float>(oldestColumn) + fractionalOffset) /
                         static_cast<float>(gridSize);
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
        snap.gridSize = m_gridSize;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
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
        snap.gridSize = m_gridSize;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
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

void HeatmapStreamState::resetLocked(int gridSize) {
    m_gridSize = gridSize;
    m_writeColumn = 0;
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
        const size_t expectedSize = static_cast<size_t>(gridSize) * gridSize;
        m_intensityRing.assign(expectedSize, 0);
        m_liquidityRing.assign(expectedSize, 0);
        m_liquidityScales.assign(gridSize, 1.0);
        m_liquidityAvailable = false;
    }
    m_timeOffset.store(0.0f);
}
