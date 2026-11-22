#pragma once
// Measurement prototype of the lean flow chunk in docs/FLOW_CHUNKS.md (draft
// spec, not implemented). Used only by flow_history_bench --columnar and its
// unit test. The production codec replaces this file (step 3 of the data
// foundation plan); it is not a wire or disk format.
//
// One chunk = one header, then per-field arrays: the summary (one entry per
// present bar, in slot order), the row offsets (int16, from the bar's low row),
// and the row records (24 bytes). Notables are not part of the chunk. The low
// row is not stored: it is floorDiv(low, rowTickUnits). Delta and maxRowVol are
// derived on read.
//
// Three encodings of the same chunk:
//   Fixed  - the in-memory arrays written at fixed width (no varints).
//   Varint - every value LEB128 (signed values zigzag), no deltas.
//   Delta  - varints with deltas: slot gaps, open from the previous close,
//            high/low/close from open/low, row offset gaps.
// All three write field-major (every value of one field, then the next).
#include "flow/FlowBar.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace sentinel::flow::proto {

struct RowRecord {
    int64_t bidVol = 0, askVol = 0; // base units
    uint32_t bidCount = 0, askCount = 0;
    bool operator==(const RowRecord&) const = default;
};
static_assert(sizeof(RowRecord) == 24);
static_assert(alignof(RowRecord) == 8);

struct Grid {
    int64_t rowTickUnits = 0, priceScale = 0, baseScale = 0, quoteScale = 0;
    bool operator==(const Grid&) const = default;
};
static_assert(sizeof(Grid) == 32);

struct FlowChunk {
    int64_t levelMs = 0, startMs = 0;
    uint32_t slotCount = 0;             // bars the span holds (60, 24 or 16)
    std::vector<Grid> grids;        // 1 entry except a 1d chunk crossing a grid change
    // Summary: one entry per present bar, ascending slot.
    std::vector<uint16_t> slot;
    std::vector<uint8_t> grid;      // index into grids
    std::vector<int64_t> open, high, low, close; // price units (price * priceScale)
    std::vector<int64_t> baseVolume, quoteVolume, buyVol, sellVol;
    std::vector<uint32_t> count;
    std::vector<uint32_t> rowBegin; // bars + 1 entries; rows of bar i are [rowBegin[i], rowBegin[i+1])
    // Rows, parallel arrays.
    std::vector<int16_t> offset;    // row - lowRow of its bar, ascending within a bar
    std::vector<RowRecord> rows;
    size_t bars() const { return slot.size(); }
    bool operator==(const FlowChunk&) const = default;
};

enum class Encoding { Fixed, Varint, Delta };

struct Stats { uint64_t lowRowMismatch = 0, nativeTickMismatch = 0; };

inline int64_t toUnits(double price, int64_t scale) {
    const auto units = recording::priceUnits(price, double(scale));
    if (!units) throw std::invalid_argument("flow proto price not representable");
    return *units;
}

// Bars must share levelMs == bar.timeframeMs and lie in [startMs, startMs + slotCount * levelMs).
inline FlowChunk fromBars(std::span<const FlowBar> bars, int64_t levelMs, int64_t startMs, uint32_t slotCount,
                          Stats* stats = nullptr) {
    FlowChunk c;
    c.levelMs = levelMs; c.startMs = startMs; c.slotCount = slotCount;
    c.rowBegin.push_back(0);
    int64_t previous = -1;
    for (const auto& b : bars) {
        if (b.timeframeMs != levelMs || b.startMs < startMs || (b.startMs - startMs) % levelMs)
            throw std::invalid_argument("flow proto bar outside chunk");
        const auto s = (b.startMs - startMs) / levelMs;
        if (s >= int64_t(slotCount) || s <= previous || s > UINT16_MAX) throw std::invalid_argument("flow proto slot");
        previous = s;
        const Grid g{std::llround(b.nativeTick * double(b.priceScale)), b.priceScale, b.baseScale, b.quoteScale};
        if (stats && double(g.rowTickUnits) / double(g.priceScale) != b.nativeTick) ++stats->nativeTickMismatch;
        size_t gi = 0;
        while (gi < c.grids.size() && !(c.grids[gi] == g)) ++gi;
        if (gi == c.grids.size()) c.grids.push_back(g);
        if (gi > UINT8_MAX) throw std::invalid_argument("flow proto grid table");
        c.slot.push_back(uint16_t(s));
        c.grid.push_back(uint8_t(gi));
        c.open.push_back(toUnits(b.open, b.priceScale)); c.high.push_back(toUnits(b.high, b.priceScale));
        c.low.push_back(toUnits(b.low, b.priceScale)); c.close.push_back(toUnits(b.close, b.priceScale));
        if (stats && recording::floorDiv(c.low.back(), g.rowTickUnits) != b.lowRow) ++stats->lowRowMismatch;
        c.baseVolume.push_back(b.baseVolume); c.quoteVolume.push_back(b.quoteVolume);
        c.buyVol.push_back(b.buyVol); c.sellVol.push_back(b.sellVol);
        if (b.count > UINT32_MAX) throw std::invalid_argument("flow proto count");
        c.count.push_back(uint32_t(b.count));
        for (const auto& r : b.rows) {
            c.offset.push_back(r.offset);
            c.rows.push_back({r.bidVol, r.askVol, r.bidCount, r.askCount});
        }
        c.rowBegin.push_back(uint32_t(c.rows.size()));
    }
    return c;
}

