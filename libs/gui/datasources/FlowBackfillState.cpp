#include "FlowBackfillState.hpp"
#include <algorithm>
#include <limits>

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
    m_liveCurrentBucket = m_lastLiveBucket = m_probeBucket = -1;
    m_probeDue = m_sentAt = m_liveSampleDue = 0;
    m_sampledLive = -1;
    m_liveArmed = m_liveTracking = false;
    m_timedOut.reset();
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
    if (m_liveCurrentBucket < m_start || m_liveCurrentBucket > m_end)
        m_liveTracking = false;
}
void FlowBackfillState::uncover(int64_t below, int64_t from) {
    for (auto it = m_covered.begin(); it != m_covered.end() && it->first < below;) {
        const auto to = it->second;
        it = m_covered.erase(it);
        if (to > below) { m_covered.emplace(below, to); break; }
    }
    auto it = m_covered.lower_bound(from);
    if (it != m_covered.begin() && std::prev(it)->second > from) std::prev(it)->second = from;
    m_covered.erase(it, m_covered.end());
}
void FlowBackfillState::refresh(int64_t bucket, int64_t dueMs) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    auto [it, added] = m_refresh.emplace(bucket, dueMs);
    if (!added) it->second = std::min(it->second, dueMs);
    while (m_refresh.size() > FlowSeriesBuffer::kMaxBuckets) m_refresh.erase(m_refresh.begin());
}
void FlowBackfillState::beginLive(int64_t bucket) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    m_liveArmed = true;
    m_liveCurrentBucket = std::max(m_liveCurrentBucket, bucket);
}
void FlowBackfillState::markLiveEdge(int64_t bucket) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    // The clock's edge is the live seam and sizes cold-replay timeouts from the start;
    // it is covered only once live delivery is armed (beginLive or observeLive).
    m_liveCurrentBucket = std::max(m_liveCurrentBucket, bucket);
    if (!m_liveArmed) return;
    if (!m_window || bucket < m_start || bucket > m_end) { m_liveTracking = false; return; }
    if (m_liveTracking && m_lastLiveBucket >= 0 && bucket >= m_lastLiveBucket)
        cover(std::max(m_start, m_lastLiveBucket), bucket + m_tf);
    else cover(bucket, bucket + m_tf);
    m_lastLiveBucket = bucket;
    m_liveTracking = true;
}
void FlowBackfillState::observeLive(int64_t bucket, int64_t nowMs) {
    if (m_tf < 1000 || bucket < 0 || m_symbol.isEmpty()) return;
    if (!m_liveArmed) beginLive(bucket);
    m_liveCurrentBucket = std::max(m_liveCurrentBucket, bucket);
    if (m_window && bucket >= m_start) cover(bucket, bucket + m_tf);
    if (m_window && bucket >= m_start && bucket <= m_end && !m_liveTracking) {
        m_lastLiveBucket = bucket;
        m_liveTracking = true;
    }
    m_probeBucket = std::max(m_probeBucket, bucket);
    if (!m_probeDue) m_probeDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
}
std::optional<FlowBackfillState::Request> FlowBackfillState::next(int64_t nowMs) {
    m_hold = Hold::Sent;
    if (m_symbol.isEmpty() || m_tf < 1000) { m_hold = Hold::NoSelection; return {}; }
    if (nowMs < m_retryAt) { m_hold = m_retryHold; return {}; }
    if (m_pending) {
        if (!m_pending->held) { m_hold = Hold::InFlight; return {}; }
        if (fitHeldToSeam(nowMs)) {
            m_pending->held = false;
            m_pending->id = ++m_nextId;
            m_sentAt = nowMs;
            return m_pending;
        }
    }
    Request request{0, m_generation, m_symbol, m_tf};
    const int64_t limit = historyLimit();
    for (auto it = m_refresh.begin(); it != m_refresh.end() && it->first <= limit; ++it) {
        if (it->second > nowMs) continue;
        request.endMs = it->first;
        request.limit = 1;
        request.refresh = true;
        it = m_refresh.erase(it);
        // Adjacent due buckets share one page rather than one job each.
        while (it != m_refresh.end() && it->second <= nowMs && it->first == request.endMs + m_tf &&
               it->first <= limit && request.limit < m_pageLimit) {
            request.endMs = it->first;
            ++request.limit;
            it = m_refresh.erase(it);
        }
        return send(request, nowMs);
    }
    // The probe samples the seal watermark; any bucket history owns answers it.
    if (const int64_t probe = std::min(m_probeBucket, limit); m_probeDue && nowMs >= m_probeDue && probe >= 0) {
        request.endMs = probe;
        request.limit = 1;
        request.refresh = request.probe = true;
        m_probeDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
        return send(request, nowMs);
    }
    if (m_pagingStopped) { m_hold = Hold::Stopped; return {}; }
    if (!m_window || m_end < m_start) { m_hold = Hold::Idle; return {}; }
    if (const int64_t end = gapEnd(); end >= m_start) {
        int64_t gapStart = m_start;
        if (auto above = m_covered.upper_bound(end); above != m_covered.begin())
            gapStart = std::max(gapStart, std::prev(above)->second);
        request.endMs = end;
        request.limit = int(std::min<int64_t>(m_pageLimit, (end - gapStart) / m_tf + 1));
        return send(request, nowMs);
    }
    // Only after every older gap: the live sample never delays history the view needs.
    if (liveSampling()) {
        const int64_t from = sampleFrom();
        if (nowMs >= m_liveSampleDue) {
            request.endMs = m_liveCurrentBucket;
            request.limit = int((m_liveCurrentBucket - from) / m_tf + 1);
            request.sample = true;
            m_liveSampleDue = nowMs + FlowSeriesBuffer::kRefetchIntervalMs;
            return send(request, nowMs);
        }
        if (!covers(from, m_liveCurrentBucket + m_tf)) { m_hold = Hold::LiveSample; return {}; }
    }
    m_hold = Hold::Idle;
    return {};
}
std::optional<FlowBackfillState::Request> FlowBackfillState::send(Request request, int64_t nowMs) {
    request.id = ++m_nextId;
    m_pending = request;
    m_sentAt = nowMs;
    return m_pending;
}
// A held request goes out again only within the current seam. Buckets it named that are
// now live (or after) are dropped; a refresh keeps them queued for after the seam, and a
// live sample keeps its closed buckets as a plain page. False when nothing is left.
bool FlowBackfillState::fitHeldToSeam(int64_t nowMs) {
    auto& request = *m_pending;
    const int64_t allowed = request.sample && !m_liveArmed ? std::numeric_limits<int64_t>::max() : historyLimit();
    if (request.endMs <= allowed) return true;
    const int64_t past = std::min<int64_t>((request.endMs - allowed + m_tf - 1) / m_tf, request.limit);
    if (request.refresh && !request.probe)
        for (int64_t i = 0; i < past; ++i) refresh(request.endMs - i * m_tf, nowMs);
    request.endMs -= past * m_tf;
    request.limit -= int(past);
    request.sample = false;
    m_timedOut.reset();
    if (request.limit > 0) return true;
    m_pending.reset();
    return false;
}
int64_t FlowBackfillState::seamWakeWallMs() const {
    if (m_symbol.isEmpty() || m_liveCurrentBucket < 0 || liveSampling() || m_refresh.empty()) return 0;
    return m_refresh.rbegin()->first > historyLimit() ? m_liveCurrentBucket + m_tf : 0;
}
// Newest bucket a request other than the live sample may name: before the live bucket,
// and before the live sample's buckets while it runs. Unbounded before the clock is known.
int64_t FlowBackfillState::historyLimit() const {
    if (m_liveCurrentBucket < 0) return std::numeric_limits<int64_t>::max();
    return (liveSampling() ? sampleFrom() : m_liveCurrentBucket) - m_tf;
}
// Live not armed and the live bucket in the window: history samples the live edge.
bool FlowBackfillState::liveSampling() const {
    return !m_liveArmed && m_window && m_liveCurrentBucket >= m_start && m_liveCurrentBucket <= m_end;
}
// Oldest bucket the live sample owns: from the bucket the last sample ended at (closed
// since, or still live) through the live bucket, within one page and the window.
int64_t FlowBackfillState::sampleFrom() const {
    int64_t from = m_liveCurrentBucket;
    if (m_sampledLive >= 0)
        from = std::max(m_sampledLive, m_liveCurrentBucket - int64_t(m_pageLimit - 1) * m_tf);
    return std::max(from, m_start);
}
// The newest uncovered bucket of the window; below m_start when the window is covered.
int64_t FlowBackfillState::gapEnd() const {
    int64_t end = std::min(m_end, historyLimit());
    // Skip proven runs above the next gap, including an edge that has moved.
    while (end >= m_start) {
        auto above = m_covered.upper_bound(end);
        if (above == m_covered.begin()) break;
        const auto covered = std::prev(above);
        if (covered->second <= end) break;
        end = covered->first - m_tf;
    }
    return end;
}
bool FlowBackfillState::workDue(int64_t nowMs) const {
    if (m_symbol.isEmpty() || m_tf < 1000) return false;
    const int64_t limit = historyLimit();
    for (const auto& [bucket, due] : m_refresh) {
        if (bucket > limit) break;
        if (due <= nowMs) return true;
    }
    if (m_pagingStopped || !m_window || m_end < m_start) return false;
    return gapEnd() >= m_start || (liveSampling() && !covers(sampleFrom(), m_liveCurrentBucket + m_tf));
}
bool FlowBackfillState::covers(int64_t from, int64_t to) const {
    if (to <= from) return true;
    const auto above = m_covered.upper_bound(from);
    if (above == m_covered.begin()) return false;
    return std::prev(above)->second >= to;
}
const char* FlowBackfillState::holdName(Hold hold) {
    switch (hold) {
    case Hold::Sent: return "sent";
    case Hold::Idle: return "idle";
    case Hold::NoSelection: return "no_selection";
    case Hold::InFlight: return "in_flight";
    case Hold::Pacing: return "page_gap_100ms";
    case Hold::Backoff: return "retry_backoff";
    case Hold::Stopped: return "paging_stopped";
    case Hold::LiveSample: return "live_sample_interval";
    }
    return "unknown";
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
    if (!m_pending || b.symbol != m_pending->symbol || b.timeframeMs != m_pending->timeframeMs)
        return false;
    const bool late = m_timedOut && b.id == m_timedOut->id &&
        m_timedOut->generation == m_pending->generation &&
        m_timedOut->symbol == m_pending->symbol &&
        m_timedOut->timeframeMs == m_pending->timeframeMs &&
        m_timedOut->endMs == m_pending->endMs && m_timedOut->limit == m_pending->limit;
    if (!late && (m_pending->held || b.id != m_pending->id)) return false;
    const auto request = late ? *m_timedOut : *m_pending;
    m_pending.reset();
    m_timedOut.reset();
    if (request.generation != m_generation) return false;
    m_retryAt = nowMs + 100;
    m_retryHold = Hold::Pacing;
    m_backoff = 1000;
    if (request.sample) m_sampledLive = std::max(m_sampledLive, request.endMs);
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
    } else if (request.probe && std::min(m_probeBucket, historyLimit()) <= request.endMs) {
        m_probeBucket = -1;
        m_probeDue = 0;
    }
    return true;
}
bool FlowBackfillState::fail(quint64 id, SentinelStreamClient::FlowFailure failure, int64_t nowMs) {
    if (!m_pending || m_pending->held || id != m_pending->id) return false;
    if (m_pending->generation != m_generation) { m_pending.reset(); m_timedOut.reset(); return true; }
    if (failure == SentinelStreamClient::FlowFailure::Oversized) {
        m_timedOut.reset();
        if (m_pending->limit == 1) {
            m_refusedBucket = m_pending->endMs;
            cover(*m_refusedBucket, *m_refusedBucket + m_tf);
            m_pending.reset();
        } else {
            const int oldLimit = m_pending->limit;
            m_pageLimit = std::min(m_pageLimit, std::max(1, oldLimit / 2));
            if (m_pending->refresh && !m_pending->probe)
                for (int i = m_pageLimit; i < oldLimit; ++i)
                    refresh(m_pending->endMs - int64_t(i) * m_tf, nowMs + 100);
            m_pending->limit = m_pageLimit;
            m_pending->held = true;
        }
        m_retryAt = nowMs + 100;
        m_retryHold = Hold::Pacing;
        return true;
    }
    if (failure == SentinelStreamClient::FlowFailure::Fatal) {
        m_pending.reset();
        m_timedOut.reset();
        m_pagingStopped = true;
        return true;
    }
    m_pending->held = true;
    m_retryAt = nowMs + m_backoff;
    m_retryHold = Hold::Backoff;
    m_backoff = std::min<int64_t>(30'000, m_backoff * 2);
    return true;
}
bool FlowBackfillState::timeout(int64_t nowMs) {
    if (!inFlight() || nowMs - m_sentAt < flightTimeoutMs()) return false;
    m_timedOut = m_pending;
    return fail(m_pending->id, SentinelStreamClient::FlowFailure::Retryable, nowMs);
}
void FlowBackfillState::refuseSelection() {
    ++m_generation;
    m_symbol.clear();
    m_pending.reset();
    m_timedOut.reset();
    m_refresh.clear();
    m_probeDue = 0;
    m_retryAt = 0;
    m_liveArmed = m_liveTracking = false;
    m_pagingStopped = true;
}
int64_t FlowBackfillState::flightTimeoutMs() const {
    if (m_pending && m_pending->timeframeMs == 1000 && m_liveCurrentBucket >= 0 &&
        m_pending->endMs < m_liveCurrentBucket - 30 * 60 * 1000)
        return 30'000;
    return 5000;
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
    if (inFlight()) return m_sentAt + flightTimeoutMs();
    if (nowMs < m_retryAt) return m_retryAt;
    int64_t at = 0;
    const auto earliest = [&at](int64_t t) { at = at == 0 ? t : std::min(at, t); };
    const int64_t limit = historyLimit();
    for (const auto& [bucket, due] : m_refresh) {
        if (bucket > limit) break; // waits for the seam; the next live frame or view change sends it
        earliest(due);
    }
    if (m_probeDue) earliest(m_probeDue);
    if (!m_symbol.isEmpty() && !m_pagingStopped && liveSampling() && m_liveSampleDue > nowMs)
        earliest(m_liveSampleDue);
    return at;
}
