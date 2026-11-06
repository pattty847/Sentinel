#pragma once
#include "SparseColumns.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace heatmap {
// v2: the chunk key names a neutral source id instead of an HMC2 layer code.
// v3: every source serves stored 1h and 1d levels built from its minutes; their
// columns are composed and carry exact numerators (docs/MARKETDATA.md).
inline constexpr uint16_t kChunkWireVersion = 3;
// Chunk sources. The wire carries only the id. The hmc2.* ids are MIGRATION-ONLY:
// they name the HMC2 near/deep recording layers while HMC2 is the source. Nothing
// may branch product or renderer behaviour on them; raw-derived levels add new ids.
struct ChunkSource {
    std::string_view id, hmc2Layer;
};
inline constexpr std::array<ChunkSource, 2> kChunkSources{{
    {"hmc2.near", "near"},
    {"hmc2.deep", "deep"},
}};
const ChunkSource* findChunkSource(std::string_view id);
const ChunkSource* chunkSourceForHmc2Layer(std::string_view layer);
// Stored levels, finest first. 1m is recorded; 1h and 1d are rollups of it.
inline constexpr std::array<int64_t, 3> kChunkLevels{kMinuteMs, kHourMs, kDayMs};
// A 1d chunk holds this many UTC days (a fixed span keeps key arithmetic a
// modulo; see docs/MARKETDATA.md for the size bound).
inline constexpr int64_t kDayChunkDays = 16;
// Fixed chunk span for a (source, stored level): 1m -> 1 UTC hour, 1h -> 1 UTC
// day, 1d -> kDayChunkDays UTC days. Zero for an unknown source or level.
int64_t chunkSpanMs(std::string_view source, int64_t levelMs);
int64_t chunkSpanMs(int64_t levelMs);
// Chunk starts are whole spans after 2000-01-01 UTC, the first supported day
// (for 1m and 1h this is the same as Unix-epoch alignment). chunkStartOf is the
// start of the level's chunk holding timeMs (timeMs itself for an unknown level).
int64_t chunkStartOf(int64_t levelMs, int64_t timeMs);
bool isChunkStart(int64_t levelMs, int64_t startMs);

struct ChunkKey {
    std::string symbol, source;
    int64_t levelMs = kMinuteMs, startMs = 0;
    bool operator==(const ChunkKey&) const = default;
};
enum class ChunkKind : uint8_t { Chunk = 1, LiveColumn = 2, NotModified = 3, Error = 4 };
// Per-key reply errors. Budget refusals are explicit (Busy), never silent drops.
enum class ChunkError : uint16_t {
    InvalidRequest = 1, // malformed request or key (key fields echo what was sent)
    Unavailable = 2,    // no recording source on this server
    Busy = 3,           // per-session or server worker budget exhausted; retry later
    BuildFailed = 4,    // the reader or encoder failed for this key
};
const char* chunkErrorName(ChunkError code);
struct ChunkState {
    bool sealed = false;
    int64_t committedThroughMs = 0;
    uint64_t revision = 0;
};
struct ChunkFrame {
    ChunkKind kind = ChunkKind::Chunk;
    ChunkKey key;
    ChunkState state;
    SparseColumns columns;
    uint64_t contentHash = 0; // decoded FNV-1a of header prefix and uncompressed payload
    // Error kind only. The message is at most 255 bytes of server text.
    ChunkError error = ChunkError::InvalidRequest;
    std::string message;
};
// SHC1 is little-endian. All lengths and counts are checked before allocation.
// Throws invalid_argument for malformed input or a wire-version mismatch.
// Kinds: Chunk (full body), NotModified (key, state and content hash of the chunk
// the requester already holds; no payload), Error (echoed key, code, message).
// LiveColumn: same native body, with an extra i64 tailStart after end. The key
// names the open minute's chunk; the body may extend into the preceding chunk.
struct ChunkEncodeScratch { std::vector<uint8_t> raw, compressed; };
std::vector<uint8_t> encodeChunk(const ChunkFrame& frame);
std::vector<uint8_t> encodeChunk(const ChunkFrame& frame, ChunkEncodeScratch& scratch);
ChunkFrame decodeChunk(std::span<const uint8_t> wire);
// Content hash of an encoded Chunk-kind frame, from its header only (no payload
// decode). For bytes this process encoded or already validated.
uint64_t chunkContentHash(std::span<const uint8_t> chunkWire);
// Request identity is transport metadata. Cached SHC1 bytes never contain it.
std::vector<uint8_t> encodeChunkEnvelope(uint64_t requestId, std::span<const uint8_t> chunkWire);
struct ChunkEnvelope { uint64_t requestId = 0; ChunkFrame chunk; };
ChunkEnvelope decodeChunkEnvelope(std::span<const uint8_t> wire);
} // namespace heatmap