// Inverse of fromBars, without notables. lowRow, delta and maxRowVol are derived.
inline std::vector<FlowBar> toBars(const FlowChunk& c) {
    std::vector<FlowBar> out;
    out.reserve(c.bars());
    for (size_t i = 0; i < c.bars(); ++i) {
        const auto& g = c.grids.at(c.grid[i]);
        FlowBar b;
        b.startMs = c.startMs + int64_t(c.slot[i]) * c.levelMs;
        b.timeframeMs = c.levelMs;
        b.nativeTick = double(g.rowTickUnits) / double(g.priceScale);
        b.priceScale = g.priceScale; b.baseScale = g.baseScale; b.quoteScale = g.quoteScale;
        const double scale = double(g.priceScale);
        b.open = double(c.open[i]) / scale; b.high = double(c.high[i]) / scale;
        b.low = double(c.low[i]) / scale; b.close = double(c.close[i]) / scale;
        b.lowRow = recording::floorDiv(c.low[i], g.rowTickUnits);
        b.baseVolume = c.baseVolume[i]; b.quoteVolume = c.quoteVolume[i];
        b.buyVol = c.buyVol[i]; b.sellVol = c.sellVol[i]; b.delta = b.buyVol - b.sellVol;
        b.count = c.count[i];
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            const auto& rec = c.rows[r];
            b.rows.push_back({c.offset[r], rec.bidVol, rec.askVol, rec.bidCount, rec.askCount});
            b.maxRowVol = std::max(b.maxRowVol, rec.bidVol + rec.askVol);
        }
        out.push_back(std::move(b));
    }
    return out;
}

namespace detail {
struct Out {
    std::vector<uint8_t> b;
    void fixed(uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void var(uint64_t v) {
        while (v >= 0x80) { b.push_back(uint8_t(v) | 0x80); v >>= 7; }
        b.push_back(uint8_t(v));
    }
    void svar(int64_t v) { var(recording::zigzag(v)); }
};
struct In {
    std::span<const uint8_t> b;
    size_t p = 0;
    uint64_t fixed(size_t n) {
        if (n > b.size() - p) throw std::runtime_error("flow proto truncated");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(b[p++]) << (8 * i);
        return v;
    }
    uint64_t var() {
        uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const auto byte = fixed(1);
            v |= (byte & 0x7f) << shift;
            if (!(byte & 0x80)) return v;
        }
        throw std::runtime_error("flow proto varint overflow");
    }
    int64_t svar() { const auto u = var(); return int64_t(u >> 1) ^ -int64_t(u & 1); }
    // Element counts are bounded by the remaining bytes before allocation.
    size_t count(size_t limit) {
        const auto n = var();
        if (n > limit || n > b.size() - p + 1) throw std::runtime_error("flow proto count");
        return size_t(n);
    }
};
} // namespace detail

constexpr uint32_t MaxRowsPerChunk = 1u << 24;

