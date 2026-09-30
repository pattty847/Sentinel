#include "HeatmapSpanPlanner.hpp"
#include <map>
#include <numeric>
#include <tuple>

namespace heatmap {
using recording::floorDiv;

const char *spanTierName(SpanTier tier) {
    switch (tier) {
    case SpanTier::Visible: return "visible";
    case SpanTier::Fallback: return "fallback";
    case SpanTier::Prefetch: return "prefetch";
    case SpanTier::RecentTf: return "recent-tf";
    }
    return "?";
}
int SpanRank::fetchPriority() const {
    return (4 - int(tier)) * 1'000'000 - int(std::clamp<int64_t>(distance, 0, 999'999));
}

int64_t prefetchTiles(double timeLoMs, double timeHiMs, int64_t tfMs) {
    if (tfMs <= 0 || !(timeHiMs > timeLoMs)) return 2;
    const double viewTiles = (timeHiMs - timeLoMs) / (double(tfMs) * double(tiles::kTileColumns));
    return std::max<int64_t>(2, int64_t(std::ceil(std::min(viewTiles, 1e6))));
}

std::vector<PlannedSpan> planSpans(const std::string &symbol, int64_t tfMs, double lo, double hi,
                                   std::span<const SourceAvailability> sources, std::span<const PlannedSpan> retained) {
    std::map<SpanId, PlannedSpan> spans;
    const auto visible = tiles::tilesCovering(lo, hi, tfMs);
    if (visible.count() > 0 && tfMs % kMinuteMs == 0) {
        const int64_t margin = prefetchTiles(lo, hi, tfMs);
        for (int64_t tile = visible.first - margin; tile < visible.end + margin; ++tile) {
            PlannedSpan span{{symbol, tfMs, tile}, {}, {}};
            if (!visible.contains(tile))
                span.rank = {SpanTier::Prefetch, tile < visible.first ? visible.first - tile : tile - visible.end + 1};
            const int64_t start = span.id.startMs(), end = span.id.endMs();
            for (const auto &source : sources) {
                auto chunks = tiles::chunksFor(symbol, source.source, tfMs, start, end, source.time);
                if (chunks.empty()) continue;
                span.sources.push_back({source.source, std::clamp(source.time.oldestMs, start, end),
                                        std::clamp(source.time.endMs, start, end), std::move(chunks)});
            }
            if (!span.sources.empty()) spans.emplace(span.id, std::move(span));
        }
    }
    for (const auto &span : retained) {
        auto [it, inserted] = spans.emplace(span.id, span);
        if (!inserted && span.rank < it->second.rank) it->second.rank = span.rank;
    }
    std::vector<PlannedSpan> out;
    out.reserve(spans.size());
    for (auto &[id, span] : spans) out.push_back(std::move(span));
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.rank < b.rank; });
    return out;
}

// ------------------------------------------------------------------ resolution
namespace {
using Ranges = std::vector<PriceRange>;
Ranges unite(Ranges ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.lo < b.lo; });
    Ranges out;
    for (const auto &r : ranges) {
        if (r.end <= r.lo) continue;
        if (!out.empty() && r.lo <= out.back().end) out.back().end = std::max(out.back().end, r.end);
        else out.push_back(r);
    }
    return out;
}
Ranges intersect(const Ranges &a, const Ranges &b) {
    Ranges out;
    for (size_t i = 0, j = 0; i < a.size() && j < b.size();) {
        const auto lo = std::max(a[i].lo, b[j].lo), end = std::min(a[i].end, b[j].end);
        if (lo < end) out.push_back({lo, end});
        if (a[i].end < b[j].end) ++i;
        else ++j;
    }
    return out;
}
Ranges subtract(const Ranges &a, const Ranges &b) {
    Ranges out;
    size_t j = 0;
    for (auto r : a) {
        while (j < b.size() && b[j].end <= r.lo) ++j;
        for (size_t k = j; k < b.size() && b[k].lo < r.end; ++k) {
            if (b[k].lo > r.lo) out.push_back({r.lo, b[k].lo});
            r.lo = std::max(r.lo, b[k].end);
        }
        if (r.lo < r.end) out.push_back(r);
    }
    return out;
}
// Whole bins of `tick` inside each range.
Ranges inward(const Ranges &ranges, int64_t tick) {
    Ranges out;
    for (const auto &r : ranges) {
        const int64_t lo = -floorDiv(-r.lo, tick) * tick, end = floorDiv(r.end, tick) * tick;
        if (lo < end) out.push_back({lo, end});
    }
    return out;
}
// Native tick in target price units, 0 when not a positive whole number.
int64_t unitsIn(const GridIdentity &grid, double priceScale) {
    const double units = double(grid.rowTickUnits) * priceScale / grid.priceScale;
    if (!std::isfinite(units) || units < 0.5 || units >= 0x1p62) return 0;
    const double rounded = std::round(units);
    return std::abs(units - rounded) <= 1e-9 * rounded ? int64_t(rounded) : 0;
}
SourceResolution summarizeColumn(const SparseColumns &data, const std::string &source, int64_t ms, double scale) {
    SourceResolution out;
    out.source = source;
    out.state = bucketState(data, ms);
    if (out.state != BucketState::Present) return out;
    const auto it = std::lower_bound(data.columns.begin(), data.columns.end(), ms,
                                     [](const SparseColumn &c, int64_t t) { return c.bucketStartMs < t; });
    if (it == data.columns.end() || it->bucketStartMs != ms) return out;
    bool first = true;
    for (const auto &native : it->native) {
        const int64_t tick = unitsIn(native.grid, scale);
        // A grid this scale cannot express builds nothing here (binColumn veils it).
        if (!tick) return {source, out.state, {}, 0, {}};
        out.nativeTicks.push_back(tick);
        const int64_t g = out.commonUnits ? std::gcd(out.commonUnits, tick) : tick;
        const int64_t factor = out.commonUnits ? out.commonUnits / g : 1;
        if (factor > INT64_MAX / tick) return {source, out.state, {}, 0, {}};
        out.commonUnits = factor * tick;
        for (const auto &side : native.coverage) {
            Ranges full;
            for (const auto &run : side) {
                if (!run.coveredMs || run.coveredMs != native.observedMs || run.hi < run.lo) continue;
                if (run.lo < 0 || run.hi >= INT64_MAX / tick - 1) continue;
                full.push_back({run.lo * tick, (run.hi + 1) * tick});
            }
            full = unite(std::move(full));
            out.bands = first ? std::move(full) : intersect(out.bands, full);
            first = false;
        }
    }
    std::sort(out.nativeTicks.begin(), out.nativeTicks.end());
    out.nativeTicks.erase(std::unique(out.nativeTicks.begin(), out.nativeTicks.end()), out.nativeTicks.end());
    return out;
}
// Price units of the whole tick bins [lo, end) that [priceLo, priceHi) touches.
std::optional<PriceRange> binsInView(double priceLo, double priceHi, double scale, int64_t tick) {
    if (tick <= 0 || !std::isfinite(priceLo) || !std::isfinite(priceHi) || !(priceHi > priceLo)) return std::nullopt;
    const double lo = std::floor(std::max(priceLo, 0.0) * scale), hi = std::ceil(priceHi * scale);
    if (!(hi > lo) || hi >= 0x1p62) return std::nullopt;
    const int64_t first = floorDiv(int64_t(lo), tick), last = floorDiv(int64_t(hi) - 1, tick);
    return PriceRange{first * tick, (last + 1) * tick};
}
bool inView(const ColumnResolution &column, int64_t tfMs, double timeLo, double timeHi) {
    return double(column.startMs) < timeHi && double(column.startMs + tfMs) > timeLo;
}
} // namespace

