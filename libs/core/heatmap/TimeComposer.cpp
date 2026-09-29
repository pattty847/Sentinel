#include "TimeComposer.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <map>
#include <tuple>
#include <stdexcept>

namespace heatmap {
namespace {
using Identity = std::tuple<int64_t, double, uint64_t, double, double>;
Identity identity(const NativeColumn& n) {
    return {n.grid.rowTickUnits, n.grid.priceScale, n.grid.configHash,
            n.sizeScale.floor, n.sizeScale.codesPerOctave};
}
struct NativeSum {
    NativeColumn result;
    std::array<std::map<int64_t, int64_t>, 2> edges;
    std::map<std::pair<int64_t, bool>, long double> sums;
    void add(const NativeColumn& n) {
        result.grid = n.grid;
        result.sizeScale = n.sizeScale;
        result.observedMs += n.observedMs;
        for (size_t side = 0; side < 2; ++side)
            for (const auto& run : n.coverage[side]) {
                edges[side][run.lo] += static_cast<int64_t>(run.coveredMs);
                edges[side][run.hi + 1] -= static_cast<int64_t>(run.coveredMs);
            }
        for (size_t i = 0; i < n.entries.size(); ++i) {
            const auto& entry = n.entries[i];
            sums[{n.baseRow + entry.row(), entry.isAsk()}] += entryNumerator(n, i);
        }
    }
    NativeColumn finish() {
        for (size_t side = 0; side < 2; ++side) {
            int64_t duration = 0;
            auto& runs = result.coverage[side];
            for (auto it = edges[side].begin(); it != edges[side].end(); ++it) {
                duration += it->second;
                const auto next = std::next(it);
                if (!duration || next == edges[side].end()) continue;
                const auto hi = next->first - 1;
                if (!runs.empty() && runs.back().hi + 1 == it->first && runs.back().coveredMs == uint64_t(duration))
                    runs.back().hi = hi;
                else runs.push_back({it->first, hi, uint64_t(duration)});
            }
        }
        result.baseRow = sums.empty() ? 0 : sums.begin()->first.first;
        result.entries.reserve(sums.size());
        result.numerators.reserve(sums.size());
        for (const auto& [key, numerator] : sums) {
            const auto [row, ask] = key;
            const auto duration = coveredMs(result.coverage[ask], row);
            if (!duration) continue;
            result.entries.push_back({packRowSide(row, result.baseRow, ask),
                recording::encodeSize(static_cast<double>(numerator / duration), result.sizeScale), duration});
            result.numerators.push_back(numerator);
        }
        return std::move(result);
    }
};
struct ColumnSum {
    SparseColumn result;
    std::map<Identity, NativeSum> native;
};
}
SparseColumns compose(std::span<const SparseColumns> levels, int64_t tfMs) {
    if (tfMs < kMinuteMs || tfMs > kDayMs || tfMs % kMinuteMs || levels.empty())
        throw std::invalid_argument("invalid heatmap timeframe or empty levels");
    SparseColumns out;
    out.symbol = levels.front().symbol;
    out.layer = levels.front().layer;
    out.tfMs = tfMs;
    std::vector<const SparseColumns*> selected;
    for (const auto& level : levels) {
        validate(level);
        if (level.symbol != out.symbol || level.layer != out.layer)
            throw std::invalid_argument("mixed symbols or layers in heatmap composition");
        if (level.tfMs == kHourMs && level.layer != "deep")
            throw std::invalid_argument("hour rollups exist only for deep");
        if (tfMs % level.tfMs == 0) selected.push_back(&level);
    }
    if (selected.empty()) throw std::invalid_argument("timeframe requires a finer source level");
    std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
        return std::tie(a->tfMs, a->startMs) > std::tie(b->tfMs, b->startMs);
    });
    out.startMs = selected.front()->startMs;
    out.endMs = selected.front()->endMs;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& level = *selected[i];
        out.startMs = std::min(out.startMs, level.startMs);
        out.endMs = std::max(out.endMs, level.endMs);
        for (size_t j = 0; j < i; ++j) {
            const auto& coarse = *selected[j];
            if (std::max(coarse.startMs, level.startMs) >= std::min(coarse.endMs, level.endMs)) continue;
            if (coarse.tfMs == level.tfMs || coarse.tfMs % level.tfMs)
                throw std::invalid_argument("overlapping incompatible heatmap levels");
        }
    }
    out.startMs = recording::floorDiv(out.startMs, tfMs) * tfMs;
    if (out.endMs > std::numeric_limits<int64_t>::max() - tfMs)
        throw std::invalid_argument("heatmap time range overflow");
    out.endMs = recording::floorDiv(out.endMs + tfMs - 1, tfMs) * tfMs;
    std::map<int64_t, ColumnSum> buckets;
    for (size_t i = 0; i < selected.size(); ++i) {
        for (const auto& column : selected[i]->columns) {
            const bool superseded = std::any_of(selected.begin(), selected.begin() + i, [&](const auto* coarse) {
                return column.bucketStartMs >= coarse->startMs && column.bucketStartMs < coarse->endMs;
            });
            if (superseded) continue;
            const auto bucket = recording::floorDiv(column.bucketStartMs, tfMs) * tfMs;
            auto& sum = buckets[bucket];
            sum.result.bucketStartMs = bucket;
            sum.result.observedMs += column.observedMs;
            sum.result.flags |= column.flags;
            for (const auto& native : column.native) sum.native[identity(native)].add(native);
        }
    }
    out.columns.reserve(buckets.size());
    for (auto& [bucket, sum] : buckets) {
        sum.result.flags &= ~recording::kPartial;
        if (sum.result.observedMs < uint64_t(tfMs)) sum.result.flags |= recording::kPartial;
        for (auto& [key, native] : sum.native) sum.result.native.push_back(native.finish());
        out.columns.push_back(std::move(sum.result));
    }
    validate(out);
    return out;
}
} // namespace heatmap
