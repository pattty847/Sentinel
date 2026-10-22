#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

// The time a candle series is proven for, as sorted, merged, half-open spans
// [startMs, endMs). A REST page proves its whole wire window; a memory page proves
// from its oldest retained bar; the live stream proves everything from its first
// bucket on while it flows. A bucket inside coverage without a bar had no trades; a
// bucket outside it was never proven and draws nothing.
//
// No cap and no lossy eviction: only ring eviction removes coverage (trimBefore).
// Spans change on the GUI thread when a page, live bar or live run arrives, never per
// frame; every query is a read that does not allocate.
class CandleCoverage {
public:
    struct Span {
        int64_t startMs = 0;
        int64_t endMs = 0;
    };

    CandleCoverage() { m_spans.reserve(16); }

    void clear() {
        m_spans.clear();
        m_liveFromMs = m_liveToMs = 0;
    }

    void add(int64_t startMs, int64_t endMs) {
        if (endMs <= startMs) return;
        // First span that ends at or after startMs (touching spans merge).
        auto first = std::lower_bound(m_spans.begin(), m_spans.end(), startMs,
                                      [](const Span& s, int64_t ms) { return s.endMs < ms; });
        auto last = first;
        while (last != m_spans.end() && last->startMs <= endMs) {
            startMs = std::min(startMs, last->startMs);
            endMs = std::max(endMs, last->endMs);
            ++last;
        }
        if (first == last) {
            m_spans.insert(first, Span{startMs, endMs});
            return;
        }
        *first = {startMs, endMs};
        m_spans.erase(first + 1, last);
    }

    // A live bar [startMs, endMs) arrived: coverage is open-ended from the first one.
    void extendLive(int64_t startMs, int64_t endMs) {
        if (startMs <= 0 || endMs <= startMs) return;
        if (m_liveFromMs == 0 || startMs < m_liveFromMs) m_liveFromMs = startMs;
        m_liveToMs = std::max(m_liveToMs, endMs);
    }

    // History reached the live edge at ms before any live bar: the stream of this
    // selection covers the time after it. Closing keeps only what live bars delivered.
    void openLive(int64_t ms) {
        if (ms <= 0 || m_liveFromMs > 0) return;
        m_liveFromMs = m_liveToMs = ms;
    }

    // The live stream stopped (session, selection or disconnect): keep what it delivered.
    void closeLive() {
        if (m_liveFromMs > 0) add(m_liveFromMs, m_liveToMs);
        m_liveFromMs = m_liveToMs = 0;
    }

    // Bars older than ms were evicted: their coverage no longer has bars behind it.
    void trimBefore(int64_t ms) {
        auto keep = std::find_if(m_spans.begin(), m_spans.end(), [ms](const Span& s) { return s.endMs > ms; });
        m_spans.erase(m_spans.begin(), keep);
        if (!m_spans.empty()) m_spans.front().startMs = std::max(m_spans.front().startMs, ms);
        if (m_liveFromMs > 0 && m_liveFromMs < ms) m_liveFromMs = ms;
    }

    // The span that contains ms, or nullptr. The live run is not a span.
    const Span* spanContaining(int64_t ms) const {
        const auto it = firstEndingAfter(ms);
        return it != m_spans.end() && it->startMs <= ms ? &*it : nullptr;
    }

    // The largest span end at or before ms, or INT64_MIN when there is none.
    int64_t lastEndAtOrBefore(int64_t ms) const {
        const auto it = firstEndingAfter(ms);
        return it == m_spans.begin() ? INT64_MIN : std::prev(it)->endMs;
    }

    bool covers(int64_t ms) const {
        return (m_liveFromMs > 0 && ms >= m_liveFromMs) || spanContaining(ms) != nullptr;
    }

    // True when every time in [startMs, endMs) is covered.
    bool coversRange(int64_t startMs, int64_t endMs) const {
        int64_t lo = 0, hi = 0;
        return !newestGap(startMs, endMs, lo, hi);
    }

    // Smallest covered time >= ms, or INT64_MAX when nothing at or after ms is covered.
    int64_t nextCovered(int64_t ms) const {
        int64_t best = m_liveFromMs > 0 ? std::max(ms, m_liveFromMs) : INT64_MAX;
        const auto it = firstEndingAfter(ms);
        if (it != m_spans.end()) best = std::min(best, std::max(ms, it->startMs));
        return best;
    }

    // Start of the covered run that contains ms (a span touching the live run joins
    // it), or ms when it is not covered.
    int64_t runStart(int64_t ms) const {
        int64_t start = ms;
        if (m_liveFromMs > 0 && ms >= m_liveFromMs) start = m_liveFromMs;
        if (const Span* s = spanContaining(start)) return s->startMs;
        if (start > 0 && start == m_liveFromMs)
            if (const Span* s = spanContaining(start - 1)) return s->startMs;
        return start;
    }

    // Newest uncovered part [gapLo, gapHi) of [lo, hi); false when [lo, hi) is covered.
    bool newestGap(int64_t lo, int64_t hi, int64_t& gapLo, int64_t& gapHi) const {
        if (m_liveFromMs > 0) hi = std::min(hi, m_liveFromMs);
        if (hi <= lo) return false;
        if (const Span* s = spanContaining(hi - 1)) hi = s->startMs; // merged: nothing touches it
        if (hi <= lo) return false;
        gapLo = std::max(lo, lastEndAtOrBefore(hi - 1));
        gapHi = hi;
        return true;
    }

    bool empty() const { return m_spans.empty() && m_liveFromMs == 0; }
    int64_t liveFromMs() const { return m_liveFromMs; }
    int spanCount() const { return static_cast<int>(m_spans.size()); }
    const Span& span(int i) const { return m_spans[static_cast<size_t>(i)]; }

private:
    std::vector<Span>::const_iterator firstEndingAfter(int64_t ms) const {
        return std::upper_bound(m_spans.begin(), m_spans.end(), ms,
                                [](int64_t t, const Span& s) { return t < s.endMs; });
    }

    std::vector<Span> m_spans;
    int64_t m_liveFromMs = 0, m_liveToMs = 0;
};