ResolutionSummary summarizeResolution(const SparseColumns &composed, const std::string &source, int64_t tfMs,
                                      int64_t startMs, int64_t endMs, double priceScale) {
    ResolutionSummary out{tfMs, priceScale, {}};
    if (tfMs <= 0 || !(priceScale > 0) || composed.tfMs != tfMs) return out;
    for (int64_t ms = floorDiv(startMs, tfMs) * tfMs; ms < endMs; ms += tfMs)
        out.columns.push_back({ms, {summarizeColumn(composed, source, ms, priceScale)}});
    return out;
}

void mergeResolution(ResolutionSummary &into, const ResolutionSummary &from) {
    if (into.columns.empty() && !into.tfMs) {
        into.tfMs = from.tfMs;
        into.priceScale = from.priceScale;
    }
    if (from.tfMs != into.tfMs || from.priceScale != into.priceScale) return;
    auto it = into.columns.begin();
    for (const auto &column : from.columns) {
        it = std::lower_bound(it, into.columns.end(), column.startMs,
                              [](const ColumnResolution &c, int64_t ms) { return c.startMs < ms; });
        if (it == into.columns.end() || it->startMs != column.startMs) it = into.columns.insert(it, {column.startMs, {}});
        it->sources.insert(it->sources.end(), column.sources.begin(), column.sources.end());
        ++it;
    }
}

