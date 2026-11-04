#pragma once
#include <zstd.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sentinel::roller {
// JSON transport for exact paired bytes: four-byte LE raw length, one zstd frame,
// lowercase hex. Both enclosed state formats already carry their own CRC32.
inline std::string packCheckpointBytes(const std::vector<uint8_t>& raw) {
    constexpr size_t MaxBytes=128*1024*1024;
    if (raw.empty() || raw.size()>MaxBytes) throw std::runtime_error("C checkpoint blob size");
    std::vector<uint8_t> packed(4+ZSTD_compressBound(raw.size()));
    for (int i=0;i<4;++i) packed[i]=uint8_t(raw.size()>>(8*i));
    const auto n=ZSTD_compress(packed.data()+4,packed.size()-4,raw.data(),raw.size(),3);
    if (ZSTD_isError(n) || n>MaxBytes) throw std::runtime_error("C checkpoint compression failed");
    std::string hex; hex.reserve((n+4)*2);
    constexpr char digits[]="0123456789abcdef";
    for (size_t i=0;i<n+4;++i) { hex.push_back(digits[packed[i]>>4]); hex.push_back(digits[packed[i]&15]); }
    return hex;
}
inline std::vector<uint8_t> unpackCheckpointBytes(const std::string& hex) {
    constexpr size_t MaxBytes=128*1024*1024;
    if (hex.size()<10 || hex.size()%2 || hex.size()>2*(MaxBytes+4))
        throw std::runtime_error("C checkpoint encoded size");
    const auto digit=[](char c) -> uint8_t {
        if (c>='0' && c<='9') return uint8_t(c-'0');
        if (c>='a' && c<='f') return uint8_t(c-'a'+10);
        throw std::runtime_error("C checkpoint invalid hex");
    };
    std::vector<uint8_t> packed(hex.size()/2);
    for (size_t i=0;i<packed.size();++i) packed[i]=uint8_t((digit(hex[2*i])<<4)|digit(hex[2*i+1]));
    uint32_t n=0;
    for (int i=0;i<4;++i) n|=uint32_t(packed[i])<<(8*i);
    if (!n || n>MaxBytes || ZSTD_findFrameCompressedSize(packed.data()+4,packed.size()-4)!=packed.size()-4)
        throw std::runtime_error("C checkpoint compression framing");
    std::vector<uint8_t> raw(n);
    if (ZSTD_decompress(raw.data(),raw.size(),packed.data()+4,packed.size()-4)!=raw.size())
        throw std::runtime_error("C checkpoint decompression failed");
    return raw;
}
}