// Header: magic FLP1, u8 encoding, i64 level, i64 start, u32 slotCount, u8 grids,
// grids (4 x i64). Then the summary section (bar count, slots, grid indexes,
// OHLC, four volumes, trade counts), then the rows section (row count per bar,
// offsets, four row fields). summaryOnly stops after the summary (flow.summary).
inline std::vector<uint8_t> encode(const FlowChunk& c, Encoding e, bool summaryOnly = false,
                                   bool withHeader = true) {
    detail::Out o;
    if (withHeader) {
        for (char ch : {'F', 'L', 'P', '1'}) o.b.push_back(uint8_t(ch));
        o.fixed(uint8_t(e), 1); o.fixed(uint64_t(c.levelMs), 8); o.fixed(uint64_t(c.startMs), 8); o.fixed(c.slotCount, 4);
        o.fixed(c.grids.size(), 1);
        for (const auto& g : c.grids)
            for (auto v : {g.rowTickUnits, g.priceScale, g.baseScale, g.quoteScale}) o.fixed(uint64_t(v), 8);
    }
    const bool fixed = e == Encoding::Fixed, delta = e == Encoding::Delta;
    const auto u = [&](uint64_t v, size_t n) { fixed ? o.fixed(v, n) : o.var(v); };
    const auto s = [&](int64_t v, size_t n) { fixed ? o.fixed(uint64_t(v), n) : o.svar(v); };
    const size_t n = c.bars();
    fixed ? o.fixed(n, 2) : o.var(n);
    int64_t prev = -1;
    for (size_t i = 0; i < n; ++i) { u(delta ? uint64_t(c.slot[i] - prev - 1) : c.slot[i], 2); prev = c.slot[i]; }
    if (c.grids.size() > 1) for (size_t i = 0; i < n; ++i) o.fixed(c.grid[i], 1);
    if (delta) {
        int64_t prevClose = 0;
        for (size_t i = 0; i < n; ++i) { o.svar(c.open[i] - prevClose); prevClose = c.close[i]; }
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.high[i] - c.open[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.open[i] - c.low[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.close[i] - c.low[i]));
    } else {
        for (const auto* f : {&c.open, &c.high, &c.low, &c.close}) for (auto v : *f) s(v, 8);
    }
    for (const auto* f : {&c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol}) for (auto v : *f) s(v, 8);
    for (auto v : c.count) u(v, 4);
    if (summaryOnly) return std::move(o.b);
    for (size_t i = 0; i < n; ++i) u(c.rowBegin[i + 1] - c.rowBegin[i], 2);
    for (size_t i = 0; i < n; ++i) {
        int prevOffset = -1;
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            if (delta) o.var(uint64_t(c.offset[r] - prevOffset - 1));
            else s(c.offset[r], 2);
            prevOffset = c.offset[r];
        }
    }
    for (const auto& r : c.rows) s(r.bidVol, 8);
    for (const auto& r : c.rows) s(r.askVol, 8);
    for (const auto& r : c.rows) u(r.bidCount, 4);
    for (const auto& r : c.rows) u(r.askCount, 4);
    return std::move(o.b);
}

