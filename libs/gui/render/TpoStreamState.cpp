#include "TpoStreamState.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

    const int64_t sessionMs = std::max<int64_t>(m_sessionMs, timeframeMs);
    const int sessionPeriods = static_cast<int>(std::max<int64_t>(1, sessionMs / timeframeMs));
    if (sessionPeriods <= 0) {
        return false;
    }
    if (m_gridWidth != sessionPeriods || m_gridHeight != gridHeight) {
        resetLocked(sessionPeriods, gridHeight);
    }

    const int64_t sessionStartMs = (bucketStartMs / sessionMs) * sessionMs;
    if (m_sessionStartMs <= 0) {
        m_sessionStartMs = sessionStartMs;
    }
    if (sessionStartMs != m_sessionStartMs) {
        resetLocked(sessionPeriods, gridHeight);
        m_sessionStartMs = sessionStartMs;
    }

    const int periodIdx = static_cast<int>((bucketStartMs - m_sessionStartMs) / timeframeMs);
    if (periodIdx < 0) {
        return false;
    }
    // Clamp period index to visible session width.
    const int boundedPeriod = std::min(periodIdx, m_gridWidth - 1);

    std::unordered_set<int> touchedColumns;
    touchedColumns.reserve(static_cast<size_t>(std::min(gridHeight, 256)));
    for (int row = 0; row < gridHeight; ++row) {
        if (data.at(row) == '\0') {
            continue;
        }
        const int64_t tickKey = static_cast<int64_t>(row);
        auto& level = m_levelsByTick[tickKey];
        level.row = row;
        if (!level.periodIndices.empty() && level.periodIndices.back() == periodIdx) {
            continue;
        }
        level.periodIndices.push_back(periodIdx);
        const int rank = static_cast<int>(level.periodIndices.size()) - 1;
        if (rank < 0 || rank >= m_gridWidth) {
            continue;
        }
        QByteArray& column = m_columns[static_cast<size_t>(rank)];
        if (column.size() != m_gridHeight) {
            column = QByteArray(m_gridHeight, '\0');
        }
        if (row >= 0 && row < column.size()) {
            column[row] = static_cast<char>(0x7F);
            touchedColumns.insert(rank);
        }
    }

    // Keep write column for debug/snapshots aligned to latest period slot.
    m_writeColumn = boundedPeriod;
    if (m_filledColumns < m_gridWidth) {
        m_filledColumns = std::max(m_filledColumns, boundedPeriod + 1);
    }

    m_timeframeMs = timeframeMs;
    m_lastSliceStartMs = bucketStartMs;
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        for (const int col : touchedColumns) {
            if (col < 0 || col >= m_gridWidth) {
                continue;
            }
            m_pending.push_back(PendingUpload{col, bucketStartMs, bucketEndMs, m_columns[static_cast<size_t>(col)]});
        }
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
    m_sessionStartMs = 0;
    m_lastSliceStartMs = 0;
    m_timeframeMs = 0;
    m_columns.assign(static_cast<size_t>(gridWidth), QByteArray(gridHeight, '\0'));
    m_levelsByTick.clear();
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pending.clear();
    }
}

