#pragma once
#include "servermodel/Hmc2Store.hpp"
#include "servermodel/HmcolFormat.hpp"
#include <bit>
#include <fstream>
#include <zstd.h>

// Independent schema-3 absolute hour writer for upgrade/restart fixtures.
namespace recording_fixture {
using Bytes = std::vector<uint8_t>;
template <class T> inline void put(Bytes &b, T value) {
    using U = std::conditional_t<sizeof(T) == 8, uint64_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint16_t>>;
    const auto bits = std::bit_cast<U>(value);
    for (size_t n = 0; n < sizeof(T); ++n) b.push_back(static_cast<uint8_t>(bits >> (8 * n)));
}
inline void set32(Bytes &b, size_t at, uint32_t value) {
    for (size_t n = 0; n < 4; ++n) b[at + n] = static_cast<uint8_t>(value >> (8 * n));
}
inline void schema3Hour(const std::filesystem::path &path, const recording::Hmc2Record &r) {
    using namespace recording;
    Bytes b;
    put(b, kHmc2Magic); put(b, uint16_t{3}); put(b, uint32_t{0}); put(b, uint32_t{0});
    for (const auto &value : {r.header.symbol, r.header.layer}) {
        put(b, static_cast<uint16_t>(value.size())); b.insert(b.end(), value.begin(), value.end());
    }
    put(b, r.header.tfMs); put(b, r.header.priceScale); put(b, r.header.rowTickUnits);
    put(b, r.header.sizeScale.floor); put(b, r.header.sizeScale.codesPerOctave); put(b, r.header.configHash);
    set32(b, 6, b.size()); set32(b, 10, hmcol::crc32(b.data(), b.size()));
    Bytes raw;
    put(raw, r.bucketStartMs); put(raw, r.observedMs); put(raw, r.flags);
    for (auto v : {r.bidRowLo, r.bidRowHi, r.askRowLo, r.askRowHi}) put(raw, v);
    for (auto v : {r.midOpen, r.midClose, r.midMin, r.midMax}) put(raw, v);
    put(raw, static_cast<uint32_t>(r.entries.size())); raw.push_back(0); put(raw, int64_t{0});
    int64_t previous = 0;
    for (const auto &e : r.entries) {
        putVarint(raw, zigzag(e.row - previous)); raw.push_back(e.isAsk);
        putVarint(raw, zigzag(e.twapCode)); putVarint(raw, zigzag(int64_t(e.peakCode) - e.twapCode));
        previous = e.row;
    }
    Bytes compressed(ZSTD_compressBound(raw.size()));
    const auto n = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(n)) throw std::runtime_error("fixture zstd");
    compressed.resize(n);
    put(b, kHmc2RecordMagic); put(b, static_cast<uint32_t>(n)); put(b, static_cast<uint32_t>(raw.size()));
    put(b, hmcol::crc32(compressed.data(), n)); b.insert(b.end(), compressed.begin(), compressed.end());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(reinterpret_cast<const char *>(b.data()), b.size());
}
}
