#include "ChunkCodec.hpp"
#include "../servermodel/RecordingCodec.hpp"
#include <zstd.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace heatmap {
namespace {
constexpr size_t kMaxPayload = 256u * 1024u * 1024u;
constexpr uint32_t kMaxColumns = 1440, kMaxEntries = 8'000'000;
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
void putNumerator(Writer& w, long double n) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(std::numeric_limits<long double>::max_digits10) << n;
    w.str(os.str());
}
long double getNumerator(Reader& r) {
    const auto s = r.str();
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    long double n = 0;
    is >> n;
    if (!is || is.peek() != std::char_traits<char>::eof() || !std::isfinite(n) || n < 0) fail();
    return n;
}
uint8_t layerCode(const std::string& layer) {
    if (layer == "near") return 1;
    if (layer == "deep") return 2;
    fail(); return 0;
}
bool validExtent(const ChunkKey& key, int64_t end) {
    const int64_t span = key.levelMs == kMinuteMs ? kHourMs :
                         key.levelMs == kHourMs && key.layer == "deep" ? kDayMs : 0;
    return span && !key.symbol.empty() && (key.layer == "near" || key.layer == "deep") &&
           key.startMs >= 0 && key.startMs % span == 0 &&
           key.startMs <= std::numeric_limits<int64_t>::max() - span &&
           end == key.startMs + span;
}
std::string layerName(uint8_t code) {
    if (code == 1) return "near";
    if (code == 2) return "deep";
    fail(); return {};
}
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
} // namespace

std::vector<uint8_t> encodeChunk(const ChunkFrame& frame) {
    if (frame.kind != ChunkKind::Chunk || frame.key.symbol != frame.columns.symbol ||
        frame.key.layer != frame.columns.layer || frame.key.levelMs != frame.columns.tfMs ||
        frame.key.startMs != frame.columns.startMs ||
        !validExtent(frame.key, frame.columns.endMs) ||
        (frame.state.sealed && frame.state.revision)) fail();
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
            putGrid(raw, n.grid, n.sizeScale);
            raw.i(n.baseRow); raw.u(n.observedMs);
            raw.u(uint8_t(n.composed)); raw.u(uint8_t(!n.entryCoveredMs.empty()));
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
            if (n.composed) putNumerator(raw, n.numerators[j]);
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
    wire.u(frame.requestId); wire.str(frame.key.symbol); wire.u(layerCode(frame.key.layer));
    wire.i(frame.key.levelMs); wire.i(frame.key.startMs); wire.i(frame.columns.endMs);
    const auto common = commonGrid(frame.columns);
    putGrid(wire, common.grid, common.scale);
    wire.i(frame.state.committedThroughMs); wire.u(frame.state.revision);
    wire.u(hashBytes(raw.data)); wire.u(counts.columns); wire.u(counts.entries);
    wire.u(uint32_t(raw.data.size())); wire.u(uint32_t(compressed.size()));
    wire.data.insert(wire.data.end(), compressed.begin(), compressed.end());
    return std::move(wire.data);
}

ChunkFrame decodeChunk(std::span<const uint8_t> wire) {
    Reader h{wire};
    for (char c : "SHC1") { if (c && h.u<uint8_t>() != uint8_t(c)) fail(); }
    if (h.u<uint16_t>() != kChunkWireVersion) fail();
    ChunkFrame out;
    out.kind = ChunkKind(h.u<uint8_t>());
    if (out.kind != ChunkKind::Chunk) fail();
    const auto sealed = h.u<uint8_t>(); if (sealed > 1) fail();
    out.state.sealed = sealed;
    out.requestId = h.u<uint64_t>(); out.key.symbol = h.str();
    out.key.layer = layerName(h.u<uint8_t>());
    out.key.levelMs = h.i(); out.key.startMs = h.i(); const auto end = h.i();
    const auto headerGrid = getGrid(h);
    out.state.committedThroughMs = h.i(); out.state.revision = h.u<uint64_t>();
    out.contentHash = h.u<uint64_t>();
    const auto nColumns = h.u<uint32_t>(), nEntries = h.u<uint32_t>();
    const auto rawLen = h.u<uint32_t>(), zLen = h.u<uint32_t>();
    if (nColumns > kMaxColumns || nEntries > kMaxEntries || rawLen > kMaxPayload ||
        zLen > kMaxPayload || zLen != wire.size() - h.pos ||
        (out.state.sealed && out.state.revision) ||
        !validExtent(out.key, end)) fail();
    std::vector<uint8_t> raw(rawLen);
    const auto decoded = ZSTD_decompress(raw.data(), raw.size(), wire.data() + h.pos, zLen);
    if (ZSTD_isError(decoded) || decoded != rawLen || hashBytes(raw) != out.contentHash) fail();
    Reader r{raw};
    out.columns = {out.key.symbol, out.key.layer, out.key.levelMs, out.key.startMs, end};
    const auto nRanges = r.u<uint16_t>();
    if (nRanges > kMaxColumns) fail();
    out.columns.scannedRanges.reserve(nRanges);
    for (unsigned j = 0; j < nRanges; ++j) out.columns.scannedRanges.push_back({r.i(), r.i()});
    out.columns.columns.reserve(nColumns);
    uint64_t totalEntries = 0;
    std::vector<std::vector<uint32_t>> entryCounts;
    for (uint32_t i = 0; i < nColumns; ++i) {
        SparseColumn col;
        col.bucketStartMs = r.i(); col.observedMs = r.u<uint64_t>(); col.flags = r.u<uint32_t>();
        const auto nativeCount = r.u<uint16_t>(); if (nativeCount > 1024) fail();
        col.native.reserve(nativeCount);
        std::vector<uint32_t> perNative;
        for (unsigned j = 0; j < nativeCount; ++j) {
            NativeColumn n;
            const auto g = getGrid(r); n.grid = g.grid; n.sizeScale = g.scale;
            n.baseRow = r.i(); n.observedMs = r.u<uint64_t>();
            const auto composed = r.u<uint8_t>(), sidecar = r.u<uint8_t>();
            if (composed > 1 || sidecar > 1) fail();
            n.composed = composed;
            for (auto& side : n.coverage) {
                const auto count = r.u<uint16_t>();
                side.reserve(count);
                for (unsigned k = 0; k < count; ++k) side.push_back({r.i(), r.i(), r.u<uint64_t>()});
            }
            const auto count = r.u<uint32_t>(); totalEntries += count;
            if (totalEntries > nEntries) fail();
            n.entries.reserve(count);
            if (sidecar) n.entryCoveredMs.reserve(count);
            if (n.composed) n.numerators.reserve(count);
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
                if (n.composed) n.numerators.push_back(getNumerator(r));
            }
        }
    if (r.pos != raw.size()) fail();
    validate(out.columns);
    const auto common = commonGrid(out.columns);
    if (common.grid.configHash != headerGrid.grid.configHash ||
        common.grid.rowTickUnits != headerGrid.grid.rowTickUnits ||
        common.grid.priceScale != headerGrid.grid.priceScale ||
        common.scale.floor != headerGrid.scale.floor ||
        common.scale.codesPerOctave != headerGrid.scale.codesPerOctave) fail();
    return out;
}
} // namespace heatmap
