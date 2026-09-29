#include "BinCell.hpp"
#include "NativeRows.hpp"
#include <map>
#include <stdexcept>

namespace heatmap {
namespace {
double gridPosition(double price, double tick) {
    const double value = price / tick, nearest = std::round(value);
    return std::abs(value - nearest) <= 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(value))
        ? nearest : value;
}
struct GridSum {
    uint64_t observedMs = 0;
    std::array<std::map<int64_t, int64_t>, 2> edges;
    std::map<int64_t, std::array<long double, 2>> numerators;
};
}
BinCell binCell(const SparseColumn& column, double priceLo, double displayTick,
                const recording::SizeScale& outputScale) {
    BinCell result;
    if (!column.observedMs || column.native.empty() || !std::isfinite(priceLo) || priceLo < 0 ||
        !std::isfinite(displayTick) || displayTick <= 0 || !std::isfinite(priceLo + displayTick)) return result;
    std::map<double, GridSum> grids;
    for (const auto& native : column.native) {
        const double tick = native.grid.rowTickUnits / native.grid.priceScale;
        const double group = displayTick / tick;
        const double position = gridPosition(priceLo, tick);
        if (!std::isfinite(tick) || tick <= 0 || !std::isfinite(group) || group < 1 ||
            std::abs(group - std::round(group)) >= 1e-8 || position != std::round(priceLo / tick) ||
            (priceLo + displayTick) / tick > 0x1p52) return result;
        const auto lo = static_cast<int64_t>(std::llround(position));
        const auto end = static_cast<int64_t>(std::llround((priceLo + displayTick) / tick));
        auto& sum = grids[tick];
        sum.observedMs += native.observedMs;
        for (size_t side = 0; side < 2; ++side)
            for (const auto& run : native.coverage[side]) {
                const auto low = std::max(lo, run.lo), high = std::min(end - 1, run.hi);
                if (low > high) continue;
                sum.edges[side][low] += static_cast<int64_t>(run.coveredMs);
                sum.edges[side][high + 1] -= static_cast<int64_t>(run.coveredMs);
            }
        for (size_t i = 0; i < native.entries.size(); ++i) {
            const auto& entry = native.entries[i];
            const auto row = native.baseRow + entry.row();
            if (row >= lo && row < end) sum.numerators[row][entry.isAsk()] += entryNumerator(native, i);
        }
    }
    std::array<long double, 2> values{};
    result.valid = true;
    for (auto& [tick, sum] : grids) {
        const auto lo = static_cast<int64_t>(std::llround(priceLo / tick));
        const auto end = static_cast<int64_t>(std::llround((priceLo + displayTick) / tick));
        std::array<long double, 2> nativeValues{};
        for (size_t side = 0; side < 2; ++side) {
            auto& edges = sum.edges[side];
            int64_t duration = 0;
            for (auto& [row, delta] : edges) { duration += delta; delta = duration; }
            auto covered = [&](int64_t row) -> int64_t {
                const auto it = edges.upper_bound(row);
                return it == edges.begin() ? 0 : std::prev(it)->second;
            };
            if (covered(lo) != static_cast<int64_t>(sum.observedMs)) result.valid = false;
            for (auto it = edges.upper_bound(lo); it != edges.end() && it->first < end; ++it)
                if (it->second != static_cast<int64_t>(sum.observedMs)) result.valid = false;
            for (const auto& [row, numerator] : sum.numerators) {
                const auto ms = covered(row);
                if (ms) nativeValues[side] += numerator[side] / ms;
            }
        }
        const auto weight = static_cast<long double>(sum.observedMs) / column.observedMs;
        for (size_t side = 0; side < 2; ++side) values[side] += nativeValues[side] * weight;
    }
    result.bid = static_cast<double>(values[0]);
    result.ask = static_cast<double>(values[1]);
    if (!std::isfinite(result.bid) || !std::isfinite(result.ask))
        throw std::runtime_error("nonfinite binned heatmap quantity");
    result.dominantAsk = values[1] > values[0];
    result.code = recording::withSide(recording::encodeSize(result.dominantAsk ? result.ask : result.bid, outputScale),
                                     result.dominantAsk);
    return result;
}
std::vector<BinCell> binColumn(const SparseColumn& column, double priceLo, double priceHi,
                              double displayTick, const recording::SizeScale& outputScale) {
    if (!std::isfinite(priceLo) || !std::isfinite(priceHi) || priceLo < 0 || priceHi <= priceLo ||
        !std::isfinite(displayTick) || displayTick <= 0)
        throw std::invalid_argument("invalid heatmap price range");
    const auto lo = std::floor(gridPosition(priceLo, displayTick));
    const auto end = std::ceil(gridPosition(priceHi, displayTick));
    if (end - lo < 1 || end - lo > 16384 || end > 0x1p52)
        throw std::invalid_argument("heatmap price range exceeds row limit");
    const auto count = static_cast<size_t>(end - lo);
    std::vector<BinCell> cells(count);
    if (!column.observedMs || column.native.empty()) return cells;
    const double bandLo = lo * displayTick, bandEnd = end * displayTick;
    struct Group { double tick; std::vector<const NativeColumn*> sources; };
    std::vector<Group> groups;
    for (const auto& native : column.native) {
        const auto tick = native.grid.rowTickUnits / native.grid.priceScale;
        const auto factor = displayTick / tick;
        if (!std::isfinite(tick) || tick <= 0 || !std::isfinite(factor) || factor < 1 ||
            std::abs(factor - std::round(factor)) >= 1e-8 ||
            gridPosition(bandLo, tick) != std::round(bandLo / tick) || bandEnd / tick > 0x1p52) return cells;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.tick == tick; });
        if (it == groups.end()) groups.push_back({tick, {&native}});
        else it->sources.push_back(&native);
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    for (auto& cell : cells) cell.valid = true;
    std::vector<std::array<long double, 2>> values(count), nativeValues(count);
    detail::DecodeTables tables;
    for (const auto& grid : groups) {
        const auto nativeLo = static_cast<int64_t>(std::llround(bandLo / grid.tick));
        const auto nativeEnd = static_cast<int64_t>(std::llround(bandEnd / grid.tick));
        const auto factor = static_cast<int64_t>(std::llround(displayTick / grid.tick));
        const auto aggregate = detail::aggregateRows(grid.sources, tables, nativeLo, nativeEnd);
        std::fill(nativeValues.begin(), nativeValues.end(), std::array<long double, 2>{});
        for (size_t side = 0; side < 2; ++side) {
            const auto& runs = aggregate.coverage[side];
            size_t runIndex = 0;
            for (size_t row = 0; row < count; ++row) {
                const auto cellLo = nativeLo + static_cast<int64_t>(row) * factor;
                const auto cellEnd = cellLo + factor;
                auto cursor = cellLo;
                while (cursor < cellEnd) {
                    while (runIndex < runs.size() && runs[runIndex].hi < cursor) ++runIndex;
                    if (runIndex == runs.size() || runs[runIndex].lo > cursor) {
                        cells[count - 1 - row].valid = false;
                        break;
                    }
                    if (runs[runIndex].coveredMs != aggregate.observedMs) cells[count - 1 - row].valid = false;
                    cursor = std::min(cellEnd, runs[runIndex].hi + 1);
                }
            }
            runIndex = 0;
            for (const auto& row : aggregate.rows) {
                while (runIndex < runs.size() && runs[runIndex].hi < row.row) ++runIndex;
                if (runIndex == runs.size() || runs[runIndex].lo > row.row) continue;
                const auto index = count - 1 - static_cast<size_t>((row.row - nativeLo) / factor);
                nativeValues[index][side] += row.numerator[side] / runs[runIndex].coveredMs;
            }
        }
        const auto weight = static_cast<long double>(aggregate.observedMs) / column.observedMs;
        for (size_t row = 0; row < count; ++row)
            for (size_t side = 0; side < 2; ++side) values[row][side] += nativeValues[row][side] * weight;
    }
    for (size_t row = 0; row < count; ++row) {
        auto& cell = cells[row];
        cell.bid = static_cast<double>(values[row][0]);
        cell.ask = static_cast<double>(values[row][1]);
        if (!std::isfinite(cell.bid) || !std::isfinite(cell.ask))
            throw std::runtime_error("nonfinite binned heatmap quantity");
        cell.dominantAsk = values[row][1] > values[row][0];
        cell.code = recording::withSide(recording::encodeSize(cell.dominantAsk ? cell.ask : cell.bid, outputScale),
                                        cell.dominantAsk);
    }
    return cells;
}
} // namespace heatmap
