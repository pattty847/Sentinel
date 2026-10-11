#include "Crc32.hpp"
#include <array>

namespace sentinel {
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

} // namespace

uint32_t crc32Update(uint32_t prev, const void* data, std::size_t size) noexcept {
    uint32_t c = prev ^ 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        c = kCrc32Table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace sentinel
