#pragma once

#include <QString>
#include "../chart/core/CandleInterval.hpp"
#include "CandleCoverage.hpp"
#include <algorithm>
#include <map>
#include <optional>

// GUI-thread paging policy, independent of timers/transport/rendering. The wire
// echoes symbol, timeframe and range (no request id). Keep the outstanding request
// even across selection changes so an A -> B -> A reply cannot match a new A job.
//
// Pages follow the series coverage, newest first: (1) holes in the visible window
// plus prefetch, (2) the span from that window to the live edge, (3) older history
// past the window after empty pages. A refresh pass re-fetches the window first.
class CandleBackfillState {
public:
    static constexpr qint64 kThrottleMs = 100;
    static constexpr int kMaxEmptyPages = 3;
    static constexpr qint64 kFloorRetryMs = 60'000;
    static constexpr qint64 kDefaultEmptyLookbackMs = 7 * 24 * 60 * 60 * 1000LL;
    static constexpr qint64 kPageCap = 350;
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
        // Extends the oldest edge: the empty-page and lookback rules apply.
        bool bottom = false;
        // Reaches the live edge: the live stream covers the time after coverEndMs.
        bool liveEdge = false;
        qint64 coverEndMs = 0; // exclusive end of the time this page proves
    };
    // What the buffer holds for the selected series.
    struct Loaded {
        const CandleCoverage& coverage;
        qint64 oldestBarMs = 0; // 0: no bars
        bool cacheFull = false;
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
            m_liveFetchedMs = 0;
            m_fetched.clear();
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

    std::optional<Request> next(const Loaded& loaded, qint64 nowMs) {
        if (m_pending && m_pending->held && m_pending->generation != m_generation) m_pending.reset();
        if (m_pending && m_pending->held) {
            if (!needsHistory(loaded, nowMs) || retryDelayMs(loaded, nowMs) > 0) return std::nullopt;
            m_pending->held = false;
            m_pending->retryAtMs = 0;
            m_nextSendMs = nowMs + kThrottleMs;
            return m_pending;
        }
        if (m_pending || !needsHistory(loaded, nowMs) || retryDelayMs(loaded, nowMs) > 0) return std::nullopt;
        const std::optional<Request> request =
            m_refreshPending ? refreshPage(nowMs) : page(*plan(loaded, nowMs), nowMs);
        if (!request) return std::nullopt;
        m_pending = request;
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
        m_accepted = completed;
        // An empty memory page proves nothing (acceptedCoverage) but was answered: this
        // selection does not ask for it again. Every other page's proof is buffer coverage;
        // the part of a short memory page below its oldest bar is asked once more.
        if (chart::candleHistoryFromMemory(tfMs) && oldestReplyMs <= 0) m_fetched.add(startMs, completed.coverEndMs);
        if (completed.liveEdge) m_liveFetchedMs = std::max(m_liveFetchedMs, completed.coverEndMs);
        if (completed.refresh) {
            // Empty refresh windows still advance; they are not older-history floors.
            m_refreshCursorMs = chart::candleHistoryFromMemory(tfMs) && oldestReplyMs > 0
                                    ? std::min(boundary - tfMs, oldestReplyMs)
                                    : startMs + (chart::candleHistoryFromMemory(tfMs) ? m_tfMs : 0);
            m_refreshCursorMs = (m_refreshCursorMs / m_tfMs) * m_tfMs;
            if (m_refreshCursorMs <= m_refreshStartMs) m_refreshPending = false;
            return true;
        }
        if (!completed.bottom) return true;
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

    // The time the page accept() just took proves for quiet-bar synthesis, as
    // [startMs, endMs). A REST page proves its whole window (a bucket it lacks had no
    // trades). A memory page only returns the bars the server retained, which does not
    // prove the server saw the time before its oldest bar (restart, retention): it
    // proves from its oldest bar on, and an empty one proves nothing. What was asked
    // is remembered separately, so an unproven window is not asked again.
    bool acceptedCoverage(qint64 oldestReplyMs, qint64& startMs, qint64& endMs) const {
        if (!m_accepted) return false;
        const Request& page = *m_accepted;
        startMs = page.startMs;
        if (chart::candleHistoryFromMemory(page.timeframeMs)) {
            if (oldestReplyMs <= 0) return false;
            startMs = oldestReplyMs;
        }
        endMs = page.coverEndMs;
        return endMs > startMs;
    }

    // Where the live stream takes over after the page accept() just took, or 0 when
    // that page did not reach the live edge.
    qint64 acceptedLiveFromMs() const {
        return m_accepted && m_accepted->liveEdge ? m_accepted->coverEndMs : 0;
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

    qint64 retryDelayMs(const Loaded& loaded, qint64 nowMs) const {
        if (m_tfMs <= 0) return 0;
        qint64 deadline = std::max(m_retryAfterMs, m_nextSendMs);
        if (m_pending && m_pending->held && m_pending->generation == m_generation)
            deadline = std::max(deadline, m_pending->retryAtMs);
        // The empty-page pause holds only the oldest-edge scan, never a hole above it.
        if (!m_refreshPending) {
            const auto target = plan(loaded, nowMs);
            const auto it = m_history.find({m_symbol, m_tfMs});
            if (target && target->bottom && it != m_history.end())
                deadline = std::max(deadline, it->second.retryAfterMs);
        }
        return std::max<qint64>(0, deadline - nowMs);
    }

    bool needsHistory(const Loaded& loaded, qint64 nowMs) const {
        if (m_symbol.isEmpty() || m_tfMs <= 0 || m_endMs <= m_startMs) return false;
        if (m_refreshPending) return m_refreshCursorMs > 0 || std::min(m_endMs, liveEdgeMs(nowMs) + m_tfMs) > m_startMs;
        return plan(loaded, nowMs).has_value();
    }

    void disconnect() {
        ++m_generation;
        m_pending.reset();
        m_accepted.reset();
        m_retryAfterMs = 0;
        m_nextSendMs = 0;
        m_history.clear();
        m_startMs = m_endMs = 0;
        m_refreshPending = false;
        m_refreshCursorMs = 0;
        m_liveFetchedMs = 0;
        m_fetched.clear();
    }

private:
    // The next span to fetch, [loMs, hiMs). A bottom target pages down from hiMs
    // under the empty-page rules.
    struct Target {
        qint64 loMs = 0;
        qint64 hiMs = 0;
        bool bottom = false;
    };
    struct History {
        qint64 cursorMs = 0;
        int emptyPages = 0;
        qint64 retryAfterMs = 0;
        qint64 emptyScanStartMs = 0;
    };

    bool fromMemory() const { return chart::candleHistoryFromMemory(m_tfMs); }
    qint64 liveEdgeMs(qint64 nowMs) const { return (nowMs / m_tfMs) * m_tfMs; }
    qint64 floorBucket(qint64 ms) const { return (ms / m_tfMs) * m_tfMs; }
    qint64 ceilBucket(qint64 ms) const { return ((ms + m_tfMs - 1) / m_tfMs) * m_tfMs; }
    // A page that reaches the live edge also asks for the open bucket while the window holds it.
    qint64 wireBoundary(qint64 hiMs, qint64 nowMs) const {
        const qint64 liveEdge = liveEdgeMs(nowMs);
        return hiMs >= liveEdge ? std::max(hiMs, std::min(m_endMs, liveEdge + m_tfMs)) : hiMs;
    }

    std::optional<Target> plan(const Loaded& loaded, qint64 nowMs) const {
        if (m_symbol.isEmpty() || m_tfMs <= 0 || m_endMs <= m_startMs) return std::nullopt;
        const CandleCoverage& coverage = loaded.coverage;
        // A full ring evicts its oldest bars: a page older than them would be dropped at once.
        const qint64 floor = loaded.cacheFull ? std::max(m_startMs, loaded.oldestBarMs) : m_startMs;
        // The open bucket is the live stream's. Once a page reached the live edge in this
        // selection, the stream covers what follows it until its first bar arrives.
        qint64 liveTop = liveEdgeMs(nowMs);
        if (m_liveFetchedMs > 0 && coverage.liveFromMs() == 0) liveTop = std::min(liveTop, m_liveFetchedMs);
        const qint64 top = std::min(m_endMs, liveTop);
        qint64 lo = 0, hi = 0;
        if (newestUnknown(coverage, floor, top, lo, hi)) {
            if (lo > m_startMs || loaded.cacheFull) return Target{floorBucket(lo), ceilBucket(hi), false};
            if (auto bottom = bottomTarget(hi, nowMs)) return bottom;
        }
        if (newestUnknown(coverage, std::max(top, floor), liveTop, lo, hi))
            return Target{floorBucket(lo), ceilBucket(hi), false};
        // Past the window only while it holds no bar (an empty-page scan).
        if (loaded.cacheFull || (loaded.oldestBarMs > 0 && loaded.oldestBarMs <= m_startMs)) return std::nullopt;
        const auto it = m_history.find({m_symbol, m_tfMs});
        if (it == m_history.end() || it->second.emptyScanStartMs == 0) return std::nullopt;
        return bottomTarget(knownRunStart(coverage, m_startMs), nowMs);
    }

    // Known time: what the buffer proves plus this selection's unproven memory answers,
    // read from both sorted lists directly, in whole buckets (a bucket counts once any
    // of it is known). Newest unknown part [gapLo, gapHi) of [lo, hi).
    bool newestUnknown(const CandleCoverage& proven, qint64 lo, qint64 hi, qint64& gapLo, qint64& gapHi) const {
        if (proven.liveFromMs() > 0) hi = std::min(hi, floorBucket(proven.liveFromMs()));
        while (hi > lo) {
            const qint64 below = knownStartBelow(proven, hi);
            if (below >= hi) break;
            hi = below;
        }
        if (hi <= lo) return false;
        gapLo = std::max({lo, proven.lastEndAtOrBefore(hi - 1), m_fetched.lastEndAtOrBefore(hi - 1)});
        gapHi = hi;
        return true;
    }

    // Start of the known run that ends at ms (the bucket of the lowest span holding
    // ms - 1), or ms when ms - 1 is unknown.
    qint64 knownStartBelow(const CandleCoverage& proven, qint64 ms) const {
        qint64 next = ms;
        if (proven.liveFromMs() > 0 && ms - 1 >= proven.liveFromMs()) next = proven.liveFromMs();
        if (const auto* s = proven.spanContaining(ms - 1)) next = std::min(next, s->startMs);
        if (const auto* s = m_fetched.spanContaining(ms - 1)) next = std::min(next, s->startMs);
        return next < ms ? floorBucket(next) : ms;
    }

    qint64 knownRunStart(const CandleCoverage& proven, qint64 ms) const {
        qint64 start = ms;
        for (qint64 below = knownStartBelow(proven, start); below < start; below = knownStartBelow(proven, start))
            start = below;
        return start;
    }

    // Pages down from the oldest covered point; past the window only after empty pages.
    std::optional<Target> bottomTarget(qint64 hiMs, qint64 nowMs) const {
        const auto it = m_history.find({m_symbol, m_tfMs});
        const History history = it == m_history.end() ? History{} : it->second;
        if (emptyLookbackExhausted(history)) return std::nullopt;
        qint64 boundary = wireBoundary(hiMs, nowMs);
        if (history.emptyScanStartMs > 0 && history.cursorMs > 0) boundary = std::min(boundary, history.cursorMs);
        boundary = floorBucket(boundary);
        if (boundary <= m_startMs && history.emptyScanStartMs == 0) return std::nullopt;
        return Target{m_startMs, boundary, true};
    }

    std::optional<Request> page(const Target& target, qint64 nowMs) {
        return target.bottom ? bottomPage(target, nowMs) : spanPage(target, nowMs);
    }

    // Marks what a page proves; the open bucket is left to the live stream.
    void prove(Request& request, qint64 nowMs) const {
        const qint64 liveEdge = liveEdgeMs(nowMs);
        request.liveEdge = request.boundaryMs >= liveEdge;
        request.coverEndMs = std::min(request.boundaryMs, liveEdge);
    }

    // A hole in the window, or the span up to the live edge.
    std::optional<Request> spanPage(const Target& target, qint64 nowMs) {
        const qint64 boundary = wireBoundary(target.hiMs, nowMs);
        const qint64 bars = (boundary - target.loMs + (fromMemory() ? 0 : m_tfMs - 1000)) / m_tfMs;
        const int limit = static_cast<int>(std::min(kPageCap, bars));
        const qint64 end = boundary - (fromMemory() ? m_tfMs : 0);
        const qint64 start = std::max<qint64>(0, end - limit * m_tfMs);
        if (limit <= 0 || end <= 0 || (!fromMemory() && start <= 0)) return std::nullopt;
        Request request{m_symbol, m_tfMs, start, end, limit, m_generation, boundary};
        prove(request, nowMs);
        return request;
    }

    std::optional<Request> bottomPage(const Target& target, qint64 nowMs) {
        auto& history = m_history[{m_symbol, m_tfMs}];
        if (history.emptyPages >= kMaxEmptyPages) {
            // Resume the same scan after the pause, never revisit its empty prefix.
            history.emptyPages = 0;
            history.retryAfterMs = 0;
        }
        const qint64 boundary = target.hiMs;
        // After an empty window, probe full pages beyond it (bounded below).
        const qint64 bars = history.emptyScanStartMs > 0
                                ? kPageCap
                                : (boundary - m_startMs + (fromMemory() ? 0 : m_tfMs - 1000)) / m_tfMs;
        int limit = static_cast<int>(std::min(kPageCap, bars));
        // Memory history uses inclusive end and retained-bar count. Exclude the
        // oldest loaded bucket explicitly; REST rollup windows already exclude it.
        const qint64 end = boundary - (fromMemory() ? m_tfMs : 0);
        if (fromMemory()) {
            const qint64 scanStart = history.emptyScanStartMs > 0 ? history.emptyScanStartMs : boundary;
            const qint64 scanFloor = std::max<qint64>(0, scanStart - m_emptyLookbackMs);
            limit = static_cast<int>(std::min<qint64>(limit, (end - scanFloor) / m_tfMs));
            if (limit <= 0) return std::nullopt;
        }
        const qint64 start = std::max<qint64>(0, end - limit * m_tfMs);
        if (end <= 0 || (!fromMemory() && start <= 0)) return std::nullopt;
        Request request{m_symbol, m_tfMs, start, end, limit, m_generation, boundary};
        request.bottom = true;
        prove(request, nowMs);
        return request;
    }

    std::optional<Request> refreshPage(qint64 nowMs) {
        if (m_refreshCursorMs == 0) {
            m_refreshCursorMs = std::min(m_endMs, liveEdgeMs(nowMs) + m_tfMs);
            m_refreshStartMs = m_startMs;
        }
        const qint64 bars =
            (m_refreshCursorMs - m_refreshStartMs + (fromMemory() ? 0 : m_tfMs - 1000)) / m_tfMs;
        const int limit = static_cast<int>(std::min(kPageCap, bars));
        const qint64 end = m_refreshCursorMs - (fromMemory() ? m_tfMs : 0);
        const qint64 start = std::max<qint64>(0, end - limit * m_tfMs);
        if (limit <= 0 || end <= 0 || (!fromMemory() && start <= 0)) return std::nullopt;
        Request request{m_symbol, m_tfMs, start, end, limit, m_generation, m_refreshCursorMs, true};
        prove(request, nowMs);
        return request;
    }

    bool emptyLookbackExhausted(const History& history) const {
        return fromMemory() && history.emptyScanStartMs > 0 &&
               history.cursorMs <= std::max<qint64>(0, history.emptyScanStartMs - m_emptyLookbackMs) + m_tfMs;
    }

    QString m_symbol;
    qint64 m_tfMs = 0, m_startMs = 0, m_endMs = 0, m_retryAfterMs = 0;
    qint64 m_nextSendMs = 0;
    qint64 m_emptyLookbackMs;
    quint64 m_generation = 0;
    bool m_refreshPending = false;
    qint64 m_refreshCursorMs = 0, m_refreshStartMs = 0;
    qint64 m_liveFetchedMs = 0; // this selection's live-edge pages proved time up to here
    std::optional<Request> m_pending;
    std::optional<Request> m_accepted;
    CandleCoverage m_fetched; // empty memory windows this selection asked for: answered, unproven
    std::map<std::pair<QString, qint64>, History> m_history;
};
