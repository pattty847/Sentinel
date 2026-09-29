#pragma once

#include <QString>
#include <algorithm>
#include <map>
#include <optional>

// GUI-thread paging policy, independent of timers/transport/rendering. The wire
// echoes symbol, timeframe and range (no request id). Keep the outstanding request
// even across selection changes so an A -> B -> A reply cannot match a new A job.
class CandleBackfillState {
public:
    struct Request {
        QString symbol;
        qint64 timeframeSec = 0;
        qint64 startSec = 0;
        qint64 endSec = 0;
        int limit = 0;
        quint64 generation = 0;
    };

    bool setViewport(const QString& symbol, qint64 tfSec, qint64 startMs, qint64 endMs) {
        bool changed = symbol != m_symbol || tfSec != m_tfSec;
        if (changed) ++m_generation;
        m_symbol = symbol;
        m_tfSec = tfSec;
        qint64 start = 0, end = 0;
        if (tfSec > 0 && startMs > 0 && endMs > startMs) {
            const qint64 span = endMs - startMs;
            const qint64 tfMs = tfSec * 1000;
            start = (std::max<qint64>(0, startMs - span) / tfMs) * tfSec;
            end = ((endMs - 1) / tfMs + 1) * tfSec;
        }
        changed = changed || start != m_startSec || end != m_endSec;
        m_startSec = start;
        m_endSec = end;
        return changed;
    }

    std::optional<Request> next(qint64 oldestMs, bool cacheFull, qint64 nowMs) {
        if (m_pending || m_symbol.isEmpty() || m_tfSec <= 0 || m_endSec <= m_startSec ||
            cacheFull || nowMs < m_retryAfterMs) return std::nullopt;
        // Never bootstrap a wholly future range: it would falsely establish a floor.
        const qint64 nowEnd = ((nowMs / 1000) / m_tfSec + 1) * m_tfSec;
        const qint64 end = oldestMs > 0 ? oldestMs / 1000 : std::min(m_endSec, nowEnd);
        const auto floor = m_floors.find({m_symbol, m_tfSec});
        if (end <= m_startSec || (floor != m_floors.end() && (oldestMs == 0 || end <= floor->second)))
            return std::nullopt;
        const qint64 bars = (end - m_startSec + m_tfSec - 1) / m_tfSec;
        // Aim for one 350-minute REST page (at least one output bucket). A
        // 350-bar daily request otherwise holds the single flight for 1,440 REST
        // calls, delaying selection changes and live-history repair for minutes.
        const qint64 pageCap = std::clamp<qint64>(350 * 60 / m_tfSec, 1, 350);
        const int limit = static_cast<int>(std::min(pageCap, bars));
        const qint64 start = end - limit * m_tfSec;
        if (start <= 0) return std::nullopt;
        m_pending = Request{m_symbol, m_tfSec, start, end, limit, m_generation};
        return m_pending;
    }

    bool accept(const QString& symbol, qint64 tfSec, qint64 startSec, qint64 endSec,
                qint64 oldestReplyMs) {
        if (!m_pending || m_pending->symbol != symbol || m_pending->timeframeSec != tfSec ||
            m_pending->startSec != startSec || m_pending->endSec != endSec) return false;
        const bool current = m_pending->generation == m_generation;
        m_pending.reset();
        if (!current) return false;
        // Empty or overlap-only success proves no progress; errors never set a floor.
        if (oldestReplyMs <= 0 || oldestReplyMs / 1000 >= endSec)
            m_floors[{symbol, tfSec}] = endSec;
        return true;
    }

    bool fail(const QString& symbol, qint64 nowMs) {
        if (!m_pending || m_pending->symbol != symbol) return false;
        m_pending.reset();
        m_retryAfterMs = nowMs + 2000;
        return true;
    }

    qint64 retryDelayMs(qint64 nowMs) const {
        return std::max<qint64>(0, m_retryAfterMs - nowMs);
    }

    void disconnect() {
        ++m_generation;
        m_pending.reset();
        m_retryAfterMs = 0;
        m_floors.clear();
        m_startSec = m_endSec = 0;
    }

private:
    QString m_symbol;
    qint64 m_tfSec = 0, m_startSec = 0, m_endSec = 0, m_retryAfterMs = 0;
    quint64 m_generation = 0;
    std::optional<Request> m_pending;
    std::map<std::pair<QString, qint64>, qint64> m_floors;
};