std::vector<PriceRange> unbuildableRows(const ColumnResolution &column, int64_t tick, PriceRange rows) {
    if (tick <= 0 || rows.end <= rows.lo) return {};
    Ranges data, covered;
    for (const auto &source : column.sources) {
        if (source.state != BucketState::Present) continue;
        data.insert(data.end(), source.bands.begin(), source.bands.end());
        if (buildsOn(tick, source.commonUnits)) {
            const auto whole = inward(source.bands, tick);
            covered.insert(covered.end(), whole.begin(), whole.end());
        }
    }
    const auto needed = intersect(inward(unite(std::move(data)), tick), {rows});
    return subtract(needed, unite(std::move(covered)));
}

std::vector<VeiledRange> veiledRanges(const ResolutionSummary &summary, int64_t tick, double timeLo, double timeHi,
                                      double priceLo, double priceHi) {
    std::vector<VeiledRange> out;
    const auto rows = binsInView(priceLo, priceHi, summary.priceScale, tick);
    if (!rows) return out;
    // Consecutive columns with identical veiled rows merge into one range.
    std::map<std::pair<int64_t, int64_t>, size_t> last; // rows -> index in out
    for (const auto &column : summary.columns) {
        if (!inView(column, summary.tfMs, timeLo, timeHi)) continue;
        for (const auto &range : unbuildableRows(column, tick, *rows)) {
            const auto it = last.find({range.lo, range.end});
            if (it != last.end() && out[it->second].endMs == column.startMs) {
                out[it->second].endMs += summary.tfMs;
                continue;
            }
            last[{range.lo, range.end}] = out.size();
            out.push_back({column.startMs, column.startMs + summary.tfMs, range});
        }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return std::tie(a.startMs, a.price.lo) < std::tie(b.startMs, b.price.lo);
    });
    return out;
}

bool buildsInView(const ResolutionSummary &summary, int64_t tick, double timeLo, double timeHi, double priceLo,
                  double priceHi) {
    const auto rows = binsInView(priceLo, priceHi, summary.priceScale, tick);
    if (!rows) return false;
    bool loaded = false;
    for (const auto &column : summary.columns) {
        if (!inView(column, summary.tfMs, timeLo, timeHi)) continue;
        for (const auto &source : column.sources)
            if (source.state == BucketState::Present && buildsOn(tick, source.commonUnits)) loaded = true;
        if (!unbuildableRows(column, tick, *rows).empty()) return false;
    }
    return loaded;
}

int64_t autoTickUnits(const ResolutionSummary &summary, int64_t currentUnits, double timeLo, double timeHi,
                      double priceLo, double priceHi, double heightPx, const AutoTickParams &params) {
    if (!std::isfinite(heightPx) || !(heightPx > 0) || !(priceHi > priceLo) || summary.columns.empty()) return 0;
    const double unitsPerPx = (priceHi - priceLo) * summary.priceScale / heightPx;
    return autoTickUnitsIf(currentUnits, unitsPerPx, [&](int64_t tick) {
        return buildsInView(summary, tick, timeLo, timeHi, priceLo, priceHi);
    }, params);
}

std::vector<int64_t> manualPresetUnits(const ResolutionSummary &summary, int64_t maxUnits) {
    std::vector<int64_t> commons;
    for (const auto &column : summary.columns)
        for (const auto &source : column.sources)
            if (source.state == BucketState::Present && source.commonUnits > 0 && !source.bands.empty())
                commons.push_back(source.commonUnits);
    std::sort(commons.begin(), commons.end());
    commons.erase(std::unique(commons.begin(), commons.end()), commons.end());
    return manualPresetUnits(commons, maxUnits);
}
} // namespace heatmap