inline FlowChunk decode(std::span<const uint8_t> bytes) {
    detail::In in{bytes};
    for (char ch : {'F', 'L', 'P', '1'}) if (in.fixed(1) != uint8_t(ch)) throw std::runtime_error("flow proto magic");
    const auto e = Encoding(in.fixed(1));
    if (e != Encoding::Fixed && e != Encoding::Varint && e != Encoding::Delta) throw std::runtime_error("flow proto encoding");
    FlowChunk c;
    c.levelMs = int64_t(in.fixed(8)); c.startMs = int64_t(in.fixed(8)); c.slotCount = uint32_t(in.fixed(4));
    if (c.levelMs <= 0 || c.slotCount == 0 || c.slotCount > UINT16_MAX + 1u) throw std::runtime_error("flow proto header");
    const auto gridCount = in.fixed(1);
    for (size_t i = 0; i < gridCount; ++i) {
        Grid g{int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8))};
        if (g.rowTickUnits <= 0 || g.priceScale <= 0 || g.baseScale <= 0 || g.quoteScale <= 0)
            throw std::runtime_error("flow proto grid");
        c.grids.push_back(g);
    }
    const bool fixed = e == Encoding::Fixed, delta = e == Encoding::Delta;
    const auto u = [&](size_t width) { return fixed ? in.fixed(width) : in.var(); };
    const auto s = [&](size_t width) -> int64_t {
        if (!fixed) return in.svar();
        const auto v = in.fixed(width);
        // Sign-extend narrow fixed fields.
        const unsigned shift = unsigned(64 - 8 * width);
        return shift ? int64_t(v << shift) >> shift : int64_t(v);
    };
    const size_t n = fixed ? size_t(in.fixed(2)) : in.count(c.slotCount);
    if (n > c.slotCount) throw std::runtime_error("flow proto bar count");
    if (n && gridCount == 0) throw std::runtime_error("flow proto grid table");
    int64_t prev = -1;
    for (size_t i = 0; i < n; ++i) {
        const int64_t slot = delta ? prev + 1 + int64_t(in.var()) : int64_t(u(2));
        if (slot <= prev || slot >= int64_t(c.slotCount)) throw std::runtime_error("flow proto slot");
        c.slot.push_back(uint16_t(slot)); prev = slot;
    }
    c.grid.assign(n, 0);
    if (gridCount > 1)
        for (auto& g : c.grid) if ((g = uint8_t(in.fixed(1))) >= gridCount) throw std::runtime_error("flow proto grid index");
    for (auto* f : {&c.open, &c.high, &c.low, &c.close}) f->resize(n);
    if (delta) {
        // Read the four delta fields, then resolve the open -> previous close chain.
        // Unsigned arithmetic: hostile input decodes to garbage, never to UB.
        std::vector<uint64_t> openRel(n), up(n), down(n), fromLow(n);
        for (auto& v : openRel) v = uint64_t(in.svar());
        for (auto& v : up) v = in.var();
        for (auto& v : down) v = in.var();
        for (auto& v : fromLow) v = in.var();
        uint64_t prevClose = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t open = prevClose + openRel[i];
            c.open[i] = int64_t(open);
            c.high[i] = int64_t(open + up[i]);
            c.low[i] = int64_t(open - down[i]);
            c.close[i] = int64_t(open - down[i] + fromLow[i]);
            prevClose = uint64_t(c.close[i]);
        }
    } else {
        for (auto* f : {&c.open, &c.high, &c.low, &c.close}) for (auto& v : *f) v = s(8);
    }
    for (auto* f : {&c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol}) { f->resize(n); for (auto& v : *f) v = s(8); }
    c.count.resize(n);
    for (auto& v : c.count) { const auto x = u(4); if (x > UINT32_MAX) throw std::runtime_error("flow proto count"); v = uint32_t(x); }
    c.rowBegin.assign(1, 0);
    for (size_t i = 0; i < n; ++i) {
        const auto rows = u(2);
        if (rows > 32768 || c.rowBegin.back() + rows > MaxRowsPerChunk) throw std::runtime_error("flow proto rows");
        c.rowBegin.push_back(c.rowBegin.back() + uint32_t(rows));
    }
    const size_t total = c.rowBegin.back();
    if (total > bytes.size() - in.p) throw std::runtime_error("flow proto rows truncated");
    c.offset.resize(total);
    for (size_t i = 0; i < n; ++i) {
        int64_t prevOffset = -1;
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            const int64_t v = delta ? prevOffset + 1 + int64_t(std::min<uint64_t>(in.var(), 1u << 16)) : s(2);
            if (v <= prevOffset || v > INT16_MAX) throw std::runtime_error("flow proto offset");
            c.offset[r] = int16_t(v); prevOffset = v;
        }
    }
    c.rows.resize(total);
    for (auto& r : c.rows) r.bidVol = s(8);
    for (auto& r : c.rows) r.askVol = s(8);
    for (auto& r : c.rows) { const auto x = u(4); if (x > UINT32_MAX) throw std::runtime_error("flow proto count"); r.bidCount = uint32_t(x); }
    for (auto& r : c.rows) { const auto x = u(4); if (x > UINT32_MAX) throw std::runtime_error("flow proto count"); r.askCount = uint32_t(x); }
    if (in.p != bytes.size()) throw std::runtime_error("flow proto trailing bytes");
    return c;
}
} // namespace sentinel::flow::proto
