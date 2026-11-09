#include "FlowBackfillState.hpp"
#include <algorithm>

void FlowBackfillState::select(const QString& symbol, int64_t tf) {
    if (symbol == m_symbol && tf == m_tf) return;
    ++m_generation;
    m_symbol = symbol;
    m_tf = tf;
    m_start = m_end = m_retryAt = 0;
    m_backoff = 1000;
    m_pageLimit = 512;
    m_pagingStopped = false;
    m_window = false;
    m_liveFrom = m_probeBucket = -1;
    m_probeDue = m_sentAt = 0;
    m_refusedBucket.reset();
    m_covered.clear();
    m_refresh.clear();
    if (m_pending && m_pending->held) m_pending.reset();
}
void FlowBackfillState::setWindow(int64_t start, int64_t end) {
    m_window = m_tf >= 1000 && m_tf <= 86'400'000 && start >= 0 && end > start && end <= INT64_MAX - m_tf;
    if (!m_window) { m_start = m_end = 0; return; }
    // Keep work and storage bounded even at extreme zoom. Newest buckets first.
    m_start = std::max(start, end - int64_t(FlowSeriesBuffer::kMaxBuckets) * m_tf) / m_tf * m_tf;
    m_end = (end - 1) / m_tf * m_tf;
    // Coverage must not outlive buffer eviction when panning away and back.
    auto it = m_covered.begin();
    while (it != m_covered.end()) {
        const auto lo = std::max(m_start, it->first), hi = std::min(m_end + m_tf, it->second);
        if (lo >= hi) it = m_covered.erase(it);
        else if (lo != it->first) {
            it = m_covered.erase(it);
            m_covered[lo] = hi;
        } else { it->second = hi; ++it; }
    }
}
void FlowBackfillState::refresh(int64_t bucket, int64_t dueMs) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    auto [it, added] = m_refresh.emplace(bucket, dueMs);
    if (!added) it->second = std::min(it->second, dueMs);
    while (m_refresh.size() > FlowSeriesBuffer::kMaxBuckets) m_refresh.erase(m_refresh.begin());
}
void FlowBackfillState::observeLive(int64_t bucket, int64_t nowMs) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    if (m_liveFrom < 0) m_liveFrom = bucket;
    m_probeBucket = std::max(m_probeBucket, bucket);
    if (!m_probeDue) m_probeDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
}
std::optional<FlowBackfillState::Request> FlowBackfillState::next(int64_t nowMs) {
    if (m_symbol.isEmpty() || m_tf < 1000 || nowMs < m_retryAt) return {};
    if (m_pending) {
        if (!m_pending->held) return {};
        m_pending->held = false;
        m_pending->id = ++m_nextId;
        m_sentAt = nowMs;
        return m_pending;
    }
    Request request{0, m_generation, m_symbol, m_tf};
    for (auto it = m_refresh.begin(); it != m_refresh.end(); ++it) {
        if (it->second > nowMs) continue;
        request.endMs = it->first;
        request.limit = 1;
        request.refresh = true;
        it = m_refresh.erase(it);
        // Adjacent due buckets share one page rather than one job each.
        while (it != m_refresh.end() && it->second <= nowMs &&
               it->first == request.endMs + m_tf && request.limit < m_pageLimit) {
            request.endMs = it->first;
            ++request.limit;
            it = m_refresh.erase(it);
        }
        request.id = ++m_nextId;
        m_pending = request;
        m_sentAt = nowMs;
        return m_pending;
    }
    if (m_probeDue && nowMs >= m_probeDue && m_probeBucket >= 0) {
        request.endMs = m_probeBucket;
        request.limit = 1;
        request.refresh = request.probe = true;
        m_probeDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
        request.id = ++m_nextId;
        m_pending = request;
        m_sentAt = nowMs;
        return m_pending;
    }
    if (m_pagingStopped || !m_window || m_end < m_start) return {};
    int64_t end = m_liveFrom < 0 ? m_end : std::min(m_end, m_liveFrom - m_tf);
    // Skip proven runs above the next gap, including an edge that has moved.
    while (end >= m_start) {
        auto above = m_covered.upper_bound(end);
        if (above == m_covered.begin()) break;
        const auto covered = std::prev(above);
        if (covered->second <= end) break;
        end = covered->first - m_tf;
    }
    if (end < m_start) return {};
    int64_t gapStart = m_start;
    if (auto above = m_covered.upper_bound(end); above != m_covered.begin())
        gapStart = std::max(gapStart, std::prev(above)->second);
    request.endMs = end;
    request.limit = int(std::min<int64_t>(m_pageLimit, (end - gapStart) / m_tf + 1));
    request.id = ++m_nextId;
    m_pending = request;
    m_sentAt = nowMs;
    return m_pending;
}
void FlowBackfillState::cover(int64_t from, int64_t to) {
    auto it = m_covered.lower_bound(from);
    if (it != m_covered.begin()) {
        const auto prev = std::prev(it);
        if (prev->second >= from) it = prev;
    }
    while (it != m_covered.end() && it->first <= to) {
        from = std::min(from, it->first);
        to = std::max(to, it->second);
        it = m_covered.erase(it);
    }
    m_covered[from] = to;
}
bool FlowBackfillState::accept(const SentinelStreamClient::FlowBatch& b, int64_t nowMs) {
    if (!m_pending || m_pending->held || b.id != m_pending->id || b.symbol != m_pending->symbol ||
        b.timeframeMs != m_pending->timeframeMs) return false;
    const auto request = *m_pending;
    m_pending.reset();
    if (request.generation != m_generation) return false;
    m_retryAt = nowMs + 100;
    m_backoff = 1000;
    if (b.status == SentinelStreamClient::FlowStatus::Refused) { m_pagingStopped = true; return true; }
    if (b.oldestMs < 0 || b.oldestMs > request.endMs || b.oldestMs % m_tf != 0) {
        m_pagingStopped = true; // malformed/non-progressing page
        return false;
    }
    const auto lo = std::max(b.oldestMs, m_start), hi = std::min(request.endMs + m_tf, m_end + m_tf);
    if (lo < hi) cover(lo, hi);
    for (const auto& range : b.ranges) {
        if (range.status == SentinelStreamClient::FlowStatus::Building &&
            range.fromMs >= m_start && range.fromMs <= m_end)
            refresh(range.fromMs, nowMs + FlowSeriesBuffer::kRefetchIntervalMs);
    }
    // One bucket samples the seal once per interval; seal advancement triggers
    // targeted store requests for the now-final provisional buckets.
    int64_t newestProvisional = -1;
    for (const auto& record : b.bars)
        if (record.source == SentinelStreamClient::FlowSource::Live)
            newestProvisional = std::max(newestProvisional, record.bar.startMs);
    if (newestProvisional >= 0) {
        m_probeBucket = std::max(m_probeBucket, newestProvisional);
        if (!m_probeDue) m_probeDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
    } else if (request.probe && m_probeBucket == request.endMs) {
        m_probeBucket = -1;
        m_probeDue = 0;
    }
    return true;
}
bool FlowBackfillState::fail(quint64 id, SentinelStreamClient::FlowFailure failure, int64_t nowMs) {
    if (!m_pending || m_pending->held || id != m_pending->id) return false;
    if (m_pending->generation != m_generation) { m_pending.reset(); return true; }
    if (failure == SentinelStreamClient::FlowFailure::Oversized) {
        if (m_pending->limit == 1) {
            m_refusedBucket = m_pending->endMs;
            cover(*m_refusedBucket, *m_refusedBucket + m_tf);
            m_pending.reset();
        } else {
            m_pageLimit = std::min(m_pageLimit, std::max(1, m_pending->limit / 2));
            m_pending->limit = m_pageLimit;
            m_pending->held = true;
        }
        m_retryAt = nowMs + 100;
        return true;
    }
    if (failure == SentinelStreamClient::FlowFailure::Fatal) {
        m_pending.reset();
        m_pagingStopped = true;
        return true;
    }
    m_pending->held = true;
    m_retryAt = nowMs + m_backoff;
    m_backoff = std::min<int64_t>(30'000, m_backoff * 2);
    return true;
}
bool FlowBackfillState::timeout(int64_t nowMs) {
    if (!inFlight() || nowMs - m_sentAt < 5000) return false;
    return fail(m_pending->id, SentinelStreamClient::FlowFailure::Retryable, nowMs);
}
std::optional<int64_t> FlowBackfillState::takeRefusedBucket() {
    auto bucket = m_refusedBucket;
    m_refusedBucket.reset();
    return bucket;
}
void FlowBackfillState::disconnect() {
    m_pending.reset();
    select({}, 0);
}

int64_t FlowBackfillState::wakeAtMs(int64_t nowMs) const {
    if (inFlight()) return m_sentAt + 5000;
    if (nowMs < m_retryAt) return m_retryAt;
    int64_t at = 0;
    for (const auto& [bucket, due] : m_refresh) at = at == 0 ? due : std::min(at, due);
    if (m_probeDue) at = at == 0 ? m_probeDue : std::min(at, m_probeDue);
    return at;
}
