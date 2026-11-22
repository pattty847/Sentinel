#pragma once
// Measurement prototype of docs/FLOW_CHUNKS.md (draft spec, not implemented).
// Used only by flow_history_bench --columnar and test_flow_chunk_proto; the
// production codec replaces this file (plan step 3).
//
// Encoding::Spec writes exactly the section 4 flow payload (grid table,
// coverage runs, summary section, rows section) and encodeDayFile/parseDayFile
// exactly the section 5 day file. Encoding::Varint (no deltas) and
// Encoding::Fixed (fixed-width arrays, the in-memory image) are measurement
// variants with the same field order; they are not part of the spec.
// trades.big is not prototyped.
#include "flow/FlowBar.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
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

// Served statuses are 1-3. Building exists only inside assembly: a chunk with a
// building slot is not sealed, and encode refuses it.
enum class Status : uint8_t { Complete = 1, Partial = 2, Unrecorded = 3, Building = 4 };
struct CoverageRun {
    uint16_t fromSlot = 0, toSlot = 0; // [from, to)
    Status status = Status::Complete;
    bool operator==(const CoverageRun&) const = default;
};

struct FlowChunk {
    int64_t levelMs = 0, startMs = 0;
    uint32_t slotCount = 0;         // from the key, not the payload
    std::vector<Grid> grids;
    std::vector<CoverageRun> coverage;
    // Summary: one entry per present bar, ascending slot.
    std::vector<uint16_t> slot;
    std::vector<uint8_t> grid;      // index into grids
    std::vector<int64_t> open, high, low, close; // price units
    std::vector<int64_t> baseVolume, quoteVolume, buyVol, sellVol;
    std::vector<uint32_t> count;
    std::vector<uint32_t> rowBegin; // bars + 1 entries
    // Rows, parallel arrays.
    std::vector<int16_t> offset;    // row - low row of its bar
    std::vector<RowRecord> rows;
    size_t bars() const { return slot.size(); }
    bool operator==(const FlowChunk&) const = default;
};

enum class Encoding { Spec, Varint, Fixed };
struct Stats { uint64_t lowRowMismatch = 0, nativeTickMismatch = 0; };

inline int64_t toUnits(double price, int64_t scale) {
    const auto units = recording::priceUnits(price, double(scale));
    if (!units) throw std::invalid_argument("flow proto price not representable");
    return *units;
}

