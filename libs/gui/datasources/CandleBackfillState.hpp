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
    static constexpr qint64 kThrottleMs = 100;
    static constexpr int kMaxEmptyPages = 3;
    static constexpr qint64 kFloorRetryMs = 60'000;
    static constexpr qint64 kDefaultEmptyLookbackSec = 7 * 24 * 60 * 60;
    explicit CandleBackfillState(qint64 emptyLookbackSec = kDefaultEmptyLookbackSec)
        : m_emptyLookbackSec(std::max<qint64>(2, emptyLookbackSec)) {}
    struct Request {
        QString symbol;
        qint64 timeframeSec = 0;
        qint64 startSec = 0;
        qint64 endSec = 0;
        int limit = 0;
        quint64 generation = 0;
        qint64 boundarySec = 0; // exclusive progress boundary, independent of wire end
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
        if (m_pending || !needsOlderData(oldestMs, cacheFull, nowMs) ||
            retryDelayMs(nowMs) > 0) return std::nullopt;
        auto& history = m_history[{m_symbol, m_tfSec}];
        if (history.emptyPages >= kMaxEmptyPages) {
            // Resume the same scan after the pause, never revisit its empty prefix.
            history.emptyPages = 0;
            history.retryAfterMs = 0;
        }
        const qint64 nowEnd = ((nowMs / 1000) / m_tfSec + 1) * m_tfSec;
        qint64 boundary = oldestMs > 0 ? oldestMs / 1000 : std::min(m_endSec, nowEnd);
        if (history.cursorSec > 0) boundary = std::min(boundary, history.cursorSec);
        if (boundary <= m_startSec && history.emptyScanStartSec == 0) return std::nullopt;
        const qint64 pageCap = std::clamp<qint64>(350 * 60 / m_tfSec, 1, 350);
        // After an empty window, probe full pages beyond it (bounded below).
        const qint64 bars = history.emptyScanStartSec > 0 ? pageCap
            : (boundary - m_startSec + m_tfSec - 1) / m_tfSec;
        int limit = static_cast<int>(std::min(pageCap, bars));
        // 1s history uses inclusive end and retained-bar count. Exclude the
        // oldest loaded bucket explicitly; REST rollup windows already exclude it.
        const qint64 end = boundary - (m_tfSec == 1 ? 1 : 0);
        if (m_tfSec == 1) {
            const qint64 scanStart = history.emptyScanStartSec > 0 ? history.emptyScanStartSec : boundary;
            const qint64 scanFloor = std::max<qint64>(0, scanStart - m_emptyLookbackSec);
            limit = static_cast<int>(std::min<qint64>(limit, end - scanFloor));
            if (limit <= 0) return std::nullopt;
        }
        const qint64 start = std::max<qint64>(0, end - limit * m_tfSec);
        if (end <= 0 || (m_tfSec != 1 && start <= 0)) return std::nullopt;
        m_pending = Request{m_symbol, m_tfSec, start, end, limit, m_generation, boundary};
        m_nextSendMs = nowMs + kThrottleMs;
        return m_pending;
    }

    bool accept(const QString& symbol, qint64 tfSec, qint64 startSec, qint64 endSec,
                qint64 oldestReplyMs, qint64 nowMs) {
        if (!m_pending || m_pending->symbol != symbol || m_pending->timeframeSec != tfSec ||
            m_pending->startSec != startSec || m_pending->endSec != endSec) return false;
        const bool current = m_pending->generation == m_generation;
        const auto boundary = m_pending->boundarySec;
        m_pending.reset();
        if (!current) return false;
        auto& history = m_history[{symbol, tfSec}];
        if (oldestReplyMs > 0 && oldestReplyMs / 1000 < boundary) {
            history = {oldestReplyMs / 1000, 0, 0, 0};
        } else {
            if (history.emptyScanStartSec == 0) history.emptyScanStartSec = boundary;
            history.cursorSec = startSec;
            if (++history.emptyPages >= kMaxEmptyPages)
                history.retryAfterMs = nowMs + kFloorRetryMs;
        }
        return true;
    }

    bool scanPaused() const {
        const auto it = m_history.find({m_symbol, m_tfSec});
        return it != m_history.end() && it->second.emptyPages >= kMaxEmptyPages;
    }

    bool fail(const QString& symbol, qint64 nowMs) {
        if (!m_pending || m_pending->symbol != symbol) return false;
        m_pending.reset();
        m_retryAfterMs = nowMs + 2000;
        return true;
    }

    qint64 retryDelayMs(qint64 nowMs) const {
        qint64 deadline = std::max(m_retryAfterMs, m_nextSendMs);
        const auto it = m_history.find({m_symbol, m_tfSec});
        if (it != m_history.end() && !emptyLookbackExhausted(it->second))
            deadline = std::max(deadline, it->second.retryAfterMs);
        return std::max<qint64>(0, deadline - nowMs);
    }

    bool needsOlderData(qint64 oldestMs, bool cacheFull, qint64 nowMs) const {
        if (m_symbol.isEmpty() || m_tfSec <= 0 || m_endSec <= m_startSec || cacheFull) return false;
        const qint64 nowEnd = ((nowMs / 1000) / m_tfSec + 1) * m_tfSec;
        const qint64 loadedEdge = oldestMs > 0 ? oldestMs / 1000 : std::min(m_endSec, nowEnd);
        if (loadedEdge <= m_startSec) return false;
        const auto it = m_history.find({m_symbol, m_tfSec});
        return it == m_history.end() || !emptyLookbackExhausted(it->second);
    }

    void disconnect() {
        ++m_generation;
        m_pending.reset();
        m_retryAfterMs = 0;
        m_nextSendMs = 0;
        m_history.clear();
        m_startSec = m_endSec = 0;
    }

private:
    QString m_symbol;
    qint64 m_tfSec = 0, m_startSec = 0, m_endSec = 0, m_retryAfterMs = 0;
    qint64 m_nextSendMs = 0;
    qint64 m_emptyLookbackSec;
    quint64 m_generation = 0;
    std::optional<Request> m_pending;
    struct History {
        qint64 cursorSec = 0;
        int emptyPages = 0;
        qint64 retryAfterMs = 0;
        qint64 emptyScanStartSec = 0;
    };
    bool emptyLookbackExhausted(const History& history) const {
        return m_tfSec == 1 && history.emptyScanStartSec > 0 &&
            history.cursorSec <= std::max<qint64>(0, history.emptyScanStartSec - m_emptyLookbackSec) + 1;
    }
    std::map<std::pair<QString, qint64>, History> m_history;
};
