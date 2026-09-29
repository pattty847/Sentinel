#include "FootprintStreamState.hpp"

#include <QtEndian>

#include <limits>
#include <cmath>
#include <algorithm>

int FootprintStreamState::expectedBytesForGridHeight(int gridHeight) {
    if (gridHeight <= 0) {
        return -1;
    }
    constexpr int kBytesPerLevel = static_cast<int>(sizeof(uint16_t));
    if (gridHeight > (std::numeric_limits<int>::max() / kBytesPerLevel)) {
        return -1;
    }
    return gridHeight * kBytesPerLevel;
}

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
    if (!std::isfinite(tickSize) || !std::isfinite(maxPrice) || !std::isfinite(minPrice) || tickSize <= 0.0 || maxPrice <= minPrice) {
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
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pendingUploads.clear();
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
    if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0 ||
        bucketEndMs - bucketStartMs != timeframeMs || bucketStartMs % timeframeMs != 0) {
        return false;
    }
    if (gridWidth <= 0 || gridHeight <= 0) {
        return false;
    }
    const int expectedBytes = expectedBytesForGridHeight(gridHeight);
    if (expectedBytes <= 0) {
        return false;
    }
    if (deltaLevelsQ16.size() != expectedBytes) {
        return false;
    }
    if (!std::isfinite(tickSize) || !std::isfinite(maxPrice) || !std::isfinite(minPrice) || tickSize <= 0.0 || maxPrice <= minPrice) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth != gridWidth || m_gridHeight != gridHeight) {
        resetLocked(gridWidth, gridHeight);
    }

    const bool hasPrevious = (m_filledColumns > 0 && m_writeColumn >= 0);
    bool sameBucketUpdate = hasPrevious && (bucketStartMs == m_lastSliceStartMs);
    bool shouldReset = !m_rangeValid || m_tickSize != tickSize || m_maxPrice != maxPrice ||
        m_minPrice != minPrice || m_timeframeMs != timeframeMs;
    if (hasPrevious && bucketStartMs < m_lastSliceStartMs) {
        shouldReset = true;
    }
    if (hasPrevious && !sameBucketUpdate) {
        const int64_t maxGapMs = static_cast<int64_t>(m_gridWidth) * timeframeMs;
        if (maxGapMs > 0 && (bucketStartMs - m_lastSliceStartMs) > maxGapMs) {
            shouldReset = true;
        }
    }
    if (shouldReset) {
        resetLocked(m_gridWidth, m_gridHeight);
        sameBucketUpdate = false;
    }

    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
    m_rangeValid = true;

    // Explicit empty slots preserve elapsed time when publications skip buckets.
    if (m_lastSliceStartMs > 0 && !sameBucketUpdate) {
        const int missing = static_cast<int>(std::min<int64_t>(m_gridWidth - 1,
            std::max<int64_t>(0, (bucketStartMs - m_lastSliceStartMs) / timeframeMs - 1)));
        for (int i = 0; i < missing; ++i) {
            m_writeColumn = (m_writeColumn + 1) % m_gridWidth;
            auto& gap = m_columns[m_writeColumn];
            gap.valid = true;
            std::fill(gap.deltaQ16.begin(), gap.deltaQ16.end(), 0x8000);
            ++m_filledColumns;
            std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
            m_pendingUploads.push_back({m_writeColumn, 0, 0});
        }
        m_filledColumns = std::min(m_filledColumns, m_gridWidth);
    }
    m_timeframeMs = timeframeMs;
    if (!sameBucketUpdate || m_writeColumn < 0) {
        m_writeColumn = (m_writeColumn + 1) % m_gridWidth;
        if (m_filledColumns < m_gridWidth) {
            ++m_filledColumns;
        }
    }

    auto& slot = m_columns[static_cast<size_t>(m_writeColumn)];
    slot.bucketStartMs = bucketStartMs;
    slot.bucketEndMs = bucketEndMs;
    slot.timeframeMs = timeframeMs;
    slot.valid = true;
    const auto* src = reinterpret_cast<const uchar*>(deltaLevelsQ16.constData());
    for (int y = 0; y < gridHeight; ++y) {
        slot.deltaQ16[static_cast<size_t>(y)] = qFromLittleEndian<uint16_t>(src + (y * sizeof(uint16_t)));
    }
    m_lastSliceStartMs = bucketStartMs;

    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pendingUploads.push_back(PendingUpload{m_writeColumn, bucketStartMs, bucketEndMs});
    }
    return true;
}

FootprintStreamState::Snapshot FootprintStreamState::snapshot() const {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snap.gridWidth = m_gridWidth;
        snap.gridHeight = m_gridHeight;
        snap.writeColumn = m_writeColumn;
        snap.filledColumns = m_filledColumns;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeframeMs = m_timeframeMs;
        snap.minPrice = m_minPrice;
        snap.maxPrice = m_maxPrice;
        snap.tickSize = m_tickSize;
        snap.rangeValid = m_rangeValid;
        snap.resetGeneration = m_resetGeneration;
    }
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        snap.pendingUploads = static_cast<int>(m_pendingUploads.size());
    }
    return snap;
}

int FootprintStreamState::pendingUploadCount() const {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    return static_cast<int>(m_pendingUploads.size());
}

void FootprintStreamState::takePendingUploads(std::vector<PendingUpload>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

bool FootprintStreamState::copyColumnForUpload(int x, QByteArray& out) const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth <= 0 || m_gridHeight <= 0) {
        out.clear();
        return false;
    }
    if (x < 0 || x >= m_gridWidth) {
        out.clear();
        return false;
    }
    const auto& slot = m_columns[static_cast<size_t>(x)];
    if (!slot.valid || slot.deltaQ16.size() != static_cast<size_t>(m_gridHeight)) {
        out.clear();
        return false;
    }

    const int expectedBytes = expectedBytesForGridHeight(m_gridHeight);
    if (expectedBytes <= 0) {
        out.clear();
        return false;
    }
    out.resize(expectedBytes);
    auto* dst = reinterpret_cast<uchar*>(out.data());
    for (int y = 0; y < m_gridHeight; ++y) {
        qToLittleEndian<uint16_t>(slot.deltaQ16[static_cast<size_t>(y)], dst + (y * sizeof(uint16_t)));
    }
    return true;
}

void FootprintStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_writeColumn = -1;
    m_filledColumns = 0;
    m_lastSliceStartMs = 0;
    m_timeframeMs = 0;
    m_minPrice = 0.0;
    m_maxPrice = 0.0;
    m_tickSize = 0.0;
    m_rangeValid = false;
    ++m_resetGeneration;

    m_columns.assign(static_cast<size_t>(gridWidth), ColumnSlot{});
    for (auto& slot : m_columns) {
        slot.deltaQ16.assign(static_cast<size_t>(gridHeight), 0u);
    }

    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pendingUploads.clear();
    }
}
