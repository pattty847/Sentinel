/*
 * Sentinel – TpoStreamState
 *
 * CPU-side ring storage for TPO profile columns before GPU upload.
 *
 * Key fixes vs. original stub:
 *  1. Stores the actual letter byte from the server instead of a 0x7F sentinel.
 *  2. Supports both HorizontalProfile (rank-indexed) and VerticalTimeline modes.
 *  3. Proper session boundary alignment using SessionManager.
 *
 * Threading: Ingest on data thread; snapshot / reads must take the internal lock.
 */
#include "TpoStreamState.hpp"
#include "TpoDebugTrace.hpp"
#include "SentinelLogging.hpp"

#include "../../core/servermodel/SessionManager.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

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
}

// ─────────────────────────────────────────────────────────────────────────────
//  Session configuration
// ─────────────────────────────────────────────────────────────────────────────

void TpoStreamState::setSessionMs(int64_t sessionMs) {
    if (sessionMs > 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessionMs = sessionMs;
    }
}

void TpoStreamState::setSessionType(int sessionType) {
    const auto type = static_cast<SessionManager::SessionType>(sessionType);
    const int64_t dur = SessionManager::sessionDurationMs(type);
    if (dur > 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessionMs = dur;
        m_sessionType = sessionType;
    }
}

void TpoStreamState::setDisplayMode(DisplayMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_displayMode != mode) {
        m_displayMode = mode;
        // Reset so the new mode's column layout is built from scratch.
        if (m_gridWidth > 0 && m_gridHeight > 0) {
            resetLocked(m_gridWidth, m_gridHeight);
        }
    }
}

