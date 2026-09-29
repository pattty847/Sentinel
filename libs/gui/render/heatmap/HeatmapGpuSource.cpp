#include "HeatmapGpuSource.hpp"
#include "heatmap/NativeRows.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace heatmap::gpu {
namespace {
std::atomic<uint64_t> nextSourceId{1};

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
int32_t checkedRow(int64_t row) {
    if (row < std::numeric_limits<int32_t>::min() || row > std::numeric_limits<int32_t>::max())
        throw std::invalid_argument("heatmap native row exceeds GPU int32 range");
    return static_cast<int32_t>(row);
}
// Native rows whose whole price interval lies in [priceLo, priceHi).
std::pair<int64_t, int64_t> clipRows(const GpuSourceOptions& options, double tick) {
    int64_t lo = INT64_MIN, end = INT64_MAX;
    auto position = [tick](double price) {
        const double value = price / tick, nearest = std::round(value);
        return std::abs(value - nearest) <= 1e-9 * std::max(1.0, std::abs(value)) ? nearest : value;
    };
    if (options.priceLo) lo = static_cast<int64_t>(std::ceil(position(*options.priceLo)));
    if (options.priceHi) end = static_cast<int64_t>(std::floor(position(*options.priceHi)));
    return {lo, end};
}
struct Group { double tick; std::vector<const NativeColumn*> sources; };
std::vector<Group> groupsOf(const SparseColumn& column) {
    std::vector<Group> groups;
    for (const auto& native : column.native) {
        const double tick = native.grid.rowTickUnits / native.grid.priceScale;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.tick == tick; });
        if (it == groups.end()) groups.push_back({tick, {&native}});
        else it->sources.push_back(&native);
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.tick < b.tick; });
    return groups;
}
} // namespace

FloatFloat splitDouble(double value) {
    const float hi = static_cast<float>(value);
    return {hi, static_cast<float>(value - static_cast<double>(hi))};
}
int32_t quantizeLow(float hi, float lo) {
    if (!(hi >= std::numeric_limits<float>::min()) || !std::isfinite(hi)) return 0;
    const double quantum = std::ldexp(1.0, std::ilogb(hi) - 23 - 14);
    const auto q = std::llround(static_cast<double>(lo) / quantum);
    return static_cast<int32_t>(std::clamp<long long>(q, -16384, 16383));
}
float dequantizeLow(float hi, int32_t q) {
    if (!(hi >= std::numeric_limits<float>::min()) || !std::isfinite(hi)) return 0;
    return static_cast<float>(q) * std::ldexp(1.0f, std::ilogb(hi) - 23 - 14);
}

std::vector<FloatFloat> encodeThresholds(const recording::SizeScale& scale) {
    std::vector<FloatFloat> out;
    out.reserve(recording::kMaxCode - 1);
    auto encode = [&](double v) { return recording::encodeSize(v, scale); };
    auto ordered = [](double v) { uint64_t bits; std::memcpy(&bits, &v, 8); return bits; };
    auto fromOrdered = [](uint64_t bits) { double v; std::memcpy(&v, &bits, 8); return v; };
    for (uint32_t k = 2; k <= recording::kMaxCode; ++k) {
        // The encoder rounds log2(v/floor)*cpo + 1, so code k starts near
        // floor * 2^((k - 1.5) / cpo). Bracket, then bisect on the (positive)
        // double bit pattern, which is ordered like the values.
        const double estimate = scale.floor * std::exp2((double(k) - 1.5) / scale.codesPerOctave);
        double below = estimate * (1 - 1e-9), above = estimate * (1 + 1e-9);
        while (encode(below) >= k) below *= 1 - 1e-6;
        while (encode(above) < k) above *= 1 + 1e-6;
        uint64_t lo = ordered(below), hi = ordered(above); // encode(lo) < k <= encode(hi)
        while (hi - lo > 1) {
            const uint64_t mid = lo + (hi - lo) / 2;
            if (encode(fromOrdered(mid)) >= k) hi = mid;
            else lo = mid;
        }
        out.push_back(splitDouble(fromOrdered(hi)));
    }
    return out;
}

std::array<uint32_t, kMaxTicks> tickFactors(const GpuSource& source, double displayTick) {
    std::array<uint32_t, kMaxTicks> factors{};
    for (size_t i = 0; i < source.ticks.size() && i < kMaxTicks; ++i) {
        const double factor = displayTick / source.ticks[i];
        if (!std::isfinite(factor) || factor < 1 || std::abs(factor - std::round(factor)) >= 1e-8 ||
            factor > double(std::numeric_limits<int32_t>::max())) continue;
        factors[i] = static_cast<uint32_t>(std::llround(factor));
    }
    return factors;
}