// Coverage defaults to one complete run over the span.
inline FlowChunk fromBars(std::span<const FlowBar> bars, int64_t levelMs, int64_t startMs, uint32_t slotCount,
                          Stats* stats = nullptr) {
    FlowChunk c;
    c.levelMs = levelMs; c.startMs = startMs; c.slotCount = slotCount;
    if (slotCount == 0 || slotCount > UINT16_MAX) throw std::invalid_argument("flow proto slots");
    c.coverage.push_back({0, uint16_t(slotCount), Status::Complete});
    c.rowBegin.push_back(0);
    int64_t previous = -1;
    for (const auto& b : bars) {
        if (b.timeframeMs != levelMs || b.startMs < startMs || (b.startMs - startMs) % levelMs)
            throw std::invalid_argument("flow proto bar outside chunk");
        const auto s = (b.startMs - startMs) / levelMs;
        if (s >= int64_t(slotCount) || s <= previous) throw std::invalid_argument("flow proto slot");
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
    void var(uint64_t v) { // LEB128, unsigned
        while (v >= 0x80) { b.push_back(uint8_t(v) | 0x80); v >>= 7; }
        b.push_back(uint8_t(v));
    }
    void svar(int64_t v) { var(recording::zigzag(v)); }
};
struct In {
    std::span<const uint8_t> b;
    size_t p = 0;
    [[noreturn]] static void fail(const char* what) { throw std::runtime_error(std::string("flow proto ") + what); }
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

inline std::vector<uint8_t> encode(const FlowChunk& c, Encoding e, bool summaryOnly = false) {
    detail::Out o;
    const bool fixed = e == Encoding::Fixed, spec = e == Encoding::Spec;
    // Unsigned values are varints (Spec, Varint) or fixed width (Fixed).
    const auto u = [&](uint64_t v, size_t n) { fixed ? o.fixed(v, n) : o.var(v); };
    o.fixed(c.grids.size(), 1);
    for (const auto& g : c.grids)
        for (auto v : {g.rowTickUnits, g.priceScale, g.baseScale, g.quoteScale}) o.fixed(uint64_t(v), 8);
    o.fixed(c.coverage.size(), 1);
    for (const auto& r : c.coverage) {
        if (r.status == Status::Building) throw std::invalid_argument("flow proto unsealed coverage");
        o.fixed(r.fromSlot, 2); o.fixed(r.toSlot, 2); o.fixed(uint8_t(r.status), 1);
    }
    const size_t n = c.bars();
    u(n, 2);
    int64_t prev = -1;
    for (size_t i = 0; i < n; ++i) { u(spec ? uint64_t(c.slot[i] - prev - 1) : c.slot[i], 2); prev = c.slot[i]; }
    if (c.grids.size() > 1) for (size_t i = 0; i < n; ++i) o.fixed(c.grid[i], 1);
    if (spec) {
        int64_t prevClose = 0;
        for (size_t i = 0; i < n; ++i) { o.svar(c.open[i] - prevClose); prevClose = c.close[i]; }
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.high[i] - c.open[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.open[i] - c.low[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.close[i] - c.low[i]));
    } else {
        for (const auto* f : {&c.open, &c.high, &c.low, &c.close}) for (auto v : *f) u(uint64_t(v), 8);
    }
    for (const auto* f : {&c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol}) for (auto v : *f) u(uint64_t(v), 8);
    for (auto v : c.count) u(v, 4);
    if (summaryOnly) return std::move(o.b);
    for (size_t i = 0; i < n; ++i) u(c.rowBegin[i + 1] - c.rowBegin[i], 2);
    for (size_t i = 0; i < n; ++i) {
        int prevOffset = -1;
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            u(spec ? uint64_t(c.offset[r] - prevOffset - 1) : uint64_t(uint16_t(c.offset[r])), 2);
            prevOffset = c.offset[r];
        }
    }
    for (const auto& r : c.rows) u(uint64_t(r.bidVol), 8);
    for (const auto& r : c.rows) u(uint64_t(r.askVol), 8);
    for (const auto& r : c.rows) u(r.bidCount, 4);
    for (const auto& r : c.rows) u(r.askCount, 4);
    return std::move(o.b);
}

// Decodes and validates a payload (the section 4 decoder checks). summaryOnly
// accepts a flow.summary payload (rows left empty).
inline FlowChunk decode(std::span<const uint8_t> bytes, Encoding e, int64_t levelMs, int64_t startMs,
                        uint32_t slotCount, bool summaryOnly = false) {
    using detail::In;
    In in{bytes};
    FlowChunk c;
    c.levelMs = levelMs; c.startMs = startMs; c.slotCount = slotCount;
    if (levelMs <= 0 || slotCount == 0 || slotCount > UINT16_MAX) In::fail("key");
    const bool fixed = e == Encoding::Fixed, spec = e == Encoding::Spec;
    const auto u = [&](size_t width) { return fixed ? in.fixed(width) : in.var(); };
    const auto gridCount = in.fixed(1);
    for (size_t i = 0; i < gridCount; ++i) {
        Grid g{int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8))};
        if (g.rowTickUnits <= 0 || g.priceScale <= 0 || g.baseScale <= 0 || g.quoteScale <= 0) In::fail("grid");
        c.grids.push_back(g);
    }
    const auto runCount = in.fixed(1);
    uint32_t covered = 0;
    for (size_t i = 0; i < runCount; ++i) {
        CoverageRun r{uint16_t(in.fixed(2)), uint16_t(in.fixed(2)), Status(in.fixed(1))};
        if (r.fromSlot != covered || r.toSlot <= r.fromSlot || r.toSlot > slotCount ||
            uint8_t(r.status) < 1 || uint8_t(r.status) > 3) In::fail("coverage");
        covered = r.toSlot;
        c.coverage.push_back(r);
    }
    if (covered != slotCount) In::fail("coverage");
    const size_t n = size_t(in.bounded(u(2), slotCount));
    if (n && gridCount == 0) In::fail("grid table");
    int64_t prev = -1;
    for (size_t i = 0; i < n; ++i) {
        const auto v = u(2);
        const int64_t slot = spec ? prev + 1 + int64_t(in.bounded(v, slotCount)) : int64_t(v);
        if (slot <= prev || slot >= int64_t(slotCount)) In::fail("slot");
        c.slot.push_back(uint16_t(slot)); prev = slot;
    }
    c.grid.assign(n, 0);
    if (gridCount > 1)
        for (auto& g : c.grid) if ((g = uint8_t(in.fixed(1))) >= gridCount) In::fail("grid index");
    for (auto* f : {&c.open, &c.high, &c.low, &c.close}) f->resize(n);
    constexpr uint64_t maxI64 = uint64_t(INT64_MAX);
    if (spec) {
        std::vector<int64_t> openRel(n);
        std::vector<uint64_t> up(n), down(n), fromLow(n);
        for (auto& v : openRel) v = in.svar();
        for (auto& v : up) v = in.var();
        for (auto& v : down) v = in.var();
        for (auto& v : fromLow) v = in.var();
        int64_t prevClose = 0;
        for (size_t i = 0; i < n; ++i) {
            int64_t open, high, low, close;
            if (__builtin_add_overflow(prevClose, openRel[i], &open) || up[i] > maxI64 || down[i] > maxI64 ||
                fromLow[i] > maxI64 || __builtin_add_overflow(open, int64_t(up[i]), &high) ||
                __builtin_sub_overflow(open, int64_t(down[i]), &low) ||
                __builtin_add_overflow(low, int64_t(fromLow[i]), &close)) In::fail("price");
            c.open[i] = open; c.high[i] = high; c.low[i] = low; c.close[i] = close;
            prevClose = close;
        }
    } else {
        for (auto* f : {&c.open, &c.high, &c.low, &c.close}) for (auto& v : *f) v = int64_t(in.bounded(u(8), maxI64));
    }
    for (auto* f : {&c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol}) {
        f->resize(n);
        for (auto& v : *f) v = int64_t(in.bounded(u(8), maxI64));
    }
    c.count.resize(n);
    for (auto& v : c.count) v = uint32_t(in.bounded(u(4), UINT32_MAX));
    c.rowBegin.assign(1, 0);
    if (!summaryOnly) {
        for (size_t i = 0; i < n; ++i) {
            const auto rows = in.bounded(u(2), 32768);
            if (rows > bytes.size() - in.p) In::fail("rows truncated");
            c.rowBegin.push_back(c.rowBegin.back() + uint32_t(rows));
        }
        const size_t total = c.rowBegin.back();
        if (total > bytes.size() - in.p) In::fail("rows truncated");
        c.offset.resize(total);
        for (size_t i = 0; i < n; ++i) {
            int64_t prevOffset = -1;
            for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
                const int64_t v = spec ? prevOffset + 1 + int64_t(in.bounded(u(2), INT16_MAX))
                                       : int64_t(int16_t(uint16_t(u(2))));
                if (v <= prevOffset || v > INT16_MAX) In::fail("offset");
                c.offset[r] = int16_t(v); prevOffset = v;
            }
        }
        c.rows.resize(total);
        for (auto& r : c.rows) r.bidVol = int64_t(in.bounded(u(8), maxI64));
        for (auto& r : c.rows) r.askVol = int64_t(in.bounded(u(8), maxI64));
        for (auto& r : c.rows) r.bidCount = uint32_t(in.bounded(u(4), UINT32_MAX));
        for (auto& r : c.rows) r.askCount = uint32_t(in.bounded(u(4), UINT32_MAX));
    } else {
        c.rowBegin.assign(n + 1, 0);
    }
    if (in.p != bytes.size()) In::fail("trailing bytes");
    // Section 4 decoder checks.
    for (size_t i = 0; i < n; ++i) {
        const auto& g = c.grids[c.grid[i]];
        if (c.high[i] < c.close[i] || c.low[i] <= 0 || c.buyVol[i] > c.baseVolume[i] ||
            c.sellVol[i] > c.baseVolume[i] - c.buyVol[i]) In::fail("bar totals");
        if (summaryOnly) continue;
        const auto first = c.rowBegin[i], last = c.rowBegin[i + 1];
        if (first == last) In::fail("bar without rows");
        int64_t bid = 0, ask = 0;
        uint64_t known = 0;
        for (auto r = first; r < last; ++r) {
            if (__builtin_add_overflow(bid, c.rows[r].bidVol, &bid) || __builtin_add_overflow(ask, c.rows[r].askVol, &ask))
                In::fail("row volume");
            known += uint64_t(c.rows[r].bidCount) + c.rows[r].askCount;
        }
        const auto lowRow = recording::floorDiv(c.low[i], g.rowTickUnits);
        if (bid != c.sellVol[i] || ask != c.buyVol[i] || known > c.count[i] || c.offset[first] != 0 ||
            c.offset[last - 1] != recording::floorDiv(c.high[i], g.rowTickUnits) - lowRow) In::fail("rows");
    }
    return c;
}

