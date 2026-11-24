// SCK1 integers are little endian. Check all lengths before allocating, including
// zstd's declared content size. Hash excludes transport kind/state/revision/epoch.
#include "ChunkFrame.hpp"
#include "InstrumentSpec.hpp"
#include "flow/FlowRowStep.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>
#include <zstd.h>

namespace protocol::chunks {
namespace {
[[noreturn]] void fail() { throw std::invalid_argument("invalid SCK1 frame"); }
struct Out {
    std::vector<uint8_t> bytes;
    void u(uint64_t value, size_t n) { for (size_t i = 0; i < n; ++i) bytes.push_back(uint8_t(value >> (8 * i))); }
    void str(const std::string& s, size_t limit) {
        if (s.size() > limit) fail();
        u(s.size(), 1); bytes.insert(bytes.end(), s.begin(), s.end());
    }
};
struct In {
    std::span<const uint8_t> bytes;
    size_t pos = 0;
    uint64_t u(size_t n) {
        if (n > bytes.size() - pos) fail();
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(bytes[pos++]) << (8 * i);
        return v;
    }
    int64_t i() { return std::bit_cast<int64_t>(u(8)); }
    std::string str(size_t limit) {
        const auto n = u(1);
        if (n > limit || n > bytes.size() - pos) fail();
        std::string s(reinterpret_cast<const char*>(bytes.data() + pos), size_t(n)); pos += n; return s;
    }
};
void key(Out& out, const Key& k) {
    out.str(k.instrument, 72); out.str(k.channel, 32);
    out.u(uint64_t(k.levelMs), 8); out.u(k.priceStep, 8); out.u(uint64_t(k.startMs), 8);
}
void extent(Out& out, const Frame& f) { out.u(uint64_t(f.endMs), 8); out.u(uint64_t(f.committedThroughMs), 8); }
uint64_t hash(std::span<const uint8_t> bytes, uint64_t h = 14695981039346656037ULL) {
    for (auto b : bytes) { h ^= b; h *= 1099511628211ULL; } return h;
}
bool validState(const Frame& f) {
    return validKey(f.key) && f.sealed && f.revision == 0 &&
        f.endMs == f.key.startMs + heatmap::chunkSpanMs(f.key.levelMs) && f.committedThroughMs == f.endMs;
}
bool knownError(Error code) { return uint16_t(code) >= 1 && uint16_t(code) <= 4; }
}
bool validInstrument(const std::string& id) {
    if (id.size() > 72) return false;
    const auto colon = id.find(':');
    if (colon == std::string::npos) return false;
    const auto venue = id.substr(0, colon);
    if (venue != "coinbase" && venue != "kalshi") return false;
    try { sentinel::validateInstrumentId(id.substr(colon + 1), venue == "coinbase" ? sentinel::Venue::Coinbase : sentinel::Venue::Kalshi); }
    catch (const std::exception&) { return false; }
    return true;
}
bool validKey(const Key& k) {
    const auto span = heatmap::chunkSpanMs(k.levelMs);
    return validInstrument(k.instrument) && span &&
        (k.channel == "flow.summary" ? k.priceStep == 0 :
         k.channel == "flow.rows" && sentinel::flow::chunks::validStep(k.priceStep)) &&
        k.startMs >= recording::kHmc2MinMs && k.startMs <= recording::kHmc2EndMs - span &&
        heatmap::isChunkStart(k.levelMs, k.startMs);
}
uint64_t contentHash(const Frame& f) {
    Out prefix; key(prefix, f.key); extent(prefix, f); prefix.u(f.payloadVersion, 2);
    return hash(f.payload, hash(prefix.bytes));
}
std::vector<uint8_t> encode(const Frame& f) {
    if (f.kind != Kind::Chunk && f.kind != Kind::NotModified && f.kind != Kind::Error) fail();
    if (!f.sealed || (f.kind != Kind::Error && !validState(f))) fail();
    Out out; out.bytes = {'S', 'C', 'K', '1'};
    out.u(WireVersion, 2); out.u(uint8_t(f.kind), 1); out.u(f.sealed, 1); key(out, f.key);
    if (f.kind == Kind::Error) {
        if (!knownError(f.error)) fail();
        out.u(uint16_t(f.error), 2); out.str(f.message, 255); return std::move(out.bytes);
    }
    extent(out, f); out.u(f.revision, 8); out.u(f.epoch, 8);
    out.u(f.kind == Kind::NotModified ? f.contentHash : contentHash(f), 8);
    if (f.kind == Kind::NotModified) return std::move(out.bytes);
    if (f.payload.size() > MaxFrameBytes || f.payloadVersion != PayloadVersion) fail();
    std::vector<uint8_t> compressed(ZSTD_compressBound(f.payload.size()));
    const auto n = ZSTD_compress(compressed.data(), compressed.size(), f.payload.data(), f.payload.size(), 1);
    if (ZSTD_isError(n)) fail();
    const bool useZstd = n * 10 <= f.payload.size() * 9;
    const auto body = useZstd ? std::span<const uint8_t>(compressed.data(), n) : std::span<const uint8_t>(f.payload);
    out.u(f.payloadVersion, 2); out.u(useZstd, 1); out.u(f.payload.size(), 4); out.u(body.size(), 4);
    if (out.bytes.size() + body.size() > MaxFrameBytes - 14) fail();
    out.bytes.insert(out.bytes.end(), body.begin(), body.end()); return std::move(out.bytes);
}
Frame decode(std::span<const uint8_t> bytes) {
    if (bytes.size() > MaxFrameBytes - 14) fail();
    In in{bytes};
    if (in.u(4) != 0x314b4353 || in.u(2) != WireVersion) fail();
    Frame f; f.kind = Kind(in.u(1));
    const auto state = in.u(1); if (state != 1) fail(); f.sealed = true;
    f.key.instrument = in.str(72); f.key.channel = in.str(32);
    f.key.levelMs = in.i(); f.key.priceStep = in.u(8); f.key.startMs = in.i();
    if (f.kind == Kind::Error) {
        f.error = Error(in.u(2)); f.message = in.str(255);
        if (!knownError(f.error) || in.pos != bytes.size()) fail(); return f;
    }
    if (f.kind != Kind::Chunk && f.kind != Kind::NotModified) fail();
    f.endMs = in.i(); f.committedThroughMs = in.i();
    f.revision = in.u(8); f.epoch = in.u(8); f.contentHash = in.u(8);
    if (!validState(f)) fail();
    if (f.kind == Kind::NotModified) { if (in.pos != bytes.size()) fail(); return f; }
    f.payloadVersion = uint16_t(in.u(2)); f.codec = uint8_t(in.u(1));
    const auto raw = in.u(4), stored = in.u(4);
    if (f.payloadVersion != PayloadVersion || f.codec > 1 || raw > MaxFrameBytes || stored != bytes.size() - in.pos) fail();
    const auto body = bytes.subspan(in.pos);
    if (f.codec == 0) {
        if (raw != stored) fail(); f.payload.assign(body.begin(), body.end());
    } else {
        if (ZSTD_getFrameContentSize(body.data(), body.size()) != raw ||
            ZSTD_findFrameCompressedSize(body.data(), body.size()) != stored) fail();
        f.payload.resize(size_t(raw));
        const auto n = ZSTD_decompress(f.payload.data(), f.payload.size(), body.data(), body.size());
        if (ZSTD_isError(n) || n != raw) fail();
    }
    if (contentHash(f) != f.contentHash) fail(); return f;
}
Envelope decodeEnvelope(std::span<const uint8_t> bytes) {
    if (bytes.size() > MaxFrameBytes || bytes.size() < 18) fail();
    In in{bytes}; if (in.u(4) != 0x31454853 || in.u(2) != 1) fail();
    Envelope out; out.requestId = in.u(8);
    const auto body = bytes.subspan(in.pos);
    if (std::memcmp(body.data(), "SHC1", 4) == 0) out.frame = heatmap::decodeChunk(body);
    else if (std::memcmp(body.data(), "SCK1", 4) == 0) out.frame = decode(body);
    else fail();
    return out;
}
} // namespace protocol::chunks
