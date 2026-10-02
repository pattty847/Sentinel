#pragma once

#include "SparseColumns.hpp"
#include <queue>
#include <span>
#include <tuple>

namespace heatmap::detail {
// Shared worker scratch. Dense accumulation is bounded; sparse/widely separated
// price rows use a k-way merge instead of allocating their entire price span.
struct RowSum { int64_t row = 0; std::array<long double, 2> numerator{}; };
struct NativeRows {
    uint64_t observedMs = 0;
    std::array<std::vector<CoverageRun>, 2> coverage;
    std::vector<RowSum> rows;
};
class DecodeTables {
    struct Table { recording::SizeScale scale; std::vector<double> values; };
    std::vector<Table> tables_;
public:
    const std::vector<double>& get(const recording::SizeScale& scale) {
        for (const auto& table : tables_)
            if (table.scale.floor == scale.floor && table.scale.codesPerOctave == scale.codesPerOctave)
                return table.values;
        auto& table = tables_.emplace_back(Table{scale, {}});
        table.values.resize(recording::kMaxCode + 1);
        for (size_t code = 0; code < table.values.size(); ++code)
            table.values[code] = recording::decodeSize(static_cast<uint16_t>(code), scale);
        return table.values;
    }
};
inline void appendRun(std::vector<CoverageRun>& runs, int64_t lo, int64_t hi, uint64_t ms) {
    if (!ms || lo > hi) return;
    if (!runs.empty() && runs.back().hi + 1 == lo && runs.back().coveredMs == ms) runs.back().hi = hi;
    else runs.push_back({lo, hi, ms});
}
inline NativeRows aggregateRows(std::span<const NativeColumn* const> sources, DecodeTables& tables,
                                int64_t clipLo = INT64_MIN, int64_t clipEnd = INT64_MAX) {
    NativeRows out;
    int64_t lo = clipEnd, end = clipLo;
    for (const auto* n : sources) {
        out.observedMs += n->observedMs;
        for (const auto& side : n->coverage) for (const auto& run : side) {
            lo = std::min(lo, std::max(clipLo, run.lo));
            end = std::max(end, std::min(clipEnd, run.hi + 1));
        }
        if (!n->entries.empty()) {
            lo = std::min(lo, std::max(clipLo, n->baseRow + n->entries.front().row()));
            end = std::max(end, std::min(clipEnd, n->baseRow + n->entries.back().row() + 1));
        }
    }
    if (end <= lo) return out;
    const uint64_t span = uint64_t(end) - uint64_t(lo);
    constexpr uint64_t denseLimit = 262'144;
    if (span <= denseLimit) {
        struct Cell {
            std::array<long double, 2> numerator{};
            std::array<int64_t, 2> delta{};
        };
        std::vector<Cell> cells(static_cast<size_t>(span) + 1);
        for (const auto* n : sources) {
            for (size_t side = 0; side < 2; ++side) for (const auto& run : n->coverage[side]) {
                const auto low = std::max(lo, run.lo), high = std::min(end, run.hi + 1);
                if (low >= high) continue;
                cells[static_cast<size_t>(low - lo)].delta[side] += static_cast<int64_t>(run.coveredMs);
                cells[static_cast<size_t>(high - lo)].delta[side] -= static_cast<int64_t>(run.coveredMs);
            }
            const auto* decoded = n->composed ? nullptr : &tables.get(n->sizeScale);
            const auto first = std::lower_bound(n->entries.begin(), n->entries.end(), lo,
                [&](const auto& entry, int64_t row) { return n->baseRow + entry.row() < row; });
            for (size_t i = size_t(first - n->entries.begin()); i < n->entries.size(); ++i) {
                const auto& entry = n->entries[i];
                const auto row = n->baseRow + entry.row();
                if (row >= end) break;
                const auto value = n->composed ? entryNumerator(*n, i) :
                    static_cast<long double>((*decoded)[entry.code]) * entryCoveredMs(*n, i);
                cells[static_cast<size_t>(row - lo)].numerator[entry.isAsk()] += value;
            }
        }
        std::array<int64_t, 2> duration{};
        out.rows.reserve(static_cast<size_t>(span));
        for (size_t i = 0; i < span; ++i) {
            const auto row = lo + static_cast<int64_t>(i);
            for (size_t side = 0; side < 2; ++side) {
                duration[side] += cells[i].delta[side];
                appendRun(out.coverage[side], row, row, static_cast<uint64_t>(duration[side]));
            }
            if (cells[i].numerator[0] || cells[i].numerator[1]) out.rows.push_back({row, cells[i].numerator});
        }
        return out;
    }
    // Endpoint sort/merge includes covered zero rows without enumerating them.
    for (size_t side = 0; side < 2; ++side) {
        std::vector<std::pair<int64_t, int64_t>> edges;
        for (const auto* n : sources) for (const auto& run : n->coverage[side]) {
            const auto low = std::max(lo, run.lo), high = std::min(end, run.hi + 1);
            if (low >= high) continue;
            edges.emplace_back(low, static_cast<int64_t>(run.coveredMs));
            edges.emplace_back(high, -static_cast<int64_t>(run.coveredMs));
        }
        std::sort(edges.begin(), edges.end());
        int64_t duration = 0;
        for (size_t i = 0; i < edges.size();) {
            const auto row = edges[i].first;
            do { duration += edges[i++].second; } while (i < edges.size() && edges[i].first == row);
            if (i < edges.size()) appendRun(out.coverage[side], row, edges[i].first - 1, uint64_t(duration));
        }
    }
    struct Cursor { size_t source, index; };
    auto key = [&](const Cursor& cursor) {
        const auto* n = sources[cursor.source];
        const auto& e = n->entries[cursor.index];
        return std::tuple(n->baseRow + e.row(), e.isAsk(), cursor.source);
    };
    auto later = [&](const Cursor& a, const Cursor& b) { return key(a) > key(b); };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> queue(later);
    for (size_t source = 0; source < sources.size(); ++source) {
        const auto* n = sources[source];
        const auto first = std::lower_bound(n->entries.begin(), n->entries.end(), lo,
            [&](const auto& entry, int64_t row) { return n->baseRow + entry.row() < row; });
        if (first != n->entries.end()) queue.push({source, size_t(first - n->entries.begin())});
    }
    while (!queue.empty()) {
        auto cursor = queue.top(); queue.pop();
        const auto* n = sources[cursor.source];
        const auto& e = n->entries[cursor.index];
        const auto row = n->baseRow + e.row();
        if (row >= end) continue;
        if (row >= lo) {
            if (out.rows.empty() || out.rows.back().row != row) out.rows.push_back({row, {}});
            out.rows.back().numerator[e.isAsk()] += entryNumerator(*n, cursor.index);
        }
        if (++cursor.index < n->entries.size()) queue.push(cursor);
    }
    return out;
}
} // namespace heatmap::detail
