#pragma once
#include "SparseColumns.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace heatmap {
inline constexpr uint16_t kChunkWireVersion = 1;
struct ChunkKey {
    std::string symbol, layer;
    int64_t levelMs = kMinuteMs, startMs = 0;
    bool operator==(const ChunkKey&) const = default;
};
enum class ChunkKind : uint8_t { Chunk = 1, LiveColumn = 2, NotModified = 3, Error = 4 };
struct ChunkState {
    bool sealed = false;
    int64_t committedThroughMs = 0;
    uint64_t revision = 0;
};
struct ChunkFrame {
    ChunkKind kind = ChunkKind::Chunk;
    uint64_t requestId = 0;
    ChunkKey key;
    ChunkState state;
    SparseColumns columns;
    uint64_t contentHash = 0; // filled by encode/decode; FNV-1a of uncompressed payload
};
// SHC1 v1 is little-endian. All lengths and counts are checked before allocation.
// Throws invalid_argument for malformed input or a wire-version mismatch.
std::vector<uint8_t> encodeChunk(const ChunkFrame& frame);
ChunkFrame decodeChunk(std::span<const uint8_t> wire);
} // namespace heatmap
