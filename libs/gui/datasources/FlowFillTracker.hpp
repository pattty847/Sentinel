#pragma once
#include <cstdint>
#include <optional>

// Measurement only (flow.timing probe): how long the visible flow range takes to
// be covered after the view moves. A wait starts at the first view change whose
// range is not covered and ends, once, when coverage reaches the visible range.
// Changes whose range is already covered start no wait. No clock or Qt: callers
// pass monotonic milliseconds and whether the range is covered.
class FlowFillTracker {
public:
    struct Filled {
        int64_t sinceLastChangeMs = 0, sinceFirstChangeMs = 0;
        int pages = 0, changes = 0;
    };
    void reset() { *this = {}; }
    // A view change; an unchanged range is ignored.
    std::optional<Filled> viewChanged(int64_t from, int64_t to, bool covered, int64_t nowMs) {
        if (m_hasView && from == m_from && to == m_to) return {};
        m_hasView = true;
        m_from = from; m_to = to;
        if (!m_waiting && covered) return {};
        if (!m_waiting) {
            m_waiting = true;
            m_firstChangeMs = nowMs;
            m_pages = m_changes = 0;
        }
        ++m_changes;
        m_lastChangeMs = nowMs;
        return settle(covered, nowMs);
    }
    // A history page was applied while the range may still be uncovered.
    std::optional<Filled> pageApplied(bool covered, int64_t nowMs) {
        if (!m_waiting) return {};
        ++m_pages;
        return settle(covered, nowMs);
    }
    bool waiting() const { return m_waiting; }
    int64_t from() const { return m_from; }
    int64_t to() const { return m_to; }
private:
    std::optional<Filled> settle(bool covered, int64_t nowMs) {
        if (!covered) return {};
        m_waiting = false;
        return Filled{nowMs - m_lastChangeMs, nowMs - m_firstChangeMs, m_pages, m_changes};
    }
    bool m_hasView = false, m_waiting = false;
    int64_t m_from = 0, m_to = 0, m_firstChangeMs = 0, m_lastChangeMs = 0;
    int m_pages = 0, m_changes = 0;
};
