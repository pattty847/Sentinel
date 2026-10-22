#pragma once

#include <array>
#include <algorithm>
#include <cstdint>

// The time a candle series has been fetched for, as half-open spans [startMs, endMs).
// A history page covers its whole wire window; the live stream covers everything from its
// first bucket on while it flows. A bucket inside coverage without a bar had no trades; a
// bucket outside coverage was never fetched. Fixed capacity: no allocation, cheap to copy.
class CandleCoverage {
public:
    static constexpr int kMaxSpans = 16;
    struct Span {
        int64_t startMs = 0;
        int64_t endMs = 0;
    };

    void clear() {
        m_count = 0;
        m_liveFromMs = m_liveToMs = 0;
    }

    void add(int64_t startMs, int64_t endMs) {
        if (endMs <= startMs) return;
        int i = 0;
        while (i < m_count && m_spans[i].endMs < startMs) ++i;
        int j = i;
        while (j < m_count && m_spans[j].startMs <= endMs) {
            startMs = std::min(startMs, m_spans[j].startMs);
            endMs = std::max(endMs, m_spans[j].endMs);
            ++j;
        }
        if (i == j && m_count == kMaxSpans) {
            // Full: forget the oldest span. A forgotten span only draws as a hole and is fetched again.
            if (i == 0) return;
            std::move(m_spans.begin() + 1, m_spans.begin() + m_count, m_spans.begin());
            --m_count;
            --i;
            j = i;
        }
        const int removed = j - i;
        if (removed == 0) {
            std::move_backward(m_spans.begin() + i, m_spans.begin() + m_count, m_spans.begin() + m_count + 1);
            ++m_count;
        } else if (removed > 1) {
            std::move(m_spans.begin() + j, m_spans.begin() + m_count, m_spans.begin() + i + 1);
            m_count -= removed - 1;
        }
        m_spans[i] = {startMs, endMs};
    }

    // A live bar [startMs, endMs) arrived: coverage is open-ended from the first one.
    void extendLive(int64_t startMs, int64_t endMs) {
        if (startMs <= 0 || endMs <= startMs) return;
        if (m_liveFromMs == 0 || startMs < m_liveFromMs) m_liveFromMs = startMs;
        m_liveToMs = std::max(m_liveToMs, endMs);
    }

    // The live stream stopped (session, selection or disconnect): keep what it delivered.
    void closeLive() {
        if (m_liveFromMs > 0) add(m_liveFromMs, m_liveToMs);
        m_liveFromMs = m_liveToMs = 0;
    }

    // Bars older than ms were evicted: their coverage no longer has bars behind it.
    void trimBefore(int64_t ms) {
        int drop = 0;
        while (drop < m_count && m_spans[drop].endMs <= ms) ++drop;
        if (drop > 0) {
            std::move(m_spans.begin() + drop, m_spans.begin() + m_count, m_spans.begin());
            m_count -= drop;
        }
        if (m_count > 0) m_spans[0].startMs = std::max(m_spans[0].startMs, ms);
        if (m_liveFromMs > 0 && m_liveFromMs < ms) m_liveFromMs = ms;
    }

    bool covers(int64_t ms) const {
        if (m_liveFromMs > 0 && ms >= m_liveFromMs) return true;
        for (int i = 0; i < m_count; ++i) {
            if (ms < m_spans[i].startMs) return false;
            if (ms < m_spans[i].endMs) return true;
        }
        return false;
    }

    // True when every bucket start in [startMs, endMs) is covered.
    bool coversRange(int64_t startMs, int64_t endMs) const {
        int64_t lo = 0, hi = 0;
        return !newestGap(startMs, endMs, lo, hi);
    }

    // Smallest covered time >= ms, or INT64_MAX when nothing at or after ms is covered.
    int64_t nextCovered(int64_t ms) const {
        int64_t best = m_liveFromMs > 0 ? std::max(ms, m_liveFromMs) : INT64_MAX;
        for (int i = 0; i < m_count; ++i) {
            if (ms < m_spans[i].endMs) return std::min(best, std::max(ms, m_spans[i].startMs));
        }
        return best;
    }

    // Start of the covered run that contains ms, or ms when it is not covered.
    int64_t runStart(int64_t ms) const {
        int64_t start = ms;
        if (m_liveFromMs > 0 && ms >= m_liveFromMs) start = m_liveFromMs;
        for (int i = m_count - 1; i >= 0; --i) {
            if (m_spans[i].startMs <= start && start <= m_spans[i].endMs) start = m_spans[i].startMs;
        }
        return start;
    }

    // Newest uncovered part [gapLo, gapHi) of [lo, hi); false when [lo, hi) is covered.
    bool newestGap(int64_t lo, int64_t hi, int64_t& gapLo, int64_t& gapHi) const {
        if (m_liveFromMs > 0) hi = std::min(hi, m_liveFromMs);
        for (int i = m_count - 1; i >= 0 && hi > lo; --i) {
            const Span& s = m_spans[i];
            if (s.startMs >= hi) continue;
            if (s.endMs < hi) {
                gapLo = std::max(lo, s.endMs);
                gapHi = hi;
                return true;
            }
            hi = s.startMs; // [s.startMs, hi) is covered
        }
        if (hi <= lo) return false;
        gapLo = lo;
        gapHi = hi;
        return true;
    }

    bool empty() const { return m_count == 0 && m_liveFromMs == 0; }
    int64_t liveFromMs() const { return m_liveFromMs; }
    int spanCount() const { return m_count; }
    const Span& span(int i) const { return m_spans[static_cast<size_t>(i)]; }

private:
    std::array<Span, kMaxSpans> m_spans{};
    int m_count = 0;
    int64_t m_liveFromMs = 0, m_liveToMs = 0;
};
