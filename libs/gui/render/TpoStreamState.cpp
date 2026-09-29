/*
 * Sentinel – TpoStreamState
 *
 * GUI-side store of TPO letter columns per session period, for the most recent
 * sessions. See the header for the contract.
 */
#include "TpoStreamState.hpp"
#include "SentinelLogging.hpp"

#include "../../core/servermodel/SessionManager.hpp"

#include <algorithm>

namespace {
struct RowStats {
    int occupied = 0;
    int firstRow = -1;
    int lastRow = -1;
};

RowStats summarizeRows(const QByteArray& data) {
    RowStats stats;
    for (int i = 0; i < data.size(); ++i) {
        if (data.at(i) == '\0') {
            continue;
        }
        ++stats.occupied;
        if (stats.firstRow < 0) {
            stats.firstRow = i;
        }
        stats.lastRow = i;
    }
    return stats;
}
} // namespace

void TpoStreamState::setSessionType(int sessionType) {
    if (sessionType < 0 || sessionType > static_cast<int>(SessionManager::SessionType::M1)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessionType != sessionType) {
        m_sessionType = sessionType;
        clearLocked();
    }
}

void TpoStreamState::setMaxSessions(int sessions) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxSessions = std::clamp(sessions, 1, kMaxSessionsLimit);
    while (static_cast<int>(m_sessions.size()) > m_maxSessions) {
        m_sessions.erase(m_sessions.begin());
    }
}

int TpoStreamState::maxSessions() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxSessions;
}

void TpoStreamState::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    clearLocked();
}

void TpoStreamState::reset(int /*gridWidth*/, int gridHeight) {
    if (gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    clearLocked();
    m_gridHeight = gridHeight;
}

bool TpoStreamState::ingestSlice(int64_t bucketStartMs,
                                 int64_t bucketEndMs,
                                 int64_t timeframeMs,
                                 int /*gridWidth*/,
                                 int gridHeight,
                                 const QByteArray& data) {
    if (bucketStartMs <= 0 || bucketEndMs <= bucketStartMs || timeframeMs <= 0 ||
        gridHeight <= 0 || data.size() != gridHeight) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    // Match server boundary semantics (SessionManager::sessionContaining).
    const auto boundary = SessionManager::sessionContaining(
        bucketStartMs, static_cast<SessionManager::SessionType>(m_sessionType));
    if (!boundary.valid || boundary.endMs <= boundary.startMs) {
        return false;
    }
    // A completed session must never fold later buckets into its last column.
    if (bucketStartMs < boundary.startMs || bucketStartMs >= boundary.endMs ||
        bucketEndMs > boundary.endMs) {
        return false;
    }
    const int64_t periods64 = (boundary.endMs - boundary.startMs + timeframeMs - 1) / timeframeMs;
    if (periods64 <= 0 || periods64 > 4096) {
        sLog_DataN(5000, "TPO bucket dropped: session has " << periods64 << " periods tf=" << timeframeMs);
        return false;
    }
    const int periods = static_cast<int>(periods64);
    const int period = static_cast<int>((bucketStartMs - boundary.startMs) / timeframeMs);

    if (m_gridHeight != gridHeight || (m_timeframeMs != 0 && m_timeframeMs != timeframeMs)) {
        clearLocked();
        m_gridHeight = gridHeight;
    }

    auto it = m_sessions.find(boundary.startMs);
    if (it == m_sessions.end()) {
        if (static_cast<int>(m_sessions.size()) >= m_maxSessions &&
            boundary.startMs < m_sessions.begin()->first) {
            sLog_DataN(5000, "TPO bucket dropped: session older than retained window start="
                       << boundary.startMs << " oldest=" << m_sessions.begin()->first);
            return false;
        }
        Session session;
        session.endMs = boundary.endMs;
        session.columns.assign(static_cast<size_t>(periods), QByteArray());
        it = m_sessions.emplace(boundary.startMs, std::move(session)).first;
        sLog_Data("TPO session added: start=" << boundary.startMs << " end=" << boundary.endMs
                  << " sessionType=" << m_sessionType << " periods=" << periods
                  << " retained=" << m_sessions.size());
        while (static_cast<int>(m_sessions.size()) > m_maxSessions) {
            m_sessions.erase(m_sessions.begin());
        }
    }

    QByteArray& col = it->second.columns[static_cast<size_t>(period)];
    if (col.size() != gridHeight) {
        col = QByteArray(gridHeight, '\0');
    }
    const RowStats incomingStats = summarizeRows(data);
    m_timeframeMs = timeframeMs;
    m_lastSliceStartMs = std::max(m_lastSliceStartMs, bucketStartMs);
    if (incomingStats.occupied == 0) {
        // An empty refresh never erases rows a previous publication established.
        sLog_Probe("tpo.ingest", "empty slice ignored: start=" << bucketStartMs
                   << " period=" << period);
        return false;
    }
    if (col == data) {
        return true;
    }
    col = data;
    sLog_Probe("tpo.ingest", "bucket: start=" << bucketStartMs << " end=" << bucketEndMs
               << " tf=" << timeframeMs << " session=" << boundary.startMs
               << " period=" << period << "/" << periods
               << " rows=" << incomingStats.occupied
               << " span=[" << incomingStats.firstRow << ".." << incomingStats.lastRow << "]");
    m_pending.push_back(PendingUpload{period, periods, boundary.startMs, boundary.endMs,
                                      bucketStartMs, bucketEndMs, col});
    return true;
}

void TpoStreamState::takePendingUploads(std::vector<PendingUpload>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out.clear();
    out.swap(m_pending);
}

TpoStreamState::Snapshot TpoStreamState::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot snap;
    snap.gridHeight = m_gridHeight;
    snap.sessions = static_cast<int>(m_sessions.size());
    if (!m_sessions.empty()) {
        snap.latestSessionStartMs = m_sessions.rbegin()->first;
        snap.latestSessionEndMs = m_sessions.rbegin()->second.endMs;
    }
    snap.lastSliceStartMs = m_lastSliceStartMs;
    snap.timeframeMs = m_timeframeMs;
    snap.pendingUploads = static_cast<int>(m_pending.size());
    return snap;
}

void TpoStreamState::clearLocked() {
    m_sessions.clear();
    m_pending.clear();
    m_timeframeMs = 0;
    m_lastSliceStartMs = 0;
}
