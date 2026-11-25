#include "FlowSeriesBuffer.hpp"
#include <algorithm>
using Source = SentinelStreamClient::FlowSource;
using Status = SentinelStreamClient::FlowStatus;

void FlowSeriesBuffer::apply(const Batch& batch, int64_t, int64_t from, int64_t to) {
    if (batch.replayReady || batch.timeframeMs <= 0) return;
    if (!m_symbol.isEmpty() && batch.symbol != m_symbol) clear();
    m_symbol = batch.symbol;
    auto& s = m_series[batch.timeframeMs];
    for (const auto& in : batch.bars) {
        if (in.bar.startMs < from || in.bar.startMs >= to) continue;
        const auto held = s.bars.find(in.bar.startMs);
        if (held != s.bars.end()) {
            const auto& old = held->second;
            if (old.source == Source::Store && in.source == Source::Live) continue;
            if (old.source == in.source && (in.revision < old.revision ||
                (in.revision == old.revision && old.complete && !in.complete))) continue;
        }
        s.bars[in.bar.startMs] = in;
        s.coverage[in.bar.startMs] = {in.bar.startMs, in.bar.startMs + batch.timeframeMs,
                                    in.complete ? Status::Complete : Status::Live};
    }
    for (auto range : batch.ranges) {
        range.fromMs = std::max(range.fromMs, from); range.toMs = std::min(range.toMs, to);
        if (range.fromMs < range.toMs) s.coverage[range.fromMs] = range;
    }
    while (s.bars.size() > kMaxBuckets) s.bars.erase(s.bars.begin());
    while (s.coverage.size() > kMaxBuckets) s.coverage.erase(s.coverage.begin());
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
    while (coverage.size() > kMaxBuckets) coverage.erase(coverage.begin());
}
void FlowSeriesBuffer::retain(int64_t tf, int64_t from, int64_t to) {
    const auto it = m_series.find(tf); if (it == m_series.end()) return;
    std::erase_if(it->second.bars, [&](const auto& p) { return p.first < from || p.first >= to; });
    std::erase_if(it->second.coverage, [&](const auto& p) { return p.second.toMs <= from || p.second.fromMs >= to; });
    for (auto& [time, range] : it->second.coverage) {
        range.fromMs = std::max(range.fromMs, from); range.toMs = std::min(range.toMs, to);
    }
}
void FlowSeriesBuffer::clear() { m_symbol.clear(); m_series.clear(); }
size_t FlowSeriesBuffer::approxBarBytes(const Record& record) {
    return sizeof(std::pair<const int64_t, Record>) + kMapNodeOverheadBytes +
        record.bar.rows.capacity() * sizeof(sentinel::flow::Row) +
        record.bar.notable.capacity() * sizeof(sentinel::flow::Notable);
}
FlowSeriesBuffer::MemoryApprox FlowSeriesBuffer::approxMemory() const {
    MemoryApprox out;
    constexpr size_t node = kMapNodeOverheadBytes;
    for (const auto& [tf, s] : m_series) {
        ++out.series;
        out.bars += s.bars.size();
        out.coverageRanges += s.coverage.size();
        out.nodeBytes += s.bars.size() * (sizeof(decltype(s.bars)::value_type) + node) +
            s.coverage.size() * (sizeof(decltype(s.coverage)::value_type) + node);
        for (const auto& [start, record] : s.bars) {
            out.rows += record.bar.rows.size();
            out.rowBytes += record.bar.rows.capacity() * sizeof(sentinel::flow::Row);
            out.notableBytes += record.bar.notable.capacity() * sizeof(sentinel::flow::Notable);
        }
        if (!s.coverage.empty()) {
            const auto from = s.coverage.begin()->second.fromMs, to = s.coverage.rbegin()->second.toMs;
            out.coveredFromMs = out.coveredFromMs ? std::min(out.coveredFromMs, from) : from;
            out.coveredToMs = std::max(out.coveredToMs, to);
        }
    }
    return out;
}

void FlowSeriesBuffer::replace(const QString& symbol, int64_t tf, sentinel::flow::FlowViewSnapshot snapshot) {
    clear(); m_symbol = symbol;
    auto& s = m_series[tf];
    s.anchorMs = snapshot.anchorMs; s.prefixThroughMs = snapshot.prefixThroughMs;
    s.loadedFromMs = snapshot.loadedFromMs; s.prefixDelta = snapshot.prefixDelta;
    for (auto& [time, bucket] : snapshot.buckets) {
        using S = sentinel::flow::chunks::Status;
        const auto status = bucket.status == S::Complete ? Status::Complete :
            bucket.status == S::Partial ? Status::Partial : bucket.status == S::Unrecorded ? Status::Unrecorded : Status::Building;
        s.coverage[time] = {time, time + tf, status};
        if (bucket.value && bucket.value->bar.count) {
            auto& v = *bucket.value;
            s.bars[time] = {std::move(v.bar), v.revision, v.complete, v.sealed ? Source::Store : Source::Live};
        }
    }
}
