#include "TimeComposer.hpp"
#include "NativeRows.hpp"
#include "../servermodel/RecordingCodec.hpp"
#include <tuple>
#include <stdexcept>

namespace heatmap {
namespace {
using Identity = std::tuple<int64_t, double, uint64_t, double, double>;
Identity identity(const NativeColumn& n) {
    return {n.grid.rowTickUnits, n.grid.priceScale, n.grid.configHash,
            n.sizeScale.floor, n.sizeScale.codesPerOctave};
}
NativeColumn combine(std::span<const NativeColumn* const> sources, detail::DecodeTables& tables) {
    auto rows = detail::aggregateRows(sources, tables);
    NativeColumn out;
    out.grid = sources.front()->grid;
    out.sizeScale = sources.front()->sizeScale;
    out.composed = true;
    out.observedMs = rows.observedMs;
    out.coverage = std::move(rows.coverage);
    out.baseRow = rows.rows.empty() ? 0 : rows.rows.front().row;
    out.entries.reserve(rows.rows.size() * 2);
    out.numerators.reserve(rows.rows.size() * 2);
    std::array<size_t, 2> runIndex{};
    for (const auto& row : rows.rows) for (size_t side = 0; side < 2; ++side) {
        if (!row.numerator[side]) continue;
        const auto& runs = out.coverage[side];
        auto& i = runIndex[side];
        while (i < runs.size() && runs[i].hi < row.row) ++i;
        const auto duration = i < runs.size() && runs[i].lo <= row.row ? runs[i].coveredMs : 0;
        if (!duration) continue;
        out.entries.push_back({packRowSide(row.row, out.baseRow, side),
            recording::encodeSize(static_cast<double>(row.numerator[side] / duration), out.sizeScale)});
        if (duration != out.observedMs && out.entryCoveredMs.empty()) {
            out.entryCoveredMs.reserve(rows.rows.size() * 2);
            out.entryCoveredMs.resize(out.entries.size() - 1, out.observedMs);
        }
        if (duration != out.observedMs || !out.entryCoveredMs.empty()) out.entryCoveredMs.push_back(duration);
        out.numerators.push_back(row.numerator[side]);
    }
    return out;
}
}
SparseColumns compose(std::span<const SparseColumns> levels, int64_t tfMs) {
    std::vector<const SparseColumns*> pointers;
    pointers.reserve(levels.size());
    for (const auto& level : levels) pointers.push_back(&level);
    return compose(std::span<const SparseColumns* const>(pointers), tfMs);
}
SparseColumns compose(std::span<const SparseColumns* const> levels, int64_t tfMs, const ComposeOptions& options) {
    if (tfMs < kMinuteMs || tfMs > kDayMs || tfMs % kMinuteMs || levels.empty())
        throw std::invalid_argument("invalid heatmap timeframe or empty levels");
    if (options.startMs.has_value() != options.endMs.has_value() ||
        (options.startMs && (*options.startMs % tfMs || *options.endMs % tfMs || *options.endMs <= *options.startMs)))
        throw std::invalid_argument("invalid heatmap compose clip");
    SparseColumns out;
    out.symbol = levels.front()->symbol;
    out.layer = levels.front()->layer;
    out.tfMs = tfMs;
    std::vector<const SparseColumns*> selected;
    for (const auto* levelPointer : levels) {
        const auto& level = *levelPointer;
        if (!options.trustedInputs) validate(level);
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
    std::vector<SparseColumns::TimeRange> scanned;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& level = *selected[i];
        out.startMs = std::min(out.startMs, level.startMs);
        out.endMs = std::max(out.endMs, level.endMs);
        scanned.insert(scanned.end(), level.scannedRanges.begin(), level.scannedRanges.end());
        for (size_t j = 0; j < i; ++j) {
            const auto& coarse = *selected[j];
            if (coarse.tfMs != level.tfMs && coarse.tfMs % level.tfMs == 0) continue;
            for (const auto& a : coarse.scannedRanges) for (const auto& b : level.scannedRanges)
                if (std::max(a.startMs, b.startMs) < std::min(a.endMs, b.endMs))
                    throw std::invalid_argument("overlapping incompatible heatmap levels");
        }
    }
    out.startMs = recording::floorDiv(out.startMs, tfMs) * tfMs;
    if (out.endMs > std::numeric_limits<int64_t>::max() - tfMs)
        throw std::invalid_argument("heatmap time range overflow");
    out.endMs = recording::floorDiv(out.endMs + tfMs - 1, tfMs) * tfMs;
    std::sort(scanned.begin(), scanned.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    std::vector<SparseColumns::TimeRange> united;
    for (const auto& range : scanned) {
        if (!united.empty() && range.startMs <= united.back().endMs)
            united.back().endMs = std::max(united.back().endMs, range.endMs);
        else united.push_back(range);
    }
    // Only full output buckets are proven scanned. Discarding partial output
    // aggregates keeps recomposition safe; S5 retains the original input chunks.
    if (options.startMs) {
        out.startMs = *options.startMs;
        out.endMs = *options.endMs;
    }
    for (const auto& range : united) {
        auto start = recording::floorDiv(range.startMs + tfMs - 1, tfMs) * tfMs;
        auto end = recording::floorDiv(range.endMs, tfMs) * tfMs;
        if (options.startMs) {
            start = std::max(start, *options.startMs);
            end = std::min(end, *options.endMs);
        }
        if (start < end) out.scannedRanges.push_back({start, end});
    }
    std::vector<const SparseColumn*> sourceColumns;
    for (size_t i = 0; i < selected.size(); ++i) {
        for (const auto& column : selected[i]->columns) {
            const auto bucket = recording::floorDiv(column.bucketStartMs, tfMs) * tfMs;
            if (!isScanned(out, bucket, bucket + tfMs)) continue;
            const bool superseded = std::any_of(selected.begin(), selected.begin() + i, [&](const auto* coarse) {
                return isScanned(*coarse, column.bucketStartMs, column.bucketStartMs + selected[i]->tfMs);
            });
            if (!superseded) sourceColumns.push_back(&column);
        }
    }
    std::sort(sourceColumns.begin(), sourceColumns.end(), [](const auto* a, const auto* b) {
        return a->bucketStartMs < b->bucketStartMs;
    });
    detail::DecodeTables tables;
    for (size_t i = 0; i < sourceColumns.size();) {
        SparseColumn column;
        column.bucketStartMs = recording::floorDiv(sourceColumns[i]->bucketStartMs, tfMs) * tfMs;
        struct Group { Identity key; std::vector<const NativeColumn*> sources; };
        std::vector<Group> groups;
        do {
            const auto& source = *sourceColumns[i++];
            column.observedMs += source.observedMs;
            column.flags |= source.flags;
            for (const auto& native : source.native) {
                const auto key = identity(native);
                auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& group) { return group.key == key; });
                if (it == groups.end()) groups.push_back({key, {&native}});
                else it->sources.push_back(&native);
            }
        } while (i < sourceColumns.size() && sourceColumns[i]->bucketStartMs < column.bucketStartMs + tfMs);
        column.flags &= ~recording::kPartial;
        if (column.observedMs < uint64_t(tfMs)) column.flags |= recording::kPartial;
        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        for (const auto& group : groups) column.native.push_back(combine(group.sources, tables));
        out.columns.push_back(std::move(column));
    }
    if (!options.trustedInputs) validate(out);
    return out;
}
} // namespace heatmap
