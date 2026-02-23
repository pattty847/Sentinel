#include "TpoStreamState.hpp"

void TpoStreamState::reset(int gridWidth, int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_filledColumns = 0;
    m_writeColumn = 0;
    m_timeframeMs = 0;
    m_columns.assign(static_cast<size_t>(gridWidth), QByteArray{});
    m_pending.clear();
}

bool TpoStreamState::ingestColumn(int x,
                                  int gridWidth,
                                  int gridHeight,
                                  int64_t timeframeMs,
                                  const QByteArray& data) {
    if (x < 0 || gridWidth <= 0 || gridHeight <= 0 || x >= gridWidth || data.isEmpty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gridWidth != gridWidth || m_gridHeight != gridHeight) {
        m_gridWidth = gridWidth;
        m_gridHeight = gridHeight;
        m_columns.assign(static_cast<size_t>(gridWidth), QByteArray{});
        m_pending.clear();
        m_filledColumns = 0;
        m_writeColumn = 0;
    }
    m_columns[static_cast<size_t>(x)] = data;
    m_writeColumn = x;
    m_timeframeMs = timeframeMs;
    if (m_filledColumns < m_gridWidth) {
        ++m_filledColumns;
    }
    m_pending.push_back(PendingUpload{x, data});
    return true;
}

void TpoStreamState::takePendingUploads(std::vector<PendingUpload>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
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
    snap.timeframeMs = m_timeframeMs;
    return snap;
}