TpoStreamState::DisplayMode TpoStreamState::displayMode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_displayMode;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public interface
// ─────────────────────────────────────────────────────────────────────────────

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
    // Number of time periods per session = session duration / bucket duration.
    const int sessionPeriods = static_cast<int>(std::max<int64_t>(1, sessionMs / timeframeMs));
    if (sessionPeriods <= 0) {
        return false;
    }

    // ── Grid resize if session period count changed ──────────────────────────
    const int targetWidth = (m_displayMode == DisplayMode::VerticalTimeline)
        ? sessionPeriods
        : gridWidth;

    if (m_gridWidth != targetWidth || m_gridHeight != gridHeight) {
        resetLocked(targetWidth, gridHeight);
    }

    // ── Session boundary alignment ──────────────────────────────────────────
    // Match server boundary semantics (SessionManager::sessionContaining).
    const auto boundary = SessionManager::sessionContaining(
        bucketStartMs,
        static_cast<SessionManager::SessionType>(m_sessionType));
    const int64_t sessionStartMs = boundary.valid
        ? boundary.startMs
        : ((bucketStartMs / sessionMs) * sessionMs);
    const int64_t sessionEndMs = boundary.valid
        ? boundary.endMs
        : (sessionStartMs + sessionMs);

    if (m_sessionStartMs <= 0) {
        m_sessionStartMs = sessionStartMs;
        m_sessionEndMs   = sessionEndMs;
    }
    if (sessionStartMs != m_sessionStartMs) {
        // New session → reset all accumulated data.
        resetLocked(m_gridWidth, gridHeight);
        m_sessionStartMs = sessionStartMs;
        m_sessionEndMs   = sessionEndMs;
    }

    const int periodIdx = static_cast<int>((bucketStartMs - m_sessionStartMs) / timeframeMs);
    if (periodIdx < 0) {
        return false;
    }
    const int boundedPeriod = std::min(periodIdx, m_gridWidth - 1);

    std::unordered_set<int> touchedColumns;
    touchedColumns.reserve(static_cast<size_t>(std::min(gridHeight, 256)));

    if (m_displayMode == DisplayMode::VerticalTimeline) {
        // ── Mode B: time-period-indexed columns ─────────────────────────────
        // Column X in the texture = session period X; row Y = price level Y.
        // Each ingest simply overwrites the column for this period.
        QByteArray& col = m_columns[static_cast<size_t>(boundedPeriod)];
        if (col.size() != gridHeight) {
            col = QByteArray(gridHeight, '\0');
        }
        const RowStats beforeStats = summarizeRows(col);
        const RowStats incomingStats = summarizeRows(data);
        if (incomingStats.occupied == 0 && beforeStats.occupied > 0) {
            sentinel::log_file::appendLine(
                "/tmp/sentinel_tpo_client.log",
                QString("TPO ingest bucket preserved existing rows: start=%1 end=%2 tfMs=%3 period=%4 incomingRows=0 prevRows=%5 prevSpan=[%6..%7]")
                    .arg(bucketStartMs)
                    .arg(bucketEndMs)
                    .arg(timeframeMs)
                    .arg(boundedPeriod)
                    .arg(beforeStats.occupied)
                    .arg(beforeStats.firstRow)
                    .arg(beforeStats.lastRow));
            m_writeColumn = boundedPeriod;
            if (m_filledColumns < m_gridWidth) {
                m_filledColumns = std::max(m_filledColumns, boundedPeriod + 1);
            }
            m_timeframeMs = timeframeMs;
            m_lastSliceStartMs = bucketStartMs;
            return false;
        }
        bool changed = false;
        for (int row = 0; row < gridHeight; ++row) {
            const char letter = data.at(row);
            if (col.at(row) != letter) {
                col[row]  = letter;
                changed   = true;
            }
        }
        if (changed) {
            touchedColumns.insert(boundedPeriod);
        }
        const RowStats afterStats = summarizeRows(col);
        sentinel::log_file::appendLine(
            "/tmp/sentinel_tpo_client.log",
            QString("TPO ingest bucket: start=%1 end=%2 tfMs=%3 period=%4 incomingRows=%5 incomingSpan=[%6..%7] prevRows=%8 prevSpan=[%9..%10] resultRows=%11 resultSpan=[%12..%13] shrank=%14")
                .arg(bucketStartMs)
                .arg(bucketEndMs)
                .arg(timeframeMs)
                .arg(boundedPeriod)
                .arg(incomingStats.occupied)
                .arg(incomingStats.firstRow)
                .arg(incomingStats.lastRow)
                .arg(beforeStats.occupied)
                .arg(beforeStats.firstRow)
                .arg(beforeStats.lastRow)
                .arg(afterStats.occupied)
                .arg(afterStats.firstRow)
                .arg(afterStats.lastRow)
                .arg((beforeStats.occupied > afterStats.occupied) ? 1 : 0));
    } else {
        // ── Mode A: rank-indexed horizontal profile ──────────────────────────
        // Column[rank] holds all price levels that have been visited at least
        // (rank+1) unique time-periods within the session.
        // The actual letter byte is preserved so the renderer can colour-code
        // by letter bracket.
        for (int row = 0; row < gridHeight; ++row) {
            const char letter = data.at(row);
            if (letter == '\0') {
                continue;
            }

            const int64_t tickKey = static_cast<int64_t>(row);
            auto& level = m_levelsByTick[tickKey];
            level.row = row;

            // Skip if this period already contributed to this level.
            if (!level.periodIndices.empty() && level.periodIndices.back() == periodIdx) {
                continue;
            }

            level.periodIndices.push_back(periodIdx);
            level.letters.push_back(letter);  // ← store actual letter (was 0x7F)

            const int rank = static_cast<int>(level.periodIndices.size()) - 1;
            if (rank < 0 || rank >= m_gridWidth) {
                continue;
            }

            QByteArray& column = m_columns[static_cast<size_t>(rank)];
            if (column.size() != m_gridHeight) {
                column = QByteArray(m_gridHeight, '\0');
            }
            if (row >= 0 && row < column.size()) {
                column[row] = letter;          // ← was hardcoded 0x7F
                touchedColumns.insert(rank);
            }
        }
    }

    m_writeColumn = boundedPeriod;
    if (m_filledColumns < m_gridWidth) {
        m_filledColumns = std::max(m_filledColumns, boundedPeriod + 1);
    }
    m_timeframeMs      = timeframeMs;
    m_lastSliceStartMs = bucketStartMs;

    if (tpo_debug::enabled()) {
        std::ostringstream payload;
        payload << "{"
                << "\"bucketStartMs\":" << bucketStartMs
                << ",\"bucketEndMs\":" << bucketEndMs
                << ",\"timeframeMs\":" << timeframeMs
                << ",\"sessionType\":" << m_sessionType
                << ",\"sessionStartMs\":" << m_sessionStartMs
                << ",\"sessionEndMs\":" << m_sessionEndMs
                << ",\"periodIdx\":" << periodIdx
                << ",\"boundedPeriod\":" << boundedPeriod
                << ",\"gridWidth\":" << m_gridWidth
                << ",\"gridHeight\":" << m_gridHeight
                << ",\"filledColumns\":" << m_filledColumns
                << "}";
        tpo_debug::append("TpoStreamState.cpp:ingestSlice",
                          "tpo_period_alignment",
                          "H3",
                          payload.str());
    }

    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        for (const int col : touchedColumns) {
            if (col < 0 || col >= m_gridWidth) {
                continue;
            }
            m_pending.push_back(PendingUpload{
                col, bucketStartMs, bucketEndMs,
                m_columns[static_cast<size_t>(col)]});
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
    snap.gridWidth        = m_gridWidth;
    snap.gridHeight       = m_gridHeight;
    snap.filledColumns    = m_filledColumns;
    snap.writeColumn      = m_writeColumn;
    snap.lastSliceStartMs = m_lastSliceStartMs;
    snap.sessionStartMs   = m_sessionStartMs;
    snap.sessionEndMs     = m_sessionEndMs;
    snap.timeframeMs      = m_timeframeMs;
    snap.displayMode      = m_displayMode;
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        snap.pendingUploads = static_cast<int>(m_pending.size());
    }
    return snap;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private reset helper
