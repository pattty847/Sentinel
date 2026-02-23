#include "TpoStreamState.hpp"

void TpoStreamState::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gridWidth <= 0 || m_gridHeight <= 0) {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pending.clear();
        return;
    }
    resetLocked(m_gridWidth, m_gridHeight);
}

void TpoStreamState::reset(int gridWidth, int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    resetLocked(gridWidth, gridHeight);
}

bool TpoStreamState::ingestSlice(int64_t bucketStartMs,
                                 int64_t bucketEndMs,
                                 int64_t timeframeMs,
                                 int gridWidth,
                                 int gridHeight,
                                 const QByteArray& data) {
    if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0 ||
        gridWidth <= 0 || gridHeight <= 0 || data.size() != gridHeight) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gridWidth != gridWidth || m_gridHeight != gridHeight) {
        resetLocked(gridWidth, gridHeight);
    }

    const bool hasPrevious = (m_filledColumns > 0 && m_writeColumn >= 0);
    const bool sameBucketUpdate = hasPrevious && (bucketStartMs == m_lastSliceStartMs);
    bool shouldReset = false;
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
    }

    if (!sameBucketUpdate || m_writeColumn < 0) {
        m_writeColumn = (m_writeColumn + 1) % m_gridWidth;
        if (m_filledColumns < m_gridWidth) {
            ++m_filledColumns;
        }
    }

    m_columns[static_cast<size_t>(m_writeColumn)] = data;
    m_timeframeMs = timeframeMs;
    m_lastSliceStartMs = bucketStartMs;
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pending.push_back(PendingUpload{m_writeColumn, bucketStartMs, bucketEndMs, data});
    }
    return true;
}

void TpoStreamState::takePendingUploads(std::vector<PendingUpload>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pending.empty()) {
        out.swap(m_pending);
    }
}

TpoStreamState::Snapshot TpoStreamState::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot snap;
    snap.gridWidth = m_gridWidth;
    snap.gridHeight = m_gridHeight;
    snap.filledColumns = m_filledColumns;
    snap.writeColumn = m_writeColumn;
    snap.lastSliceStartMs = m_lastSliceStartMs;
    snap.timeframeMs = m_timeframeMs;
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        snap.pendingUploads = static_cast<int>(m_pending.size());
    }
    return snap;
}

void TpoStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_filledColumns = 0;
    m_writeColumn = -1;
    m_lastSliceStartMs = 0;
    m_timeframeMs = 0;
    m_columns.assign(static_cast<size_t>(gridWidth), QByteArray(gridHeight, '\0'));
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pending.clear();
    }
}

