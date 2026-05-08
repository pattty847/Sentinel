#pragma once

// HMCL — derived heatmap column on-disk format. Phase 1 v1.
//
// File path:  data/heatmap/<SYMBOL>/<TF_MS>/v1/<YYYY-MM-DD>.hmcol
// Layout:     [FileHeader][Slot 0][Slot 1]...[Slot N-1]
// Slots are time-addressed: slot = (bucketStartMs - dayStartMs) / timeframeMs.
// Empty slots have RecordHeader{} zeroed (bucketStartMs == 0). Reader skips them.
// Per-record CRC32 lets readers skip torn writes from a kill -9.
//
// See docs/private/plans/F1_HEATMAP_PERSISTENCE.md.

#include <cstddef>
#include <cstdint>

namespace hmcol {

constexpr uint32_t kMagic   = 0x484D434C; // 'HMCL' (little-endian)
constexpr uint16_t kVersion = 1;

constexpr uint8_t  kIntensityFormatU16 = 1;
constexpr uint8_t  kLiquidityFormatNone = 0;
constexpr uint8_t  kLiquidityFormatU16  = 1;

// Record flags (bit positions).
constexpr uint32_t kFlagHasLiquidity     = 1u << 0;
constexpr uint32_t kFlagRecenterBoundary = 1u << 1; // reserved; diagnostic only

// Sentinel: empty slot. Readers must treat any record with bucketStartMs == 0 as empty.
constexpr int64_t kEmptySlotSentinel = 0;

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic;            // kMagic
    uint16_t version;          // kVersion
    uint16_t reserved0;        // 0
    int64_t  timeframeMs;      // bucket size for this file (e.g. 60000 for 1m)
    int32_t  gridHeight;       // rows per column; constant within one file
    uint8_t  intensityFormat;  // kIntensityFormatU16
    uint8_t  liquidityFormat;  // kLiquidityFormatU16 or kLiquidityFormatNone
    uint16_t reserved1;        // 0
    int64_t  dayStartMs;       // UTC midnight for this file
    char     symbol[24];       // null-padded, not necessarily null-terminated
    uint32_t headerCrc32;      // CRC of all bytes preceding this field
    uint32_t reserved2;        // 0
};
static_assert(sizeof(FileHeader) == 64, "FileHeader must be exactly 64 bytes");

struct RecordHeader {
    int64_t  bucketStartMs;    // == kEmptySlotSentinel for empty slot
    int64_t  bucketEndMs;
    double   minPrice;
    double   maxPrice;
    double   tickSize;
    double   liquidityScale;   // 1.0 if no liquidity
    uint32_t flags;            // bitmask of kFlag*
    uint32_t recordCrc32;      // CRC of payload bytes only (not this header)
    // payload follows immediately:
    //   uint16_t intensity[gridHeight];
    //   uint16_t liquidity[gridHeight];   if (flags & kFlagHasLiquidity)
};
static_assert(sizeof(RecordHeader) == 56, "RecordHeader must be exactly 56 bytes");
#pragma pack(pop)

// Total bytes for one slot in a file with the given gridHeight + liquidityFormat.
constexpr std::size_t recordStride(int32_t gridHeight, uint8_t liquidityFormat) noexcept {
    const std::size_t intensityBytes = static_cast<std::size_t>(gridHeight) * sizeof(uint16_t);
    const std::size_t liquidityBytes =
        (liquidityFormat == kLiquidityFormatU16)
            ? static_cast<std::size_t>(gridHeight) * sizeof(uint16_t)
            : 0;
    return sizeof(RecordHeader) + intensityBytes + liquidityBytes;
}

// Byte offset within the file for a given slot index.
constexpr std::size_t slotOffset(int64_t slotIndex,
                                 int32_t gridHeight,
                                 uint8_t liquidityFormat) noexcept {
    return sizeof(FileHeader)
         + static_cast<std::size_t>(slotIndex) * recordStride(gridHeight, liquidityFormat);
}

// Slot index for a given bucketStartMs. Caller is responsible for verifying that
// dayStartMs <= bucketStartMs and that the result is < (msPerDay / timeframeMs).
constexpr int64_t slotForBucket(int64_t bucketStartMs,
                                int64_t dayStartMs,
                                int64_t timeframeMs) noexcept {
    return (bucketStartMs - dayStartMs) / timeframeMs;
}

// UTC midnight (ms since epoch) for the day containing epochMs. Always rounds down.
// Negative epochMs uses C++ floor semantics so dayStartMs <= epochMs holds.
constexpr int64_t dayStartForEpoch(int64_t epochMs) noexcept {
    constexpr int64_t kMsPerDay = 86'400'000;
    int64_t days = epochMs / kMsPerDay;
    if (epochMs < 0 && (epochMs % kMsPerDay) != 0) {
        --days;
    }
    return days * kMsPerDay;
}

// CRC-32 (IEEE 802.3, reversed polynomial 0xEDB88320). Same variant as zlib/PNG.
// Two-step API for streaming CRCs; one-shot helper for the common case.
uint32_t crc32Update(uint32_t prev, const void* data, std::size_t size) noexcept;
inline uint32_t crc32(const void* data, std::size_t size) noexcept {
    return crc32Update(0, data, size);
}

// Writes header.headerCrc32 by CRCing the leading bytes of `header`.
// Pass a freshly-populated FileHeader; this fills the field in place.
void finalizeFileHeaderCrc(FileHeader& header) noexcept;

// True if header magic + version + CRC validate. Does NOT check semantic fields
// (timeframeMs, gridHeight) — callers cross-check those against expected config.
bool verifyFileHeader(const FileHeader& header) noexcept;

// True if record's bucketStartMs != kEmptySlotSentinel and recordCrc32 matches the
// CRC of the supplied payload bytes (intensity + optional liquidity).
bool verifyRecord(const RecordHeader& header,
                  const void* payload,
                  std::size_t payloadSize) noexcept;

} // namespace hmcol
