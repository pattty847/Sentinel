#include "FootprintStreamState.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

void FootprintStreamState::setGridDimensions(int gridWidth, int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth == gridWidth && m_gridHeight == gridHeight) {
        return;
    }
    resetLocked(gridWidth, gridHeight);
}

void FootprintStreamState::updateRange(double minPrice, double maxPrice, double tickSize) {
    if (tickSize <= 0.0 || maxPrice <= minPrice) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
    m_rangeValid = true;
}

void FootprintStreamState::clear() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth <= 0 || m_gridHeight <= 0) {
        return;
    }
    resetLocked(m_gridWidth, m_gridHeight);
}

bool FootprintStreamState::ingestSlice(int64_t bucketStartMs,
                                       int64_t bucketEndMs,
                                       int64_t timeframeMs,
                                       int gridWidth,
                                       int gridHeight,
                                       double minPrice,
                                       double maxPrice,
                                       double tickSize,
                                       const QByteArray& deltaLevelsQ16) {
    if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0) {
        return false;
    }
    if (gridWidth <= 0 || gridHeight <= 0) {
        return false;
    }
    const int expectedBytes = gridHeight * static_cast<int>(sizeof(int16_t));
    if (deltaLevelsQ16.size() != expectedBytes) {
        return false;
    }
    if (tickSize <= 0.0 || maxPrice <= minPrice) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth != gridWidth || m_gridHeight != gridHeight) {
        resetLocked(gridWidth, gridHeight);
    }

    const bool hasPrevious = (m_filledColumns > 0);
    bool shouldReset = !m_rangeValid;
    if (hasPrevious && bucketStartMs <= m_lastSliceStartMs) {
        shouldReset = true;
    }
    if (hasPrevious) {
        const int64_t maxGapMs = static_cast<int64_t>(m_gridWidth) * timeframeMs;
        if (maxGapMs > 0 && (bucketStartMs - m_lastSliceStartMs) > maxGapMs) {
            shouldReset = true;
        }
    }
    if (shouldReset) {
        resetLocked(m_gridWidth, m_gridHeight);
    }

    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
    m_rangeValid = true;

    m_writeColumn = (m_writeColumn + 1) % m_gridWidth;
    if (m_filledColumns < m_gridWidth) {
        ++m_filledColumns;
    }

    auto& slot = m_columns[static_cast<size_t>(m_writeColumn)];
    slot.bucketStartMs = bucketStartMs;
    slot.bucketEndMs = bucketEndMs;
    slot.timeframeMs = timeframeMs;
    slot.valid = true;
    if (slot.deltaQ16.size() != static_cast<size_t>(gridHeight)) {
        slot.deltaQ16.assign(static_cast<size_t>(gridHeight), 0);
    }
    std::memcpy(slot.deltaQ16.data(), deltaLevelsQ16.constData(), static_cast<size_t>(expectedBytes));
    m_lastSliceStartMs = bucketStartMs;
    return true;
}

FootprintStreamState::Snapshot FootprintStreamState::snapshot() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    Snapshot snap;
    snap.gridWidth = m_gridWidth;
    snap.gridHeight = m_gridHeight;
    snap.writeColumn = m_writeColumn;
    snap.filledColumns = m_filledColumns;
    snap.lastSliceStartMs = m_lastSliceStartMs;
    snap.minPrice = m_minPrice;
    snap.maxPrice = m_maxPrice;
    snap.tickSize = m_tickSize;
    snap.rangeValid = m_rangeValid;
    snap.resetGeneration = m_resetGeneration;
    return snap;
}

void FootprintStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_writeColumn = 0;
    m_filledColumns = 0;
    m_lastSliceStartMs = 0;
    m_minPrice = 0.0;
    m_maxPrice = 0.0;
    m_tickSize = 0.0;
    m_rangeValid = false;
    ++m_resetGeneration;

    m_columns.assign(static_cast<size_t>(gridWidth), ColumnSlot{});
    for (auto& slot : m_columns) {
        slot.deltaQ16.assign(static_cast<size_t>(gridHeight), 0);
    }
}
