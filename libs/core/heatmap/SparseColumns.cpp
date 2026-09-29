#include "SparseColumns.hpp"
#include <stdexcept>

namespace heatmap {
uint32_t packRowSide(int64_t row, int64_t baseRow, bool ask) {
    if (row < baseRow || static_cast<uint64_t>(row) - static_cast<uint64_t>(baseRow) > 0x7fffffffu)
        throw std::invalid_argument("sparse native row exceeds packed width");
    return static_cast<uint32_t>(row - baseRow) | (uint32_t(ask) << 31);
}
uint64_t coveredMs(const std::vector<CoverageRun>& runs, int64_t row) {
    auto it = std::upper_bound(runs.begin(), runs.end(), row,
        [](int64_t value, const CoverageRun& run) { return value < run.lo; });
    if (it == runs.begin()) return 0;
    const auto& run = *std::prev(it);
    return row <= run.hi ? run.coveredMs : 0;
}
long double entryNumerator(const NativeColumn& column, size_t index) {
    if (!column.numerators.empty()) return column.numerators[index];
    const auto& entry = column.entries[index];
    return static_cast<long double>(recording::decodeSize(entry.code, column.sizeScale)) * entry.coveredMs;
}
void validate(const SparseColumns& data) {
    auto require = [](bool ok) { if (!ok) throw std::invalid_argument("invalid sparse columns"); };
    require(!data.symbol.empty() && (data.layer == "near" || data.layer == "deep"));
    require(data.tfMs >= kMinuteMs && data.tfMs <= kDayMs && data.tfMs % kMinuteMs == 0);
    require(data.startMs >= 0 && data.endMs >= data.startMs &&
            data.startMs % data.tfMs == 0 && data.endMs % data.tfMs == 0);
    int64_t previous = -1;
    for (const auto& column : data.columns) {
        require(column.bucketStartMs >= data.startMs && column.bucketStartMs < data.endMs &&
                column.bucketStartMs % data.tfMs == 0 && column.bucketStartMs > previous);
        previous = column.bucketStartMs;
        require(column.observedMs > 0 && column.observedMs <= uint64_t(data.tfMs));
        uint64_t observed = 0;
        for (const auto& native : column.native) {
            require(native.grid.rowTickUnits > 0 && std::isfinite(native.grid.priceScale) && native.grid.priceScale > 0);
            require(std::isfinite(native.sizeScale.floor) && native.sizeScale.floor > 0 &&
                    std::isfinite(native.sizeScale.codesPerOctave) && native.sizeScale.codesPerOctave > 0);
            require(native.observedMs > 0 && native.observedMs <= column.observedMs);
            observed += native.observedMs;
            require(observed <= column.observedMs);
            for (const auto& runs : native.coverage) {
                int64_t last = std::numeric_limits<int64_t>::min();
                for (const auto& run : runs) {
                    require(run.lo > last && run.hi >= run.lo && run.hi < std::numeric_limits<int64_t>::max() &&
                            run.coveredMs > 0 && run.coveredMs <= native.observedMs);
                    last = run.hi;
                }
            }
            require(native.numerators.empty() || native.numerators.size() == native.entries.size());
            for (size_t i = 0; i < native.entries.size(); ++i) {
                const auto& entry = native.entries[i];
                require(native.baseRow <= std::numeric_limits<int64_t>::max() - entry.row());
                require(entry.code <= recording::kMaxCode && entry.coveredMs <= native.observedMs);
                require(std::isfinite(entryNumerator(native, i)) && entryNumerator(native, i) >= 0);
            }
        }
        require(observed == column.observedMs);
    }
}
} // namespace heatmap
