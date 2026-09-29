#include "SparseColumns.hpp"
#include <stdexcept>

namespace heatmap {
bool isScanned(const SparseColumns& data, int64_t startMs, int64_t endMs) {
    const auto it = std::upper_bound(data.scannedRanges.begin(), data.scannedRanges.end(), startMs,
        [](int64_t start, const SparseColumns::TimeRange& range) { return start < range.startMs; });
    return startMs < endMs && it != data.scannedRanges.begin() && endMs <= std::prev(it)->endMs;
}
BucketState bucketState(const SparseColumns& data, int64_t bucketStartMs) {
    if (data.tfMs <= 0 || bucketStartMs % data.tfMs ||
        bucketStartMs > std::numeric_limits<int64_t>::max() - data.tfMs ||
        !isScanned(data, bucketStartMs, bucketStartMs + data.tfMs)) return BucketState::NotLoaded;
    const auto it = std::lower_bound(data.columns.begin(), data.columns.end(), bucketStartMs,
        [](const SparseColumn& column, int64_t start) { return column.bucketStartMs < start; });
    return it != data.columns.end() && it->bucketStartMs == bucketStartMs ? BucketState::Present : BucketState::Gap;
}
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
uint64_t entryCoveredMs(const NativeColumn& column, size_t index) {
    return column.entryCoveredMs.empty() ? column.observedMs : column.entryCoveredMs.at(index);
}
long double entryNumerator(const NativeColumn& column, size_t index) {
    if (column.composed) {
        if (column.numerators.size() != column.entries.size())
            throw std::invalid_argument("composed heatmap column requires exact numerators");
        return column.numerators.at(index);
    }
    const auto& entry = column.entries[index];
    return static_cast<long double>(recording::decodeSize(entry.code, column.sizeScale)) * entryCoveredMs(column, index);
}
void validate(const SparseColumns& data) {
    auto require = [](bool ok) { if (!ok) throw std::invalid_argument("invalid sparse columns"); };
    require(!data.symbol.empty() && (data.layer == "near" || data.layer == "deep"));
    require(data.tfMs >= kMinuteMs && data.tfMs <= kDayMs && data.tfMs % kMinuteMs == 0);
    require(data.startMs >= 0 && data.endMs >= data.startMs &&
            data.startMs % data.tfMs == 0 && data.endMs % data.tfMs == 0);
    int64_t lastEnd = -1;
    for (const auto& range : data.scannedRanges) {
        require(range.startMs >= data.startMs && range.endMs <= data.endMs && range.startMs < range.endMs &&
                range.startMs > lastEnd && range.startMs % data.tfMs == 0 && range.endMs % data.tfMs == 0);
        lastEnd = range.endMs;
    }
    int64_t previous = -1;
    for (const auto& column : data.columns) {
        require(column.bucketStartMs >= data.startMs && column.bucketStartMs < data.endMs &&
                column.bucketStartMs % data.tfMs == 0 && column.bucketStartMs > previous);
        require(isScanned(data, column.bucketStartMs, column.bucketStartMs + data.tfMs));
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
            require(native.composed ? native.numerators.size() == native.entries.size() : native.numerators.empty());
            require(native.entryCoveredMs.empty() || native.entryCoveredMs.size() == native.entries.size());
            // The scale is monotone: one bound check avoids decoding every raw
            // entry during validation and again during aggregation.
            require(std::isfinite(recording::decodeSize(recording::kMaxCode, native.sizeScale)));
            uint64_t previousKey = 0;
            for (size_t i = 0; i < native.entries.size(); ++i) {
                const auto& entry = native.entries[i];
                require(native.baseRow < std::numeric_limits<int64_t>::max() - entry.row());
                const auto key = (uint64_t(entry.row()) << 1) | entry.isAsk();
                require(!i || key > previousKey);
                previousKey = key;
                require(entry.code <= recording::kMaxCode && entryCoveredMs(native, i) > 0 &&
                        entryCoveredMs(native, i) <= native.observedMs);
                if (native.composed) require(std::isfinite(native.numerators[i]) && native.numerators[i] >= 0);
            }
        }
        require(observed == column.observedMs);
    }
}
} // namespace heatmap
