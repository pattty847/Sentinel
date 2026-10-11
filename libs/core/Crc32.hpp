#pragma once
#include <cstddef>
#include <cstdint>

namespace sentinel {
// CRC-32 (IEEE 802.3, reversed polynomial 0xEDB88320), as used by zlib/PNG.
uint32_t crc32Update(uint32_t prev, const void* data, std::size_t size) noexcept;
inline uint32_t crc32(const void* data, std::size_t size) noexcept {
    return crc32Update(0, data, size);
}
} // namespace sentinel
