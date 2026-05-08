#include "HmcolFormat.hpp"

#include <array>
#include <cstring>

namespace hmcol {

namespace {

// Standard CRC-32 (IEEE 802.3) reversed-polynomial table. 0xEDB88320 == reverse(0x04C11DB7).
constexpr std::array<uint32_t, 256> kCrc32Table = []() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}();

// Number of leading bytes of FileHeader covered by headerCrc32 (everything up to,
// but not including, the headerCrc32 field itself).
constexpr std::size_t kFileHeaderCrcSpan = offsetof(FileHeader, headerCrc32);

} // namespace

uint32_t crc32Update(uint32_t prev, const void* data, std::size_t size) noexcept {
    uint32_t c = prev ^ 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        c = kCrc32Table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void finalizeFileHeaderCrc(FileHeader& header) noexcept {
    header.headerCrc32 = crc32(&header, kFileHeaderCrcSpan);
}

bool verifyFileHeader(const FileHeader& header) noexcept {
    if (header.magic != kMagic) return false;
    if (header.version != kVersion) return false;
    const uint32_t expected = crc32(&header, kFileHeaderCrcSpan);
    return expected == header.headerCrc32;
}

bool verifyRecord(const RecordHeader& header,
                  const void* payload,
                  std::size_t payloadSize) noexcept {
    if (header.bucketStartMs == kEmptySlotSentinel) return false;
    const uint32_t expected = crc32(payload, payloadSize);
    return expected == header.recordCrc32;
}

} // namespace hmcol
