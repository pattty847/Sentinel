#pragma once
// Decoded fields share one owned allocation. ChunkData is mutable encoder input;
// it is never retained as the decoded/cache representation. No GUI dependencies.
#include "FlowBar.hpp"
#include <memory>
#include <span>
#include <string>
namespace sentinel::flow::chunks {
struct FlowRow {
    int64_t bidVol = 0, askVol = 0; // base units
    uint32_t bidCount = 0, askCount = 0;
    bool operator==(const FlowRow&) const = default;
};
static_assert(sizeof(FlowRow) == 24);
static_assert(alignof(FlowRow) == 8);

struct FlowGrid {
    int64_t rowTickUnits = 0, priceScale = 0, baseScale = 0, quoteScale = 0;
    bool operator==(const FlowGrid&) const = default;
};
static_assert(sizeof(FlowGrid) == 32);

// Served statuses are 1-3. Building exists only inside assembly: a chunk with a
// building slot is not sealed, and encode refuses it.
enum class Status : uint8_t { Complete = 1, Partial = 2, Unrecorded = 3, Building = 4 };
struct CoverageRun {
    uint16_t fromSlot = 0, toSlot = 0; // [from, to)
    Status status = Status::Complete;
    bool operator==(const CoverageRun&) const = default;
};

struct ChunkData {
    int64_t levelMs = 0, startMs = 0;
    uint32_t slotCount = 0;         // from the key, not the payload
    uint64_t priceStep = 1;         // from the key; 0 for a flow.summary decode
    std::vector<FlowGrid> grids;
    std::vector<CoverageRun> coverage;
    // Summary: one entry per present bar, ascending slot.
    std::vector<uint16_t> slot;
    std::vector<uint8_t> grid;      // index into grids
    std::vector<int64_t> open, high, low, close; // price units
    std::vector<int64_t> baseVolume, quoteVolume, buyVol, sellVol;
    std::vector<uint32_t> count;
    std::vector<uint32_t> rowBegin; // bars + 1 entries
    // Rows, parallel arrays.
    std::vector<uint16_t> offset;   // row - low row of its bar, at priceStep
    std::vector<FlowRow> rows;
    size_t bars() const { return slot.size(); }
    bool operator==(const ChunkData&) const = default;
};


struct FlowChunkKey {
    std::string instrument, channel;
    int64_t levelMs = 0;
    uint64_t priceStep = 1;
    int64_t startMs = 0;
    auto operator<=>(const FlowChunkKey&) const = default;
};
struct FlowChunk {
    FlowChunkKey key;
    uint64_t epoch = 0;
    uint16_t slotCount = 0, bars = 0;
    uint32_t rows = 0;
    std::span<const FlowGrid> grids;
    std::span<const CoverageRun> coverage;
    std::span<const int64_t> open, high, low, close, baseVol, quoteVol, buyVol, sellVol;
    std::span<const uint32_t> count, rowBegin;
    std::span<const uint16_t> slot;
    std::span<const uint8_t> grid;
    std::span<const FlowRow> row;
    std::span<const uint16_t> offset;
    // Move-only ownership; spans keep their addresses when the owner moves.
    std::unique_ptr<std::byte[]> storage;
    size_t storageBytes = 0;
};
ChunkData toData(const FlowChunk& chunk);
} // namespace sentinel::flow::chunks
