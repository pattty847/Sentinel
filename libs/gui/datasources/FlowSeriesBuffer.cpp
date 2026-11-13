#include "FlowSeriesBuffer.hpp"
#include <algorithm>

using Source = SentinelStreamClient::FlowSource;
using Status = SentinelStreamClient::FlowStatus;

FlowSeriesBuffer::Merge FlowSeriesBuffer::merge(const Record* held, const Record& in, int64_t seal) {
    if (in.source == Source::Live && in.bar.startMs <= seal - in.bar.timeframeMs) return Merge::Drop;
    if (!held) return Merge::Replace;
    if (in.source != held->source) return in.source == Source::Store ? Merge::Replace : Merge::Drop;
    if (in.revision != held->revision) return in.revision > held->revision ? Merge::Replace : Merge::Drop;
    if (held->complete && !in.complete) return Merge::Drop;
    return Merge::Replace;
}

std::vector<int64_t> FlowSeriesBuffer::apply(const Batch& batch, int64_t nowMs) {
    if (!m_symbol.isEmpty() && batch.symbol != m_symbol) clear();
    m_symbol = batch.symbol;
    auto& s = m_series[batch.timeframeMs];
    std::vector<int64_t> refetch;
    if (batch.history && batch.status == Status::Refused) {
        for (const auto& range : batch.ranges) s.coverage[range.fromMs] = range;
        return refetch;
    }
    if (batch.history) {
        // This is a replacement, not max(old, new): store rebuilds can retreat.
        m_sealedThroughMs = batch.sealedThroughMs;
        for (auto& [tf, held] : m_series) {
            for (auto it = held.bars.begin(); it != held.bars.end();) {
                if (it->second.source == Source::Live && it->first <= m_sealedThroughMs - tf) {
                    const auto start = it->first;
                    const bool inPage = tf == batch.timeframeMs && std::any_of(batch.ranges.begin(), batch.ranges.end(),
                        [=](const Range& r) { return r.fromMs <= start && start < r.toMs; });
                    if (!inPage && tf == batch.timeframeMs) {
                        refetch.push_back(start);
                        held.coverage[start] = {start, start + tf, Status::Building};
                    }
                    it = held.bars.erase(it);
                } else ++it;
            }
        }
        for (const auto& range : batch.ranges) {
            s.coverage[range.fromMs] = range;
            if (range.status != Status::Unrecorded) continue;
            // A terminal empty answer withdraws cached values, including a prior
            // source generation. A later discovered recording can start at revision 1.
            s.bars.erase(s.bars.lower_bound(range.fromMs), s.bars.lower_bound(range.toMs));
            s.losses.erase(s.losses.lower_bound(range.fromMs), s.losses.lower_bound(range.toMs));
            s.lossRefetchAt.erase(s.lossRefetchAt.lower_bound(range.fromMs), s.lossRefetchAt.lower_bound(range.toMs));
        }
    }
    for (const auto& in : batch.bars) {
        const auto start = in.bar.startMs;
        auto held = s.bars.find(start);
        const bool loss = !batch.history && in.source == Source::Live && !in.complete &&
            held != s.bars.end() && held->second.source == Source::Live && held->second.complete &&
            held->second.revision >= in.revision && start > m_sealedThroughMs - batch.timeframeMs;
        if (loss) {
            s.losses[start] = held->second.revision;
            s.coverage[start] = {start, start + batch.timeframeMs, Status::Partial,
                                 in.bar.priceScale, 0};
            const auto last = s.lossRefetchAt.find(start);
            if (last == s.lossRefetchAt.end() || nowMs - last->second >= kRefetchIntervalMs) {
                s.lossRefetchAt[start] = nowMs;
                refetch.push_back(start);
            }
        }
        // An authoritative page resolves a reported loss even when the live
        // revision did not change. Older snapshots cannot resolve a newer loss.
        const auto lost = s.losses.find(start);
        if (batch.history && lost != s.losses.end() &&
            (in.source == Source::Store ||
             (in.revision >= lost->second && (held == s.bars.end() || in.revision >= held->second.revision)))) {
            s.bars.erase(start);
            s.losses.erase(lost);
            held = s.bars.end();
        }
        // A history snapshot may precede a stream close with the same live
        // revision. Only a completeness gain may replace at equal revision.
        if (batch.history && in.source == Source::Live && held != s.bars.end() &&
            held->second.source == Source::Live && in.revision == held->second.revision &&
            !(in.complete && !held->second.complete)) continue;
        if (merge(held == s.bars.end() ? nullptr : &held->second, in, m_sealedThroughMs) == Merge::Replace) {
            s.bars[start] = in;
            if (!batch.history && in.source == Source::Live) {
                const auto above = s.coverage.upper_bound(start);
                const bool alreadyCovered = above != s.coverage.begin() && std::prev(above)->second.toMs > start;
                if (!alreadyCovered) s.coverage[start] = {start, start + batch.timeframeMs, Status::Live};
            }
        }
    }
    // A re-fetch can prove an empty bucket. Retire the invalidated value only
    // when no newer live revision arrived while that page was in flight.
    if (batch.history) {
        for (const auto& range : batch.ranges) {
            auto loss = s.losses.lower_bound(range.fromMs);
            while (loss != s.losses.end() && loss->first < range.toMs) {
                const auto returned = std::lower_bound(batch.bars.begin(), batch.bars.end(), loss->first,
                    [](const Record& record, int64_t start) { return record.bar.startMs < start; });
                if (returned != batch.bars.end() && returned->bar.startMs == loss->first) { ++loss; continue; }
                auto held = s.bars.find(loss->first);
                if (held != s.bars.end() && held->second.source == Source::Live &&
                    held->second.revision <= loss->second) s.bars.erase(held);
                loss = s.losses.erase(loss);
            }
        }
    }
    if (const auto window = m_windows.find(batch.timeframeMs); window != m_windows.end())
        retainWindow(batch.timeframeMs, window->second.first, window->second.second);
    while (s.bars.size() > kMaxBuckets) s.bars.erase(s.bars.begin());
    while (s.coverage.size() > kMaxBuckets) s.coverage.erase(s.coverage.begin());
    while (s.losses.size() > kMaxBuckets) s.losses.erase(s.losses.begin());
    while (s.lossRefetchAt.size() > kMaxBuckets) s.lossRefetchAt.erase(s.lossRefetchAt.begin());
    return refetch;
}

