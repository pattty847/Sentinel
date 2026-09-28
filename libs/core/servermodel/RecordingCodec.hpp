/*
Sentinel — RecordingCodec
Role: fixed-scale size codes and varints for recording v2 (docs/research/2026-09-recording-v2.md).
A size maps to a 15-bit log code on one scale forever, so sizes recorded months
apart compare directly. Bit 15 carries the side (1 = ask), matching the heatmap's
existing bid/ask-by-high-bit convention.
Threading: pure functions, any thread.
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <limits>
#include <optional>

namespace recording {

struct SizeScale {
    double floor = 1e-6;           // smallest size that gets a nonzero code (base units)
    double codesPerOctave = 819.0; // 40 octaves fit in 15 bits at ~0.085% precision
};

constexpr uint16_t kSideAskBit = 0x8000u;
constexpr uint16_t kMaxCode = 0x7FFFu;

// 0 means empty. Sizes below the floor round up to code 1 so a real order never vanishes.
inline uint16_t encodeSize(double size, const SizeScale& scale = {}) {
    if (!(size > 0.0) || !std::isfinite(size) || !(scale.floor > 0.0) || !std::isfinite(scale.floor) ||
        !(scale.codesPerOctave > 0.0) || !std::isfinite(scale.codesPerOctave)) {
        return 0;
    }
    const double code = std::round(std::log2(size / scale.floor) * scale.codesPerOctave) + 1.0;
    return static_cast<uint16_t>(std::clamp(code, 1.0, static_cast<double>(kMaxCode)));
}

inline double decodeSize(uint16_t code, const SizeScale& scale = {}) {
    const uint16_t magnitude = code & kMaxCode;
    if (magnitude == 0) {
        return 0.0;
    }
    return scale.floor * std::exp2((static_cast<double>(magnitude) - 1.0) / scale.codesPerOctave);
}

inline uint16_t withSide(uint16_t code, bool isAsk) {
    return isAsk ? static_cast<uint16_t>(code | kSideAskBit) : static_cast<uint16_t>(code & kMaxCode);
}

inline bool isAsk(uint16_t code) { return (code & kSideAskBit) != 0; }

// LEB128 unsigned varint.
inline void putVarint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<uint8_t>(value | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

// Returns false on truncated or overlong input. Advances pos on success.
inline bool getVarint(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= size) {
            return false;
        }
        const uint8_t byte = data[pos++];
        if (shift == 63 && (byte & 0xFEu) != 0) return false;
        value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            return true;
        }
    }
    return false;
}

// Checked nearest integer price conversion: do not floor floating quotients.
inline std::optional<int64_t> priceUnits(double price, double scale) {
    if (!std::isfinite(price) || price <= 0 || !std::isfinite(scale) || scale <= 0) return {};
    const double scaled = price * scale;
    // INT64_MAX rounds to 2^63 as double, so the upper bound is exclusive.
    if (!std::isfinite(scaled) || scaled >= 0x1p63 || scaled < 0.5) return {};
    return std::llround(scaled);
}
constexpr int64_t floorDiv(int64_t value, int64_t divisor) {
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

// Zigzag so small negative row indices stay short.
inline uint64_t zigzag(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
inline int64_t unzigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1u); }

} // namespace recording