// ---- Day file (section 5) ----------------------------------------------------

inline uint32_t crc32(std::span<const uint8_t> bytes, uint32_t crc = 0) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (auto b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// Table entries: 0-23 flow 1m hours, 24 flow 1h day chunk, 25 flow 1d piece, 26-49 trades.big hours.
constexpr size_t DaySections = 50;
constexpr size_t TableEntryBytes = 25;
struct DaySection {
    bool sealed = false;
    uint32_t summaryLength = 0;     // flow sections: the summary prefix; 0 for trades.big
    uint64_t sourcePrefix = 0;      // hours: fingerprint of the RAWL2 prefix through the seal point
    std::vector<uint8_t> payload;
    bool operator==(const DaySection&) const = default;
};
struct DayFile {
    std::string instrument;
    int64_t dayStartMs = 0;
    Grid grid;
    uint8_t ruleVersion = 1;
    uint32_t sealWindowMs = 15'000;
    uint32_t lateTrades = 0, rejectedTrades = 0, regressions = 0;
    std::array<DaySection, DaySections> sections;
    bool operator==(const DayFile&) const = default;
};

// Writes what it is given (tests feed it bad tables); parseDayFile validates.
inline std::vector<uint8_t> encodeDayFile(const DayFile& d) {
    detail::Out table;
    uint64_t offset = 0; // relative to the end of the table, patched below
    std::vector<uint64_t> starts;
    for (const auto& s : d.sections) { starts.push_back(offset); offset += s.payload.size(); }
    detail::Out h;
    for (char ch : {'F', 'L', 'C', '1'}) h.b.push_back(uint8_t(ch));
    h.fixed(1, 2);           // file version
    h.fixed(0, 2);           // header length, patched below
    h.fixed(0, 4);           // header CRC, patched below
    if (d.instrument.empty() || d.instrument.size() > 72) throw std::invalid_argument("flow proto instrument");
    h.fixed(d.instrument.size(), 1);
    h.b.insert(h.b.end(), d.instrument.begin(), d.instrument.end());
    h.fixed(uint64_t(d.dayStartMs), 8);
    for (auto v : {d.grid.rowTickUnits, d.grid.priceScale, d.grid.baseScale, d.grid.quoteScale}) h.fixed(uint64_t(v), 8);
    h.fixed(d.ruleVersion, 1); h.fixed(d.sealWindowMs, 4);
    h.fixed(d.lateTrades, 4); h.fixed(d.rejectedTrades, 4); h.fixed(d.regressions, 4);
    h.fixed(0, 4);           // table CRC, patched below
    const size_t headerLength = h.b.size();
    const uint64_t base = headerLength + DaySections * TableEntryBytes;
    for (size_t i = 0; i < DaySections; ++i) {
        const auto& s = d.sections[i];
        table.fixed(s.payload.empty() ? 0 : base + starts[i], 4);
        table.fixed(s.summaryLength, 4); table.fixed(s.payload.size(), 4);
        table.fixed(s.payload.empty() ? 0 : crc32(s.payload), 4);
        table.fixed(s.sealed, 1); table.fixed(s.sourcePrefix, 8);
    }
    if (base + offset > UINT32_MAX) throw std::invalid_argument("flow proto day file too large");
    const auto tableCrc = crc32(table.b);
    for (int i = 0; i < 4; ++i) h.b[headerLength - 4 + size_t(i)] = uint8_t(tableCrc >> (8 * i));
    h.b[6] = uint8_t(headerLength); h.b[7] = uint8_t(headerLength >> 8);
    const auto crc = crc32(h.b);
    for (int i = 0; i < 4; ++i) h.b[8 + i] = uint8_t(crc >> (8 * i));
    h.b.insert(h.b.end(), table.b.begin(), table.b.end());
    for (const auto& s : d.sections) h.b.insert(h.b.end(), s.payload.begin(), s.payload.end());
    return std::move(h.b);
}

// The payload key of a flow entry (0-25) of a day file.
struct EntryKey { int64_t levelMs, startMs; uint32_t slots; };
inline EntryKey flowEntryKey(size_t entry, int64_t dayStartMs) {
    constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
    if (entry < 24) return {Minute, dayStartMs + int64_t(entry) * Hour, 60};
    if (entry == 24) return {Hour, dayStartMs, 24};
    return {Day, dayStartMs, 1};
}

// Validates the header CRC, the table CRC, the table (sections back to back in
// table order; a sealed entry has a payload, an absent one none; a flow entry's
// summary length is a decodable summary prefix of a decodable payload; a
// trades.big entry has summary length 0) and every section CRC.
inline DayFile parseDayFile(std::span<const uint8_t> bytes) {
    using detail::In;
    In in{bytes};
    for (char ch : {'F', 'L', 'C', '1'}) if (in.fixed(1) != uint8_t(ch)) In::fail("day magic");
    if (in.fixed(2) != 1) In::fail("day version");
    const auto headerLength = in.fixed(2);
    const auto storedCrc = uint32_t(in.fixed(4));
    if (headerLength < 16 || headerLength > bytes.size()) In::fail("day header length");
    std::vector<uint8_t> header(bytes.begin(), bytes.begin() + ptrdiff_t(headerLength));
    std::fill(header.begin() + 8, header.begin() + 12, 0);
    if (crc32(header) != storedCrc) In::fail("day header crc");
    DayFile d;
    const auto len = in.fixed(1);
    if (len == 0 || len > 72 || len > bytes.size() - in.p) In::fail("day instrument");
    d.instrument.assign(reinterpret_cast<const char*>(bytes.data() + in.p), len);
    in.p += len;
    d.dayStartMs = int64_t(in.fixed(8));
    d.grid = {int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8))};
    d.ruleVersion = uint8_t(in.fixed(1)); d.sealWindowMs = uint32_t(in.fixed(4));
    d.lateTrades = uint32_t(in.fixed(4)); d.rejectedTrades = uint32_t(in.fixed(4)); d.regressions = uint32_t(in.fixed(4));
    const auto tableCrc = uint32_t(in.fixed(4));
    if (in.p != headerLength || d.dayStartMs % 86'400'000) In::fail("day header");
    uint64_t next = headerLength + DaySections * TableEntryBytes;
    if (next > bytes.size()) In::fail("day table truncated");
    if (crc32(bytes.subspan(headerLength, DaySections * TableEntryBytes)) != tableCrc) In::fail("day table crc");
    for (size_t i = 0; i < DaySections; ++i) {
        auto& s = d.sections[i];
        const auto offset = in.fixed(4), summary = in.fixed(4), length = in.fixed(4), crc = in.fixed(4);
        const auto sealed = in.fixed(1);
        s.sourcePrefix = in.fixed(8);
        const bool flow = i < 26;
        if (sealed > 1 || bool(sealed) != (length > 0) || (length ? offset != next : offset != 0) ||
            length > bytes.size() - next || (flow ? (sealed && (summary == 0 || summary > length)) || (!sealed && summary)
                                                  : summary != 0)) In::fail("day table");
        s.sealed = sealed; s.summaryLength = uint32_t(summary);
        s.payload.assign(bytes.begin() + ptrdiff_t(next), bytes.begin() + ptrdiff_t(next + length));
        if (length && crc32(s.payload) != crc) In::fail("day section crc");
        if (flow && sealed) {
            const auto k = flowEntryKey(i, d.dayStartMs);
            decode(std::span(s.payload).first(summary), Encoding::Spec, k.levelMs, k.startMs, k.slots, true);
            decode(s.payload, Encoding::Spec, k.levelMs, k.startMs, k.slots);
        }
        next += length;
    }
    if (next != bytes.size()) In::fail("day trailing bytes");
    return d;
}

