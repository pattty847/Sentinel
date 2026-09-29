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
// Row indices are relative to baseRow on the finest common price tick
// (GCD of source tick units), preserving exact integer grid alignment.
struct RecordingEntries {
    struct Coverage {
        int32_t bidLo = 1, bidHi = 0, askLo = 1, askHi = 0;
    };
    int64_t startMs = 0;
    uint32_t sourceMinutes = 1; // one source column is this many UTC-aligned minutes
    bool preNormalized = false; // selected sub-hour columns already use per-row covered duration
    int64_t baseRow = 0;
    double nativeTick = 0;
    double priceScale = 100;
    SizeScale sizeScale;
    std::vector<uint32_t> rowSide, code;
    std::vector<uint32_t> entryCoveredMs; // hour rollup numerator weight; empty for minutes
    std::vector<uint32_t> offsets;
    std::vector<Coverage> coverage;
    std::vector<uint32_t> observedMs;
    std::vector<uint32_t> nativeFactor; // native tick / common fine tick, per minute
    std::vector<SizeScale> columnScale; // original HMC2 log-code scale, per minute
    std::vector<std::array<int32_t, 4>> coverageRuns; // lo, hi, coveredMs, side
    std::vector<uint32_t> coverageRunOffsets; // two side slices per source column
    double loadMs = 0, decodeMs = 0;

    uint32_t columns() const { return static_cast<uint32_t>(coverage.size()); }
    uint64_t gpuBytes() const {
        uint64_t packedBytes = rowSide.size() * 4ull;
        for (const auto value : rowSide) if ((value & 0x7fffffffu) > 0xffffu) {
            packedBytes = ((rowSide.size() + 1ull) / 2ull) * 12ull;
            break;
        }
        return packedBytes + entryCoveredMs.size() * 4ull +
            offsets.size() * 4ull + coverage.size() * (sizeof(Coverage) + sizeof(uint32_t) +
            sizeof(uint32_t) + 16ull) + coverageRuns.size() * 16ull + coverageRunOffsets.size() * 4ull;
    }
};

struct BinCell { float bid = 0, ask = 0; bool valid = false; };
// CPU reference for exact timeframe and GPU parity tests.
BinCell binRecordingCell(const RecordingEntries &data, uint32_t first, uint32_t end,
                         uint32_t rowLo, uint32_t rowHi);

// A reader never acquires the writer lock and ignores a torn append tail.
// Throws on incompatible native grids, invalid dimensions, or a read failure.
RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs, uint32_t sourceMinutes = 1);
// Use persisted schema-4 hours, composing an unpersisted tail from minutes.
// A mixed-grid tail falls back to raw minutes for the complete range.
RecordingEntries loadHourEntriesWithMinuteTail(const std::filesystem::path &root,
                                               const std::string &symbol, const std::string &layer,
                                               int64_t startMs, int64_t endMs);
// Explicit selected timeframes below an hour are composed once from minute
// records, per native row/side and covered duration. Mixed grids use raw data.
RecordingEntries loadComposedMinuteEntries(const std::filesystem::path &root,
                                           const std::string &symbol, const std::string &layer,
                                           int64_t startMs, int64_t endMs, uint32_t timeframeMinutes);
RecordingEntries loadComposedHourEntries(const std::filesystem::path &root,
                                         const std::string &symbol, const std::string &layer,
                                         int64_t startMs, int64_t endMs, uint32_t timeframeMinutes);
// Concatenate adjacent independently decoded ranges without rereading the
// already painted recent range.
RecordingEntries joinRecordingEntries(const RecordingEntries &older,
                                      const RecordingEntries &recent);
RecordingEntries syntheticRecordingEntries(uint32_t count);

} // namespace recording