const FlowSeriesBuffer::Series* FlowSeriesBuffer::series(int64_t tf) const {
    const auto it = m_series.find(tf);
    return it == m_series.end() ? nullptr : &it->second;
}
void FlowSeriesBuffer::setCoverage(const QString& symbol, int64_t tf, const Range& range) {
    if (!m_symbol.isEmpty() && symbol != m_symbol) clear();
    m_symbol = symbol;
    auto& coverage = m_series[tf].coverage;
    coverage[range.fromMs] = range;
    if (const auto window = m_windows.find(tf); window != m_windows.end())
        retainWindow(tf, window->second.first, window->second.second);
    while (coverage.size() > kMaxBuckets) coverage.erase(coverage.begin());
}
void FlowSeriesBuffer::clear() {
    m_symbol.clear();
    m_series.clear();
    m_windows.clear();
    m_sealedThroughMs = 0;
}

void FlowSeriesBuffer::retainWindow(int64_t tf, int64_t fromMs, int64_t toMs) {
    m_windows[tf] = {fromMs, toMs};
    const auto series = m_series.find(tf);
    if (series == m_series.end()) return;
    const auto trim = [=](auto& map) {
        map.erase(map.begin(), map.lower_bound(fromMs));
        auto beyond = map.lower_bound(toMs);
        // Keep the three newest live buckets that arrived ahead of the view.
        // They are ready when the viewport catches up without retaining an
        // unbounded future tail.
        auto keep = map.end();
        for (int i = 0; i < 3 && keep != beyond; ++i) --keep;
        map.erase(beyond, keep);
    };
    trim(series->second.bars);
    // Preserve proof of empty/history coverage across a retained reset anchor.
    auto& coverage = series->second.coverage;
    auto before = coverage.lower_bound(fromMs);
    if (before != coverage.begin()) {
        auto range = std::prev(before)->second;
        if (range.toMs > fromMs && !coverage.contains(fromMs)) {
            range.fromMs = fromMs;
            coverage.emplace(fromMs, range);
        }
    }
    trim(coverage);
    trim(series->second.losses);
    trim(series->second.lossRefetchAt);
}
