#include "ChunkCodec.hpp"
#include "../servermodel/RecordingCodec.hpp"
#include "../servermodel/Hmc2Store.hpp"
#include <zstd.h>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace heatmap {
namespace {
constexpr size_t kMaxPayload = 16u * 1024u * 1024u;
constexpr uint32_t kMaxColumns = 1440, kMaxEntries = 8'000'000;
constexpr size_t kColumnRecordMinBytes = 22, kNativeRecordMinBytes = 66;
void fail() { throw std::invalid_argument("invalid SHC1 chunk"); }
struct Writer {
    std::vector<uint8_t> data;
    template<class T> void u(T v) {
        static_assert(std::is_unsigned_v<T>);
        for (unsigned i = 0; i < sizeof(T); ++i) data.push_back(uint8_t(v >> (8 * i)));
    }
    void i(int64_t v) { u(std::bit_cast<uint64_t>(v)); }
    void d(double v) { u(std::bit_cast<uint64_t>(v)); }
    void str(std::string_view s) {
        if (s.size() > 255) fail();
        u(uint8_t(s.size())); data.insert(data.end(), s.begin(), s.end());
    }
};
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    template<class T> T u() {
        static_assert(std::is_unsigned_v<T>);
        if (data.size() - pos < sizeof(T)) fail();
        T v = 0;
        for (unsigned i = 0; i < sizeof(T); ++i) v |= T(data[pos++]) << (8 * i);
        return v;
    }
    int64_t i() { return std::bit_cast<int64_t>(u<uint64_t>()); }
    double d() { return std::bit_cast<double>(u<uint64_t>()); }
    std::string str() {
        const auto n = u<uint8_t>();
        if (data.size() - pos < n) fail();
        std::string s(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n; return s;
    }
    void requireCount(uint64_t count, size_t minBytes) const {
        if (count > (data.size() - pos) / minBytes) fail();
    }
    uint64_t var() {
        uint64_t v = 0;
        if (!recording::getVarint(data.data(), data.size(), pos, v)) fail();
        return v;
    }
};
uint64_t hashBytes(std::span<const uint8_t> bytes) {
    uint64_t h = 14695981039346656037ULL;
    for (auto b : bytes) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
uint64_t chunkHash(std::span<const uint8_t> prefix, std::span<const uint8_t> raw) {
    uint64_t h = hashBytes(prefix);
    for (auto b : raw) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
bool validExtent(const ChunkKey& key, int64_t end) {
    const int64_t span = chunkSpanMs(key.source, key.levelMs);
    return span && !key.symbol.empty() &&
           key.startMs >= recording::kHmc2MinMs && key.startMs % span == 0 &&
           key.startMs <= recording::kHmc2EndMs - span &&
           end == key.startMs + span;
}
std::string_view hmc2Layer(const std::string& source) {
    const auto* s = findChunkSource(source);
    if (!s) fail();
    return s->hmc2Layer;
}
bool knownError(uint16_t code) { return code >= 1 && code <= 4; }
struct HeaderGrid {
    GridIdentity grid;
    recording::SizeScale scale;
};
HeaderGrid commonGrid(const SparseColumns& data) {
    // A chunk may cross a config generation. Zero in the header means that
    // the authoritative identities/scales are carried per native constituent.
    HeaderGrid result{{0, 0, 0}, {0, 0}};
    bool first = true;
    for (const auto& col : data.columns) for (const auto& n : col.native) {
        if (first) { result = {n.grid, n.sizeScale}; first = false; }
        else if (n.grid.configHash != result.grid.configHash ||
                 n.grid.rowTickUnits != result.grid.rowTickUnits ||
                 n.grid.priceScale != result.grid.priceScale ||
                 n.sizeScale.floor != result.scale.floor ||
                 n.sizeScale.codesPerOctave != result.scale.codesPerOctave) return {{0, 0, 0}, {0, 0}};
    }
    return result;
}
void putGrid(Writer& w, const GridIdentity& g, const recording::SizeScale& s) {
    w.u(g.configHash); w.i(g.rowTickUnits); w.d(g.priceScale);
    w.d(s.floor); w.d(s.codesPerOctave);
}
HeaderGrid getGrid(Reader& r) {
    HeaderGrid g;
    g.grid.configHash = r.u<uint64_t>(); g.grid.rowTickUnits = r.i();
    g.grid.priceScale = r.d(); g.scale.floor = r.d(); g.scale.codesPerOctave = r.d();
    return g;
}
struct Counts { uint32_t columns = 0, entries = 0; };
Counts count(const SparseColumns& data) {
    if (data.columns.size() > kMaxColumns) fail();
    uint64_t entries = 0;
    for (const auto& col : data.columns) for (const auto& n : col.native) {
        entries += n.entries.size();
        if (entries > kMaxEntries) fail();
    }
    return {uint32_t(data.columns.size()), uint32_t(entries)};
}
bool validState(const ChunkFrame& frame) {
    if (frame.state.sealed)
        return frame.state.revision == 0 && frame.state.committedThroughMs >= frame.columns.endMs &&
               frame.columns.scannedRanges.size() == 1 &&
               frame.columns.scannedRanges.front() ==
                   SparseColumns::TimeRange{frame.columns.startMs, frame.columns.endMs};
    return frame.columns.scannedRanges.empty() ||
           frame.columns.scannedRanges.back().endMs <= frame.state.committedThroughMs;
}
void putMagic(Writer& w, ChunkKind kind) {
    for (char c : "SHC1") { if (c) w.u(uint8_t(c)); }
    w.u(kChunkWireVersion); w.u(uint8_t(kind));
}
// NotModified: u8 sealed, key, i64 committedThrough, u64 revision, u64 content hash.
// Error: key as sent (not validated), u16 code, message. Both are exact-length.
std::vector<uint8_t> encodeControl(const ChunkFrame& frame) {
    Writer w;
    putMagic(w, frame.kind);
    if (frame.kind == ChunkKind::NotModified) {
        const int64_t span = chunkSpanMs(frame.key.source, frame.key.levelMs);
        if (!validExtent(frame.key, frame.key.startMs + span) ||
            (frame.state.sealed && frame.state.revision != 0)) fail();
        w.u(uint8_t(frame.state.sealed));
        w.str(frame.key.symbol); w.str(frame.key.source);
        w.i(frame.key.levelMs); w.i(frame.key.startMs);
        w.i(frame.state.committedThroughMs); w.u(frame.state.revision); w.u(frame.contentHash);
    } else {
        if (!knownError(uint16_t(frame.error))) fail();
        w.str(frame.key.symbol); w.str(frame.key.source);
        w.i(frame.key.levelMs); w.i(frame.key.startMs);
        w.u(uint16_t(frame.error)); w.str(frame.message);
    }
    return std::move(w.data);
}
ChunkFrame decodeControl(Reader& r, ChunkFrame& out) {
    if (out.kind == ChunkKind::NotModified) {
        const auto sealed = r.u<uint8_t>(); if (sealed > 1) fail();
        out.state.sealed = sealed;
        out.key.symbol = r.str(); out.key.source = r.str();
        out.key.levelMs = r.i(); out.key.startMs = r.i();
        out.state.committedThroughMs = r.i(); out.state.revision = r.u<uint64_t>();
        out.contentHash = r.u<uint64_t>();
        const int64_t span = chunkSpanMs(out.key.source, out.key.levelMs);
        if (!validExtent(out.key, out.key.startMs + span) || (sealed && out.state.revision != 0)) fail();
    } else {
        out.key.symbol = r.str(); out.key.source = r.str();
        out.key.levelMs = r.i(); out.key.startMs = r.i();
        const auto code = r.u<uint16_t>();
        if (!knownError(code)) fail();
        out.error = ChunkError(code);
        out.message = r.str();
    }
    if (r.pos != r.data.size()) fail();
    return std::move(out);
}
} // namespace

const ChunkSource* findChunkSource(std::string_view id) {
    for (const auto& s : kChunkSources) if (s.id == id) return &s;
    return nullptr;
}
const ChunkSource* chunkSourceForHmc2Layer(std::string_view layer) {
    for (const auto& s : kChunkSources) if (s.hmc2Layer == layer) return &s;
    return nullptr;
}
int64_t chunkSpanMs(std::string_view source, int64_t levelMs) {
    const auto* s = findChunkSource(source);
    if (!s) return 0;
    if (levelMs == kMinuteMs) return kHourMs;
    return levelMs == kHourMs && s->hourLevel ? kDayMs : 0;
}
const char* chunkErrorName(ChunkError code) {
    switch (code) {
        case ChunkError::InvalidRequest: return "invalid_request";
        case ChunkError::Unavailable: return "unavailable";
        case ChunkError::Busy: return "busy";
        case ChunkError::BuildFailed: return "build_failed";
    }
    return "unknown";
}

std::vector<uint8_t> encodeChunk(const ChunkFrame& frame) {
    if (frame.kind == ChunkKind::NotModified || frame.kind == ChunkKind::Error) return encodeControl(frame);
    if (frame.kind != ChunkKind::Chunk || frame.key.symbol != frame.columns.symbol ||
        !findChunkSource(frame.key.source) || hmc2Layer(frame.key.source) != frame.columns.layer ||
        frame.key.levelMs != frame.columns.tfMs ||
        frame.key.startMs != frame.columns.startMs ||
        !validExtent(frame.key, frame.columns.endMs) || !validState(frame)) fail();
    validate(frame.columns);
    const auto counts = count(frame.columns);
    Writer raw;
    if (frame.columns.scannedRanges.size() > kMaxColumns) fail();
    raw.u(uint16_t(frame.columns.scannedRanges.size()));
    for (const auto& range : frame.columns.scannedRanges) { raw.i(range.startMs); raw.i(range.endMs); }
    // Metadata first, then packed entries in matching column/native order.
    for (const auto& col : frame.columns.columns) {
        if (col.native.size() > 1024) fail();
        raw.i(col.bucketStartMs); raw.u(col.observedMs); raw.u(col.flags);
        raw.u(uint16_t(col.native.size()));
        for (const auto& n : col.native) {
            if (n.composed) fail(); // v1 transports native levels only.
            putGrid(raw, n.grid, n.sizeScale);
            raw.i(n.baseRow); raw.u(n.observedMs);
            raw.u(uint8_t(0)); raw.u(uint8_t(!n.entryCoveredMs.empty()));
            for (const auto& side : n.coverage) {
                if (side.size() > 65535) fail();
                raw.u(uint16_t(side.size()));
                for (const auto& run : side) {
                    raw.i(run.lo); raw.i(run.hi); raw.u(run.coveredMs);
                }
            }
            raw.u(uint32_t(n.entries.size()));
        }
    }
    for (const auto& col : frame.columns.columns) for (const auto& n : col.native) {
        uint32_t previousRow = 0;
        for (size_t j = 0; j < n.entries.size(); ++j) {
            const auto& e = n.entries[j];
            const uint64_t packed = (uint64_t(e.row() - previousRow) << 1) | e.isAsk();
            recording::putVarint(raw.data, packed);
            raw.u(e.code);
            if (!n.entryCoveredMs.empty()) recording::putVarint(raw.data, n.entryCoveredMs[j]);
            previousRow = e.row();
        }
    }
    if (raw.data.size() > kMaxPayload) fail();
    std::vector<uint8_t> compressed(ZSTD_compressBound(raw.data.size()));
    const auto zsize = ZSTD_compress(compressed.data(), compressed.size(), raw.data.data(), raw.data.size(), 3);
    if (ZSTD_isError(zsize) || zsize > kMaxPayload) fail();
    compressed.resize(zsize);
    Writer wire;
    for (char c : "SHC1") { if (c) wire.u(uint8_t(c)); }
    wire.u(kChunkWireVersion); wire.u(uint8_t(frame.kind)); wire.u(uint8_t(frame.state.sealed));
    wire.str(frame.key.symbol); wire.str(frame.key.source);
    wire.i(frame.key.levelMs); wire.i(frame.key.startMs); wire.i(frame.columns.endMs);
    const auto common = commonGrid(frame.columns);
    putGrid(wire, common.grid, common.scale);
    wire.i(frame.state.committedThroughMs); wire.u(frame.state.revision);
    wire.u(chunkHash(wire.data, raw.data)); wire.u(counts.columns); wire.u(counts.entries);
    wire.u(uint32_t(raw.data.size())); wire.u(uint32_t(compressed.size()));
    if (wire.data.size() + compressed.size() + 14 > kMaxPayload) fail();
    wire.data.insert(wire.data.end(), compressed.begin(), compressed.end());
    return std::move(wire.data);
}

ChunkFrame decodeChunk(std::span<const uint8_t> wire) {
    Reader h{wire};
    for (char c : "SHC1") { if (c && h.u<uint8_t>() != uint8_t(c)) fail(); }
    if (h.u<uint16_t>() != kChunkWireVersion) fail();
    ChunkFrame out;
    out.kind = ChunkKind(h.u<uint8_t>());
    if (out.kind == ChunkKind::NotModified || out.kind == ChunkKind::Error) return decodeControl(h, out);
    if (out.kind != ChunkKind::Chunk) fail();
    const auto sealed = h.u<uint8_t>(); if (sealed > 1) fail();
    out.state.sealed = sealed;
    out.key.symbol = h.str();
    out.key.source = h.str();
    out.key.levelMs = h.i(); out.key.startMs = h.i(); const auto end = h.i();
    const auto headerGrid = getGrid(h);
    out.state.committedThroughMs = h.i(); out.state.revision = h.u<uint64_t>();
    const auto hashOffset = h.pos;
    out.contentHash = h.u<uint64_t>();
    const auto nColumns = h.u<uint32_t>(), nEntries = h.u<uint32_t>();
    const auto rawLen = h.u<uint32_t>(), zLen = h.u<uint32_t>();
    if (wire.size() + 14 > kMaxPayload || nColumns > kMaxColumns || nEntries > kMaxEntries ||
        rawLen < 2 || rawLen > kMaxPayload ||
        zLen > kMaxPayload || zLen != wire.size() - h.pos ||
        !validExtent(out.key, end)) fail();
    if (ZSTD_getFrameContentSize(wire.data() + h.pos, zLen) != rawLen) fail();
    std::vector<uint8_t> raw(rawLen);
    const auto decoded = ZSTD_decompress(raw.data(), raw.size(), wire.data() + h.pos, zLen);
    if (ZSTD_isError(decoded) || decoded != rawLen ||
        chunkHash(wire.first(hashOffset), raw) != out.contentHash) fail();
    Reader r{raw};
    out.columns = {out.key.symbol, std::string(hmc2Layer(out.key.source)), out.key.levelMs, out.key.startMs, end};
    const auto nRanges = r.u<uint16_t>();
    if (nRanges > kMaxColumns) fail();
    r.requireCount(nRanges, 16);
    out.columns.scannedRanges.reserve(nRanges);
    for (unsigned j = 0; j < nRanges; ++j) out.columns.scannedRanges.push_back({r.i(), r.i()});
    r.requireCount(nColumns, kColumnRecordMinBytes);
    out.columns.columns.reserve(nColumns);
    uint64_t totalEntries = 0;
    std::vector<std::vector<uint32_t>> entryCounts;
    for (uint32_t i = 0; i < nColumns; ++i) {
        SparseColumn col;
        col.bucketStartMs = r.i(); col.observedMs = r.u<uint64_t>(); col.flags = r.u<uint32_t>();
        const auto nativeCount = r.u<uint16_t>(); if (nativeCount > 1024) fail();
        r.requireCount(nativeCount, kNativeRecordMinBytes);
        col.native.reserve(nativeCount);
        std::vector<uint32_t> perNative;
        for (unsigned j = 0; j < nativeCount; ++j) {
            NativeColumn n;
            const auto g = getGrid(r); n.grid = g.grid; n.sizeScale = g.scale;
            n.baseRow = r.i(); n.observedMs = r.u<uint64_t>();
            const auto composed = r.u<uint8_t>(), sidecar = r.u<uint8_t>();
            if (composed || sidecar > 1) fail();
            for (auto& side : n.coverage) {
                const auto count = r.u<uint16_t>();
                r.requireCount(count, 24);
                side.reserve(count);
                for (unsigned k = 0; k < count; ++k) side.push_back({r.i(), r.i(), r.u<uint64_t>()});
            }
            const auto count = r.u<uint32_t>(); totalEntries += count;
            if (totalEntries > nEntries) fail();
            r.requireCount(totalEntries, 3); // running total: entries are read after all headers
            n.entries.reserve(count);
            if (sidecar) n.entryCoveredMs.reserve(count);
            perNative.push_back((count << 1) | sidecar);
            col.native.push_back(std::move(n));
        }
        entryCounts.push_back(std::move(perNative));
        out.columns.columns.push_back(std::move(col));
    }
    if (totalEntries != nEntries) fail();
    for (size_t i = 0; i < out.columns.columns.size(); ++i)
        for (size_t j = 0; j < out.columns.columns[i].native.size(); ++j) {
            auto& n = out.columns.columns[i].native[j];
            const auto count = entryCounts[i][j] >> 1;
            const bool sidecar = entryCounts[i][j] & 1;
            uint32_t row = 0;
            for (uint32_t k = 0; k < count; ++k) {
                const auto packed = r.var();
                if ((packed >> 1) > 0x7fffffffu - row) fail();
                row += uint32_t(packed >> 1);
                n.entries.push_back({row | (uint32_t(packed & 1) << 31), r.u<uint16_t>()});
                if (sidecar) n.entryCoveredMs.push_back(r.var());
            }
        }
    if (r.pos != raw.size()) fail();
    validate(out.columns);
    if (!validState(out)) fail();
    const auto common = commonGrid(out.columns);
    if (common.grid.configHash != headerGrid.grid.configHash ||
        common.grid.rowTickUnits != headerGrid.grid.rowTickUnits ||
        common.grid.priceScale != headerGrid.grid.priceScale ||
        common.scale.floor != headerGrid.scale.floor ||
        common.scale.codesPerOctave != headerGrid.scale.codesPerOctave) fail();
    return out;
}
uint64_t chunkContentHash(std::span<const uint8_t> wire) {
    Reader h{wire};
    for (char c : "SHC1") { if (c && h.u<uint8_t>() != uint8_t(c)) fail(); }
    if (h.u<uint16_t>() != kChunkWireVersion || ChunkKind(h.u<uint8_t>()) != ChunkKind::Chunk) fail();
    h.u<uint8_t>(); h.str(); h.str();          // sealed, symbol, source
    constexpr size_t kSkip = 24 + 40 + 8 + 8;  // level/start/end, grid, through, revision
    if (wire.size() - h.pos < kSkip) fail();
    h.pos += kSkip;
    return h.u<uint64_t>();
}
std::vector<uint8_t> encodeChunkEnvelope(uint64_t requestId, std::span<const uint8_t> chunkWire) {
    if (chunkWire.size() > kMaxPayload - 14 || chunkWire.size() < 6) fail();
    Writer out;
    for (char c : "SHE1") { if (c) out.u(uint8_t(c)); }
    out.u(uint16_t(1)); out.u(requestId);
    out.data.insert(out.data.end(), chunkWire.begin(), chunkWire.end());
    return std::move(out.data);
}
ChunkEnvelope decodeChunkEnvelope(std::span<const uint8_t> wire) {
    Reader r{wire};
    for (char c : "SHE1") { if (c && r.u<uint8_t>() != uint8_t(c)) fail(); }
    if (r.u<uint16_t>() != 1) fail();
    ChunkEnvelope out;
    out.requestId = r.u<uint64_t>();
    out.chunk = decodeChunk(wire.subspan(r.pos));
    return out;
}
} // namespace heatmap
