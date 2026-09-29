#pragma once

#include "Hmc2Store.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recording {

// Native-row sparse columns for the isolated GPU binning experiment. All arrays
// have identical length. The first/last index of each minute is in offsets.
// Row indices are relative to baseRow on the least common compatible price
// tick across the range, preserving exact integer grid alignment.
struct RecordingEntries {
    struct Coverage {
        int32_t bidLo = 1, bidHi = 0, askLo = 1, askHi = 0;
    };
    int64_t startMs = 0;
    int64_t baseRow = 0;
    double nativeTick = 0;
    double priceScale = 100;
    SizeScale sizeScale;
    std::vector<uint32_t> column, row, side;
    std::vector<float> size;
    std::vector<uint32_t> offsets;
    std::vector<Coverage> coverage;
    std::vector<uint32_t> observedMs;
    double loadMs = 0, decodeMs = 0;

    uint32_t columns() const { return static_cast<uint32_t>(coverage.size()); }
    uint64_t gpuBytes() const { return column.size() * 16ull + offsets.size() * 4ull + coverage.size() * 20ull; }
};

// A reader never acquires the writer lock and ignores a torn append tail.
// Throws on incompatible native grids, invalid dimensions, or a read failure.
RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs);
RecordingEntries syntheticRecordingEntries(uint32_t count);

} // namespace recording
