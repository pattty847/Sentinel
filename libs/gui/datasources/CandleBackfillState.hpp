#pragma once

#include <QString>
#include "../chart/core/CandleInterval.hpp"
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
    static constexpr qint64 kDefaultEmptyLookbackMs = 7 * 24 * 60 * 60 * 1000LL;
    // A request the server answered "warming" is resent verbatim after this.
    static constexpr qint64 kWarmingRetryMs = 2000;
    explicit CandleBackfillState(qint64 emptyLookbackMs = kDefaultEmptyLookbackMs)
        : m_emptyLookbackMs(std::max<qint64>(2000, emptyLookbackMs)) {}
    struct Request {
        QString symbol;
        qint64 timeframeMs = 0;
        qint64 startMs = 0;
        qint64 endMs = 0;
        int limit = 0;
        quint64 generation = 0;
        qint64 boundaryMs = 0; // exclusive progress boundary, independent of wire end
        bool refresh = false;
        // Answered "warming": not in flight, resent verbatim at retryAtMs.
        bool held = false;
        qint64 retryAtMs = 0;
    };

    bool setViewport(const QString& symbol, qint64 tfMs, qint64 startMs, qint64 endMs) {
        bool changed = symbol != m_symbol || tfMs != m_tfMs;
        if (changed) {
            // A held request belongs to the old selection; an in-flight one
            // stays until its reply arrives.
            if (m_pending && m_pending->held) m_pending.reset();
            ++m_generation;
            m_refreshPending = false;
            m_refreshCursorMs = 0;
        }
        m_symbol = symbol;
        m_tfMs = tfMs;
        qint64 start = 0, end = 0;
        if (tfMs > 0 && startMs > 0 && endMs > startMs) {
            const qint64 span = endMs - startMs;
            start = (std::max<qint64>(0, startMs - span) / tfMs) * tfMs;
            end = ((endMs - 1) / tfMs + 1) * tfMs;
        }
        changed = changed || start != m_startMs || end != m_endMs;
        m_startMs = start;
        m_endMs = end;
        return changed;
    }

    // One refresh pass, newest first, through the same page cap/flight/throttle.
    // Capture the visible range plus prefetch when its first valid page is sent.
    void requestRefresh() {
        if (m_tfMs <= 0) return;
        m_refreshPending = true;
        m_refreshCursorMs = 0;
    }

    std::optional<Request> next(qint64 oldestMs, bool cacheFull, qint64 nowMs) {
        if (m_pending && m_pending->held && m_pending->generation != m_generation) m_pending.reset();
        if (m_pending && m_pending->held) {
            if (!needsOlderData(oldestMs, cacheFull, nowMs) || retryDelayMs(nowMs) > 0) return std::nullopt;
            m_pending->held = false;
            m_pending->retryAtMs = 0;
            m_nextSendMs = nowMs + kThrottleMs;
            return m_pending;
        }
        if (m_pending || !needsOlderData(oldestMs, cacheFull, nowMs) ||
            retryDelayMs(nowMs) > 0) return std::nullopt;
        if (m_refreshPending) {
            if (m_refreshCursorMs == 0) {
                const qint64 nowEnd = (nowMs / m_tfMs + 1) * m_tfMs;
                m_refreshCursorMs = std::min(m_endMs, nowEnd);
                m_refreshStartMs = m_startMs;
            }
            const qint64 pageCap = 350;
            const qint64 bars =
                (m_refreshCursorMs - m_refreshStartMs + (chart::candleHistoryFromMemory(m_tfMs) ? 0 : m_tfMs - 1000)) /
                m_tfMs;
            const int limit = static_cast<int>(std::min(pageCap, bars));
            const qint64 end = m_refreshCursorMs - (chart::candleHistoryFromMemory(m_tfMs) ? m_tfMs : 0);
            const qint64 start = std::max<qint64>(0, end - limit * m_tfMs);
            if (limit <= 0 || end <= 0 || (!chart::candleHistoryFromMemory(m_tfMs) && start <= 0)) return std::nullopt;
            m_pending = Request{m_symbol, m_tfMs, start, end, limit, m_generation, m_refreshCursorMs, true};
            m_nextSendMs = nowMs + kThrottleMs;
            return m_pending;
        }
        auto& history = m_history[{m_symbol, m_tfMs}];
        if (history.emptyPages >= kMaxEmptyPages) {
            // Resume the same scan after the pause, never revisit its empty prefix.
            history.emptyPages = 0;
            history.retryAfterMs = 0;
        }
        const qint64 nowEnd = (nowMs / m_tfMs + 1) * m_tfMs;
        qint64 boundary = oldestMs > 0 ? oldestMs : std::min(m_endMs, nowEnd);
        if (history.cursorMs > 0) boundary = std::min(boundary, history.cursorMs);
        boundary = (boundary / m_tfMs) * m_tfMs;
        if (boundary <= m_startMs && history.emptyScanStartMs == 0) return std::nullopt;
        const qint64 pageCap = 350;
        // After an empty window, probe full pages beyond it (bounded below).
        const qint64 bars =
            history.emptyScanStartMs > 0
                ? pageCap
                : (boundary - m_startMs + (chart::candleHistoryFromMemory(m_tfMs) ? 0 : m_tfMs - 1000)) / m_tfMs;
        int limit = static_cast<int>(std::min(pageCap, bars));
        // Memory history uses inclusive end and retained-bar count. Exclude the
        // oldest loaded bucket explicitly; REST rollup windows already exclude it.
        const qint64 end = boundary - (chart::candleHistoryFromMemory(m_tfMs) ? m_tfMs : 0);
        if (chart::candleHistoryFromMemory(m_tfMs)) {
            const qint64 scanStart = history.emptyScanStartMs > 0 ? history.emptyScanStartMs : boundary;
            const qint64 scanFloor = std::max<qint64>(0, scanStart - m_emptyLookbackMs);
            limit = static_cast<int>(std::min<qint64>(limit, (end - scanFloor) / m_tfMs));
            if (limit <= 0) return std::nullopt;
        }
        const qint64 start = std::max<qint64>(0, end - limit * m_tfMs);
        if (end <= 0 || (!chart::candleHistoryFromMemory(m_tfMs) && start <= 0)) return std::nullopt;
        m_pending = Request{m_symbol, m_tfMs, start, end, limit, m_generation, boundary};
        m_nextSendMs = nowMs + kThrottleMs;
        return m_pending;
    }

    bool accept(const QString& symbol, qint64 tfMs, qint64 startMs, qint64 endMs,
                qint64 oldestReplyMs, qint64 nowMs) {
        if (!m_pending || m_pending->symbol != symbol || m_pending->timeframeMs != tfMs ||
            m_pending->startMs != startMs || m_pending->endMs != endMs) return false;
        const bool current = m_pending->generation == m_generation;
        const auto completed = *m_pending;
        const auto boundary = completed.boundaryMs;
        m_pending.reset();
        if (!current) return false;
        if (completed.refresh) {
            // Empty refresh windows still advance; they are not older-history floors.
            m_refreshCursorMs = chart::candleHistoryFromMemory(tfMs) && oldestReplyMs > 0
                                    ? std::min(boundary - tfMs, oldestReplyMs)
                                    : startMs + (chart::candleHistoryFromMemory(tfMs) ? m_tfMs : 0);
            m_refreshCursorMs = (m_refreshCursorMs / m_tfMs) * m_tfMs;
            if (m_refreshCursorMs <= m_refreshStartMs) m_refreshPending = false;
            return true;
        }
        auto& history = m_history[{symbol, tfMs}];
        if (oldestReplyMs > 0 && oldestReplyMs < boundary) {
            history = {oldestReplyMs, 0, 0, 0};
        } else {
            if (history.emptyScanStartMs == 0) history.emptyScanStartMs = boundary;
            history.cursorMs = startMs;
            if (++history.emptyPages >= kMaxEmptyPages)
                history.retryAfterMs = nowMs + kFloorRetryMs;
        }
        return true;
    }

    // A "warming" reply to the request in flight: it is no page and moves no
    // cursor. The current selection's request is held for kWarmingRetryMs; a
    // stale one is released. False when no request in flight matches.
    bool warming(const QString& symbol, qint64 tfMs, qint64 startMs, qint64 endMs, qint64 nowMs) {
        if (!m_pending || m_pending->held || m_pending->symbol != symbol || m_pending->timeframeMs != tfMs ||
            m_pending->startMs != startMs || m_pending->endMs != endMs) return false;
        if (m_pending->generation != m_generation) {
            m_pending.reset();
            return true;
        }
        m_pending->held = true;
        m_pending->retryAtMs = nowMs + kWarmingRetryMs;
        return true;
    }

    bool scanPaused() const {
        const auto it = m_history.find({m_symbol, m_tfMs});
        return it != m_history.end() && it->second.emptyPages >= kMaxEmptyPages;
    }

    bool fail(const QString& symbol, qint64 nowMs) {
        if (!m_pending || m_pending->symbol != symbol) return false;
        m_pending.reset();
        if (m_tfMs > 0) m_retryAfterMs = nowMs + 2000;
        return true;
    }

    qint64 retryDelayMs(qint64 nowMs) const {
        if (m_tfMs <= 0) return 0;
        qint64 deadline = std::max(m_retryAfterMs, m_nextSendMs);
        if (m_pending && m_pending->held && m_pending->generation == m_generation)
            deadline = std::max(deadline, m_pending->retryAtMs);
        const auto it = m_history.find({m_symbol, m_tfMs});
        if (!m_refreshPending && it != m_history.end() && !emptyLookbackExhausted(it->second))
            deadline = std::max(deadline, it->second.retryAfterMs);
        return std::max<qint64>(0, deadline - nowMs);
    }

    bool needsOlderData(qint64 oldestMs, bool cacheFull, qint64 nowMs) const {
        if (m_symbol.isEmpty() || m_tfMs <= 0 || m_endMs <= m_startMs) return false;
        const qint64 nowEnd = (nowMs / m_tfMs + 1) * m_tfMs;
        if (m_refreshPending) return m_refreshCursorMs > 0 || std::min(m_endMs, nowEnd) > m_startMs;
        if (cacheFull) return false;
        const qint64 loadedEdge = oldestMs > 0 ? oldestMs : std::min(m_endMs, nowEnd);
        if (loadedEdge <= m_startMs) return false;
        const auto it = m_history.find({m_symbol, m_tfMs});
        return it == m_history.end() || !emptyLookbackExhausted(it->second);
    }

    void disconnect() {
        ++m_generation;
        m_pending.reset();
        m_retryAfterMs = 0;
        m_nextSendMs = 0;
        m_history.clear();
        m_startMs = m_endMs = 0;
        m_refreshPending = false;
        m_refreshCursorMs = 0;
    }

private:
    QString m_symbol;
    qint64 m_tfMs = 0, m_startMs = 0, m_endMs = 0, m_retryAfterMs = 0;
    qint64 m_nextSendMs = 0;
    qint64 m_emptyLookbackMs;
    quint64 m_generation = 0;
    bool m_refreshPending = false;
    qint64 m_refreshCursorMs = 0, m_refreshStartMs = 0;
    std::optional<Request> m_pending;
    struct History {
        qint64 cursorMs = 0;
        int emptyPages = 0;
        qint64 retryAfterMs = 0;
        qint64 emptyScanStartMs = 0;
    };
    bool emptyLookbackExhausted(const History& history) const {
        return chart::candleHistoryFromMemory(m_tfMs) && history.emptyScanStartMs > 0 &&
               history.cursorMs <= std::max<qint64>(0, history.emptyScanStartMs - m_emptyLookbackMs) + m_tfMs;
    }
    std::map<std::pair<QString, qint64>, History> m_history;
};
