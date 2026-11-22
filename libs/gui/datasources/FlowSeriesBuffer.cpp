#include "FlowSeriesBuffer.hpp"
#include <algorithm>
#include <limits>

using Source = SentinelStreamClient::FlowSource;
using Status = SentinelStreamClient::FlowStatus;

namespace {
constexpr int64_t kNoBound = std::numeric_limits<int64_t>::max();
int64_t floorTo(int64_t v, int64_t tf) { return (v / tf - (v % tf < 0 ? 1 : 0)) * tf; }
int64_t ceilTo(int64_t v, int64_t tf) { return -floorTo(-v, tf); }
void cutBelow(std::map<int64_t, SentinelStreamClient::FlowRange>& coverage, int64_t below) {
    // Keep the proof of a range that crosses the cut.
    if (auto it = coverage.lower_bound(below); it != coverage.begin() && !coverage.contains(below)) {
        auto range = std::prev(it)->second;
        if (range.toMs > below) { range.fromMs = below; coverage.emplace(below, range); }
    }
    coverage.erase(coverage.begin(), coverage.lower_bound(below));
}
void cutFrom(std::map<int64_t, SentinelStreamClient::FlowRange>& coverage, int64_t from) {
    auto it = coverage.erase(coverage.lower_bound(from), coverage.end());
    if (it != coverage.begin() && std::prev(it)->second.toMs > from) std::prev(it)->second.toMs = from;
}
}

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
    enforceRetention(batch.timeframeMs, s);
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
    auto& series = m_series[tf];
    series.coverage[range.fromMs] = range;
    enforceRetention(tf, series);
}
void FlowSeriesBuffer::clear() {
    m_symbol.clear();
    m_series.clear();
    m_views.clear();
    m_trims.clear();
    m_sealedThroughMs = 0;
}

void FlowSeriesBuffer::setView(int64_t tf, int64_t viewFrom, int64_t viewTo, int64_t keepFrom, int64_t keepTo) {
    if (tf <= 0 || viewTo <= viewFrom) return;
    auto& view = m_views[tf];
    // The last pan direction: where the view's centre moved; a zoom about it keeps it.
    if (view.viewTo > view.viewFrom) {
        const int64_t moved = (viewFrom - view.viewFrom) + (viewTo - view.viewTo);
        if (moved) view.panDirection = moved > 0 ? 1 : -1;
    }
    const int64_t width = viewTo - viewFrom;
    const int64_t left = kPadScreens + (view.panDirection < 0 ? kPanPadScreens : 0);
    const int64_t right = kPadScreens + (view.panDirection > 0 ? kPanPadScreens : 0);
    view.viewFrom = viewFrom;
    view.viewTo = viewTo;
    view.keepFrom = keepFrom;
    view.keepTo = keepTo;
    view.padFrom = floorTo(viewFrom - left * width, tf);
    view.padTo = ceilTo(viewTo + right * width, tf);
    if (const auto series = m_series.find(tf); series != m_series.end()) enforceRetention(tf, series->second);
}

std::optional<FlowSeriesBuffer::Padding> FlowSeriesBuffer::padding(int64_t tf) const {
    const auto view = m_views.find(tf);
    if (view == m_views.end()) return {};
    return Padding{view->second.padFrom, view->second.padTo, view->second.panDirection};
}

std::optional<FlowSeriesBuffer::Trim> FlowSeriesBuffer::takeTrimmed(int64_t tf) {
    const auto it = m_trims.find(tf);
    if (it == m_trims.end()) return {};
    const auto trim = it->second;
    m_trims.erase(it);
    return trim;
}

size_t FlowSeriesBuffer::approxBarBytes(const Record& record) {
    return sizeof(std::pair<const int64_t, Record>) + kMapNodeOverheadBytes +
        record.bar.rows.capacity() * sizeof(sentinel::flow::Row) +
        record.bar.notable.capacity() * sizeof(sentinel::flow::Notable);
}