// ─────────────────────────────────────────────────────────────────────────────

TpoStreamState::PocVahVal TpoStreamState::computePocVahVal() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gridWidth <= 0 || m_gridHeight <= 0 || m_columns.empty()) {
        return {};
    }

    // Build per-row histogram: count how many columns have a non-null letter at each row.
    std::vector<int> hist(static_cast<size_t>(m_gridHeight), 0);
    int total = 0;
    for (const auto& col : m_columns) {
        if (col.size() != m_gridHeight) {
            continue;
        }
        for (int row = 0; row < m_gridHeight; ++row) {
            if (col.at(row) != '\0') {
                ++hist[static_cast<size_t>(row)];
                ++total;
            }
        }
    }
    if (total == 0) {
        return {};
    }

    // POC: row with the highest count.
    int pocRow = 0;
    int pocCount = 0;
    for (int row = 0; row < m_gridHeight; ++row) {
        if (hist[static_cast<size_t>(row)] > pocCount) {
            pocCount = hist[static_cast<size_t>(row)];
            pocRow = row;
        }
    }
    if (pocCount == 0) {
        return {};
    }

    // Value Area: expand from POC until accumulated count >= 70% of total.
    // Standard Market Profile rule: at each step, choose the direction (up or down)
    // whose next two rows sum to the larger value.
    const int target = static_cast<int>(std::ceil(total * 0.70));
    int accumulated = pocCount;
    int vahRow = pocRow;
    int valRow = pocRow;

    while (accumulated < target) {
        const int upRow   = vahRow - 1;
        const int downRow = valRow + 1;
        const bool canUp   = upRow >= 0;
        const bool canDown = downRow < m_gridHeight;

        if (!canUp && !canDown) {
            break;
        }

        // Sum next two rows in each direction (standard TPO value area convention).
        auto rowCount = [&](int r) -> int {
            if (r < 0 || r >= m_gridHeight) return 0;
            return hist[static_cast<size_t>(r)];
        };
        const int upSum   = canUp   ? (rowCount(upRow)   + rowCount(upRow - 1))   : -1;
        const int downSum = canDown ? (rowCount(downRow) + rowCount(downRow + 1)) : -1;

        if (!canDown || (canUp && upSum >= downSum)) {
            // Expand upward (decreasing row index = higher price).
            accumulated += rowCount(upRow);
            --vahRow;
        } else {
            // Expand downward.
            accumulated += rowCount(downRow);
            ++valRow;
        }
    }

    PocVahVal result;
    result.pocRow = pocRow;
    result.vahRow = vahRow;
    result.valRow = valRow;
    result.valid  = true;
    return result;
}

void TpoStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth        = gridWidth;
    m_gridHeight       = gridHeight;
    m_filledColumns    = 0;
    m_writeColumn      = -1;
    m_sessionStartMs   = 0;
    m_sessionEndMs     = 0;
    m_lastSliceStartMs = 0;
    m_timeframeMs      = 0;
    m_columns.assign(static_cast<size_t>(gridWidth), QByteArray(gridHeight, '\0'));
    m_levelsByTick.clear();
    {
        std::lock_guard<std::mutex> uploadLock(m_uploadMutex);
        m_pending.clear();
    }
}