// Section 5 assembly of a 16-day 1d chunk from day pieces (entry 25 of each
// day file: one-slot payloads keyed by the day). pieces[i] is the piece of
// start + i days, or empty when that day has none; such days get `absent`.
inline FlowChunk assembleDayChunk(int64_t startMs, std::span<const std::vector<uint8_t>> pieces, Status absent) {
    constexpr int64_t Day = 86'400'000;
    FlowChunk out;
    out.levelMs = Day; out.startMs = startMs; out.slotCount = uint32_t(pieces.size());
    out.rowBegin.push_back(0);
    for (size_t day = 0; day < pieces.size(); ++day) {
        Status status = absent;
        if (!pieces[day].empty()) {
            const auto p = decode(pieces[day], Encoding::Spec, Day, startMs + int64_t(day) * Day, 1);
            status = p.coverage.front().status;
            if (p.bars()) {
                size_t gi = 0;
                while (gi < out.grids.size() && !(out.grids[gi] == p.grids[0])) ++gi;
                if (gi == out.grids.size()) out.grids.push_back(p.grids[0]);
                out.slot.push_back(uint16_t(day)); out.grid.push_back(uint8_t(gi));
                out.open.push_back(p.open[0]); out.high.push_back(p.high[0]);
                out.low.push_back(p.low[0]); out.close.push_back(p.close[0]);
                out.baseVolume.push_back(p.baseVolume[0]); out.quoteVolume.push_back(p.quoteVolume[0]);
                out.buyVol.push_back(p.buyVol[0]); out.sellVol.push_back(p.sellVol[0]);
                out.count.push_back(p.count[0]);
                out.offset.insert(out.offset.end(), p.offset.begin(), p.offset.end());
                out.rows.insert(out.rows.end(), p.rows.begin(), p.rows.end());
                out.rowBegin.push_back(uint32_t(out.rows.size()));
            }
        }
        if (!out.coverage.empty() && out.coverage.back().status == status) out.coverage.back().toSlot = uint16_t(day + 1);
        else out.coverage.push_back({uint16_t(day), uint16_t(day + 1), status});
    }
    return out;
}
} // namespace sentinel::flow::proto
