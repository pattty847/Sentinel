#pragma once

#include "Hmc2Store.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recording {

// Native-row sparse columns for the isolated GPU binning experiment. rowSide
// carries the ask bit in bit 31; code carries the 15-bit HMC2 log size code.
// The first/last index of each minute is in offsets, so no entry stores a col.
// Row indices are relative to baseRow on the least common compatible price
// tick across the range, preserving exact integer grid alignment.
struct RecordingEntries {
    struct Coverage {
        int32_t bidLo = 1, bidHi = 0, askLo = 1, askHi = 0;
    };
    static constexpr uint32_t kLodMinutes = 8;
    struct TimeLod {
        std::vector<uint32_t> rowSide, offsets, observedMs;
        std::vector<float> weightedSize; // decoded size * observed milliseconds
        std::vector<Coverage> coverage;
    } lod;
    static constexpr uint32_t kPriceBlockRows = 16;
    struct PriceLod {
        std::vector<uint32_t> rowSide, offsets;
        std::vector<float> size;
    } priceLod;
    PriceLod timePriceLod; // duration-weighted sizes over eight-minute groups
    struct DensePriceLod {
        std::vector<std::array<float, 2>> sums;
        std::vector<std::array<int32_t, 2>> meta; // [first sum offset, global row group base]
    } dense50, dense100, timeDense50, timeDense100;
    int64_t startMs = 0;
    int64_t baseRow = 0;
    double nativeTick = 0;
    double priceScale = 100;
    SizeScale sizeScale;
    std::vector<uint32_t> rowSide, code;
    std::vector<uint32_t> offsets;
    std::vector<Coverage> coverage;
    std::vector<uint32_t> observedMs;
    double loadMs = 0, decodeMs = 0;

    uint32_t columns() const { return static_cast<uint32_t>(coverage.size()); }
    uint64_t gpuBytes() const {
        return ((rowSide.size() + 1ull) / 2ull) * 12ull + offsets.size() * 4ull + coverage.size() * 20ull +
            lod.rowSide.size() * 8ull + lod.offsets.size() * 4ull + lod.coverage.size() * 20ull +
            priceLod.rowSide.size() * 8ull + priceLod.offsets.size() * 4ull +
            timePriceLod.rowSide.size() * 8ull + timePriceLod.offsets.size() * 4ull +
            (dense50.sums.size() + dense100.sums.size() + timeDense50.sums.size() + timeDense100.sums.size()) * 8ull +
            (dense50.meta.size() + dense100.meta.size() + timeDense50.meta.size() + timeDense100.meta.size()) * 8ull;
    }
};

struct BinCell { float bid = 0, ask = 0; bool valid = false; };
// CPU reference for raw/LOD equivalence tests. The GPU uses the same aligned
// interior groups and raw boundary minutes.
BinCell binRecordingCell(const RecordingEntries &data, uint32_t first, uint32_t end,
                         uint32_t rowLo, uint32_t rowHi, bool useLod, bool usePriceLod = false);

// A reader never acquires the writer lock and ignores a torn append tail.
// Throws on incompatible native grids, invalid dimensions, or a read failure.
RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs);
RecordingEntries syntheticRecordingEntries(uint32_t count);

} // namespace recording
