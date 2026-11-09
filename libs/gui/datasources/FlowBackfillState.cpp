#include "FlowBackfillState.hpp"
#include <algorithm>

void FlowBackfillState::select(const QString& symbol, int64_t tf) {
    if (symbol == m_symbol && tf == m_tf) return;
    ++m_generation;
    m_symbol = symbol;
    m_tf = tf;
    m_start = m_end = m_retryAt = 0;
    m_backoff = 1000;
    m_stopped = false;
    m_window = false;
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
    m_refresh.erase(m_refresh.begin(), m_refresh.lower_bound(m_start));
    m_refresh.erase(m_refresh.upper_bound(m_end), m_refresh.end());
}
void FlowBackfillState::refresh(int64_t bucket, int64_t dueMs) {
    if (m_stopped || m_tf < 1000 || bucket < 0) return;
    auto [it, added] = m_refresh.emplace(bucket, dueMs);
    if (!added) it->second = std::min(it->second, dueMs);
    while (m_refresh.size() > FlowSeriesBuffer::kMaxBuckets) m_refresh.erase(m_refresh.begin());
}
std::optional<FlowBackfillState::Request> FlowBackfillState::next(int64_t nowMs) {
    if (m_stopped || m_symbol.isEmpty() || m_tf < 1000 || nowMs < m_retryAt) return {};
    if (m_pending) {
        if (!m_pending->held) return {};
        m_pending->held = false;
        m_pending->id = ++m_nextId;
        return m_pending;
    }
    Request request{++m_nextId, m_generation, m_symbol, m_tf};
    for (auto it = m_refresh.begin(); it != m_refresh.end(); ++it) {
        if (it->second > nowMs) continue;
        request.endMs = it->first;
        request.limit = 1;
        request.refresh = true;
        it = m_refresh.erase(it);
        // Adjacent due buckets share one page rather than one job each.
        while (it != m_refresh.end() && it->second <= nowMs &&
               it->first == request.endMs + m_tf && request.limit < 512) {
            request.endMs = it->first;
            ++request.limit;
            it = m_refresh.erase(it);
        }
        m_pending = request;
        return m_pending;
    }
    if (!m_window || m_end < m_start) return {};
    int64_t end = m_end;
    for (auto it = m_covered.rbegin(); it != m_covered.rend(); ++it) {
        if (it->first <= end && it->second > end) end = it->first - m_tf;
    }
    if (end < m_start) return {};
    request.endMs = end;
    request.limit = int(std::min<int64_t>(512, (end - m_start) / m_tf + 1));
    m_pending = request;
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
    if (b.status == SentinelStreamClient::FlowStatus::Refused) { m_stopped = true; return true; }
    if (b.oldestMs < 0 || b.oldestMs > request.endMs || b.oldestMs % m_tf != 0) {
        m_stopped = true; // malformed/non-progressing page
        return false;
    }
    const auto lo = std::max(b.oldestMs, m_start), hi = std::min(request.endMs + m_tf, m_end + m_tf);
    if (lo < hi) cover(lo, hi);
    for (const auto& range : b.ranges) {
        if (range.status == SentinelStreamClient::FlowStatus::Building &&
            range.fromMs >= m_start && range.fromMs <= m_end)
            refresh(range.fromMs, nowMs + FlowSeriesBuffer::kRefetchIntervalMs);
    }
    // Provisional composites need a later store snapshot even when their
    // current range is complete and the viewport remains stationary.
    for (const auto& record : b.bars) {
        if (record.source == SentinelStreamClient::FlowSource::Live &&
            record.bar.startMs >= m_start && record.bar.startMs <= m_end)
            refresh(record.bar.startMs, nowMs + FlowSeriesBuffer::kRefetchIntervalMs);
    }
    return true;
}
bool FlowBackfillState::fail(quint64 id, bool retryable, int64_t nowMs) {
    if (!m_pending || m_pending->held || id != m_pending->id) return false;
    if (m_pending->generation != m_generation) { m_pending.reset(); return true; }
    if (!retryable) { m_pending.reset(); m_stopped = true; return true; }
    m_pending->held = true;
    m_retryAt = nowMs + m_backoff;
    m_backoff = std::min<int64_t>(30'000, m_backoff * 2);
    return true;
}
void FlowBackfillState::disconnect() {
    m_pending.reset();
    select({}, 0);
}

int64_t FlowBackfillState::wakeAtMs(int64_t nowMs) const {
    if (m_stopped || inFlight()) return 0;
    if (nowMs < m_retryAt) return m_retryAt;
    int64_t at = 0;
    for (const auto& [bucket, due] : m_refresh) at = at == 0 ? due : std::min(at, due);
    return at;
}