GpuSource buildGpuSource(const SparseColumns& data, const GpuSourceOptions& options) {
    if (data.tfMs <= 0) throw std::invalid_argument("invalid heatmap timeframe");
    GpuSource out;
    out.id = nextSourceId.fetch_add(1);
    out.symbol = data.symbol;
    out.layer = data.layer;
    out.tfMs = data.tfMs;
    const int64_t tf = data.tfMs;
    out.firstBucket = recording::floorDiv(data.startMs, tf);
    const int64_t endBucket = recording::floorDiv(data.endMs + tf - 1, tf);
    if (endBucket - out.firstBucket > (int64_t(1) << 26))
        throw std::invalid_argument("heatmap source spans too many buckets");
    const int64_t availableStart = options.availableStartMs.value_or(data.startMs);
    const int64_t availableEnd = options.availableEndMs.value_or(data.endMs);
    out.availableFirstBucket = recording::floorDiv(availableStart, tf);
    out.availableEndBucket = recording::floorDiv(availableEnd + tf - 1, tf);
    if (options.priceLo) out.clipPriceLo = *options.priceLo;
    if (options.priceHi) out.clipPriceHi = *options.priceHi;

    out.bucketSlots.assign(static_cast<size_t>(endBucket - out.firstBucket), kSlotNotLoaded);
    for (const auto& range : data.scannedRanges) {
        const int64_t a = recording::floorDiv(range.startMs, tf) - out.firstBucket;
        const int64_t b = recording::floorDiv(range.endMs, tf) - out.firstBucket;
        for (int64_t s = std::max<int64_t>(a, 0); s < std::min<int64_t>(b, int64_t(out.bucketSlots.size())); ++s)
            out.bucketSlots[size_t(s)] = kSlotGap;
    }

    // Wide entries only when some group's relative row span cannot fit 16 bits.
    for (const auto& column : data.columns) {
        for (const auto& group : groupsOf(column)) {
            const auto [clipLo, clipEnd] = clipRows(options, group.tick);
            int64_t lo = INT64_MAX, hi = INT64_MIN;
            for (const auto* n : group.sources) {
                if (n->entries.empty()) continue;
                lo = std::min(lo, std::max(clipLo, n->baseRow + int64_t(n->entries.front().row())));
                hi = std::max(hi, std::min(clipEnd - 1, n->baseRow + int64_t(n->entries.back().row())));
            }
            if (lo <= hi && hi - lo > 0xffff) out.wide = true;
        }
        if (out.wide) break;
    }

    detail::DecodeTables tables;
    out.columnGroups.reserve(data.columns.size() + 1);
    out.columnGroups.push_back(0);
    double coveredLo = std::numeric_limits<double>::infinity(), coveredHi = -coveredLo;
    for (size_t c = 0; c < data.columns.size(); ++c) {
        const auto& column = data.columns[c];
        const int64_t slot = recording::floorDiv(column.bucketStartMs, tf) - out.firstBucket;
        if (slot < 0 || slot >= int64_t(out.bucketSlots.size()))
            throw std::invalid_argument("heatmap column outside source extent");
        out.bucketSlots[size_t(slot)] = static_cast<uint32_t>(c);
        for (const auto& group : groupsOf(column)) {
            auto tickIt = std::find(out.ticks.begin(), out.ticks.end(), group.tick);
            if (tickIt == out.ticks.end()) {
                if (out.ticks.size() == kMaxTicks) throw std::invalid_argument("too many native ticks for GPU source");
                if (out.ticks.empty()) out.priceScale = group.sources.front()->grid.priceScale;
                out.ticks.push_back(group.tick);
                tickIt = out.ticks.end() - 1;
            }
            const auto [clipLo, clipEnd] = clipRows(options, group.tick);
            const auto aggregate = detail::aggregateRows(group.sources, tables, clipLo, clipEnd);
            GroupMeta meta;
            meta.tickIndex = static_cast<uint32_t>(tickIt - out.ticks.begin());
            for (size_t side = 0; side < 2; ++side) {
                meta.runBegin[side] = static_cast<uint32_t>(out.runs.size());
                for (const auto& run : aggregate.coverage[side]) {
                    coveredLo = std::min(coveredLo, double(run.lo) * group.tick);
                    coveredHi = std::max(coveredHi, double(run.hi + 1) * group.tick);
                    if (run.coveredMs != aggregate.observedMs) continue;
                    if (meta.runBegin[side] < out.runs.size() && out.runs.back()[1] + int64_t(1) == run.lo)
                        out.runs.back()[1] = checkedRow(run.hi);
                    else out.runs.push_back({checkedRow(run.lo), checkedRow(run.hi)});
                }
                meta.runEnd[side] = static_cast<uint32_t>(out.runs.size());
            }
            const long double weight = static_cast<long double>(aggregate.observedMs) / column.observedMs;
            meta.baseRow = checkedRow(aggregate.rows.empty() ? 0 : aggregate.rows.front().row);
            meta.entryBegin = static_cast<uint32_t>(out.entryCount);
            std::array<size_t, 2> runIndex{};
            for (const auto& row : aggregate.rows) {
                const uint64_t rel = static_cast<uint64_t>(row.row - meta.baseRow);
                if (rel > (out.wide ? 0x7fffffffull : 0xffffull))
                    throw std::invalid_argument("heatmap row span exceeds packed width");
                for (size_t side = 0; side < 2; ++side) {
                    if (!row.numerator[side]) continue;
                    const auto& runs = aggregate.coverage[side];
                    auto& i = runIndex[side];
                    while (i < runs.size() && runs[i].hi < row.row) ++i;
                    if (i == runs.size() || runs[i].lo > row.row) continue; // uncovered: binColumn skips it
                    const double value = static_cast<double>(row.numerator[side] / runs[i].coveredMs * weight);
                    if (!(value > 0) || !std::isfinite(value)) continue;
                    const auto ff = splitDouble(value);
                    const uint32_t key = uint32_t(side) | uint32_t(rel << 1);
                    out.entries.push_back(floatBits(ff.hi));
                    if (out.wide) {
                        out.entries.push_back(floatBits(ff.lo));
                        out.entries.push_back(key);
                    } else {
                        const auto q = quantizeLow(ff.hi, ff.lo);
                        out.entries.push_back(key | (uint32_t(q) << 17));
                    }
                    if (++out.entryCount > 0x7fffffffull) throw std::invalid_argument("too many heatmap entries");
                }
            }
            meta.entryEnd = static_cast<uint32_t>(out.entryCount);
            out.groups.push_back(meta);
        }
        out.columnGroups.push_back(static_cast<uint32_t>(out.groups.size()));
    }
    if (coveredLo < coveredHi) { out.coveredPriceLo = coveredLo; out.coveredPriceHi = coveredHi; }
    return out;
}
} // namespace heatmap::gpu