void FlowSeriesBuffer::enforceRetention(int64_t tf, Series& s) {
    const auto found = m_views.find(tf);
    const View* view = found == m_views.end() ? nullptr : &found->second;
    // Without a view every bucket counts as older than it, and only the counts apply: the
    // byte cap is measured outside a view.
    const int64_t keepFrom = view ? view->keepFrom : kNoBound, keepTo = view ? view->keepTo : kNoBound;
    size_t outsideBytes = 0;
    if (view)
        for (const auto& [start, record] : s.bars)
            if (start < keepFrom || start >= keepTo) outsideBytes += approxBarBytes(record);
    size_t bars = s.bars.size(), ranges = s.coverage.size();
    auto barLo = s.bars.begin(), barHi = s.bars.end();
    auto rangeLo = s.coverage.begin(), rangeHi = s.coverage.end();
    int64_t below = std::numeric_limits<int64_t>::min(), from = kNoBound; // keep [below, from)
    while (bars > kMaxBuckets || ranges > kMaxBuckets || outsideBytes > m_retainBytes) {
        // The outermost bucket on each side of the kept span.
        std::optional<int64_t> left, right;
        if (barLo != barHi && barLo->first < keepFrom) left = barLo->first;
        if (rangeLo != rangeHi && rangeLo->first < keepFrom) left = std::min(left.value_or(kNoBound), rangeLo->first);
        if (barLo != barHi && std::prev(barHi)->first >= keepTo) right = std::prev(barHi)->first;
        if (rangeLo != rangeHi && std::prev(rangeHi)->first >= keepTo)
            right = std::max(right.value_or(std::numeric_limits<int64_t>::min()), std::prev(rangeHi)->first);
        if (!left && !right) break;
        // Distance beyond the view in units of that side's padding; ties trim the older side.
        const bool takeLeft = left && (!right ||
            static_cast<long double>(view->viewFrom - *left) * std::max<int64_t>(1, view->padTo - view->viewTo) >=
            static_cast<long double>(*right + tf - view->viewTo) * std::max<int64_t>(1, view->viewFrom - view->padFrom));
        if (takeLeft) {
            if (barLo != barHi && barLo->first == *left) { outsideBytes -= std::min(outsideBytes, approxBarBytes(barLo->second)); ++barLo; --bars; }
            if (rangeLo != rangeHi && rangeLo->first == *left) { ++rangeLo; --ranges; }
            below = floorTo(*left, tf) + tf;
        } else {
            if (barLo != barHi && std::prev(barHi)->first == *right) { --barHi; outsideBytes -= std::min(outsideBytes, approxBarBytes(barHi->second)); --bars; }
            if (rangeLo != rangeHi && std::prev(rangeHi)->first == *right) { --rangeHi; --ranges; }
            from = *right;
        }
    }
    // The kept span holds at most kMaxBuckets buckets (the pager's window); a span past it
    // keeps the newest.
    if (bars > kMaxBuckets) below = std::max(below, std::prev(barHi, std::ptrdiff_t(kMaxBuckets))->first);
    if (below != std::numeric_limits<int64_t>::min()) {
        s.bars.erase(s.bars.begin(), s.bars.lower_bound(below));
        cutBelow(s.coverage, below);
        s.losses.erase(s.losses.begin(), s.losses.lower_bound(below));
        s.lossRefetchAt.erase(s.lossRefetchAt.begin(), s.lossRefetchAt.lower_bound(below));
    }
    if (from != kNoBound) {
        s.bars.erase(s.bars.lower_bound(from), s.bars.end());
        cutFrom(s.coverage, from);
        s.losses.erase(s.losses.lower_bound(from), s.losses.end());
        s.lossRefetchAt.erase(s.lossRefetchAt.lower_bound(from), s.lossRefetchAt.end());
    }
    while (s.coverage.size() > kMaxBuckets) {
        below = std::max(below, floorTo(s.coverage.begin()->first, tf) + tf);
        s.coverage.erase(s.coverage.begin());
    }
    if (below == std::numeric_limits<int64_t>::min() && from == kNoBound) return;
    auto& trim = m_trims[tf];
    trim.belowMs = std::max(trim.belowMs, below);
    trim.fromMs = std::min(trim.fromMs, from);
}

FlowSeriesBuffer::MemoryApprox FlowSeriesBuffer::approxMemory() const {
    MemoryApprox out;
    constexpr size_t node = kMapNodeOverheadBytes;
    for (const auto& [tf, s] : m_series) {
        ++out.series;
        out.bars += s.bars.size();
        out.coverageRanges += s.coverage.size();
        out.nodeBytes += s.bars.size() * (sizeof(decltype(s.bars)::value_type) + node) +
            s.coverage.size() * (sizeof(decltype(s.coverage)::value_type) + node) +
            s.lossRefetchAt.size() * (sizeof(decltype(s.lossRefetchAt)::value_type) + node) +
            s.losses.size() * (sizeof(decltype(s.losses)::value_type) + node);
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
