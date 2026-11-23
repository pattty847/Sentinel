#pragma once
// Encoder input has explicit grids and coverage; callers supply only sealed bars.
// All malformed payloads throw. The summary is an exact prefix of the rows bytes.
#include "FlowChunk.hpp"
#include "FlowRowStep.hpp"
namespace sentinel::flow::chunks {
struct Stats { uint64_t lowRowMismatch = 0, nativeTickMismatch = 0; };
// barGrids, when supplied, has one original recorder grid per bar (before its
// stored step); this is required to distinguish a coarse live tick from a new grid.
ChunkData fromBars(std::span<const FlowBar> bars, int64_t levelMs, int64_t startMs, uint32_t slotCount,
                   Stats* stats = nullptr, std::span<const FlowGrid> barGrids = {},
                   std::span<const CoverageRun> coverage = {});
std::vector<FlowBar> toBars(const ChunkData& chunk);
std::vector<FlowBar> toBars(const FlowChunk& chunk);
std::vector<uint8_t> encode(const ChunkData& chunk, bool summaryOnly = false, uint32_t* summaryLength = nullptr);
FlowChunk decode(std::span<const uint8_t> bytes, FlowChunkKey key, uint16_t slotCount, uint64_t epoch = 0);
// Explicit slot count also supports the one-day piece stored in entry 25.
FlowChunk decode(std::span<const uint8_t> bytes, int64_t levelMs, int64_t startMs,
                 uint32_t slotCount, bool summaryOnly = false, uint64_t priceStep = 1);
ChunkData rebin(const ChunkData& chunk, uint64_t step);
ChunkData assembleDayChunk(int64_t startMs, std::span<const std::vector<uint8_t>> pieces, Status absent,
                           std::span<const uint64_t> steps = {});
} // namespace sentinel::flow::chunks
