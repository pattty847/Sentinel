#pragma once
// Canonical little-endian/LEB128 primitives shared by the flow byte codecs.
#include "FlowRowStep.hpp"
#include "Crc32.hpp"
#include <array>
#include <span>
#include <string>
#include <vector>
namespace sentinel::flow::chunks {
namespace detail {
struct Out {
    std::vector<uint8_t> b;
    void fixed(uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void var(uint64_t v) { // LEB128, unsigned
        while (v >= 0x80) { b.push_back(uint8_t(v) | 0x80); v >>= 7; }
        b.push_back(uint8_t(v));
    }
    void svar(int64_t v) { var(recording::zigzag(v)); }
};
struct In {
    std::span<const uint8_t> b;
    size_t p = 0;
    [[noreturn]] static void fail(const char* what) { throw std::runtime_error(std::string("flow chunk ") + what); }
    uint64_t fixed(size_t n) {
        if (n > b.size() - p) fail("truncated");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(b[p++]) << (8 * i);
        return v;
    }
    // LEB128: at most 10 bytes, the tenth at most 1, no trailing zero byte (as FLB1, FlowCodec.cpp:33-45).
    uint64_t var() {
        uint64_t v = 0;
        for (unsigned i = 0; i < 10; ++i) {
            const auto byte = fixed(1);
            if (i == 9 && byte > 1) fail("varint overflow");
            v |= (byte & 0x7f) << (7 * i);
            if (!(byte & 0x80)) {
                if (i && byte == 0) fail("noncanonical varint");
                return v;
            }
        }
        fail("varint overflow");
    }
    int64_t svar() { const auto u = var(); return int64_t(u >> 1) ^ -int64_t(u & 1); }
    uint64_t bounded(uint64_t v, uint64_t max) { if (v > max) fail("value out of range"); return v; }
};
} // namespace detail

inline uint32_t crc32(std::span<const uint8_t> bytes, uint32_t crc = 0) {
    return sentinel::crc32Update(crc, bytes.data(), bytes.size());
}
} // namespace sentinel::flow::chunks
