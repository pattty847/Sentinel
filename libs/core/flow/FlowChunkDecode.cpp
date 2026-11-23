// Parse bounded summary metadata on the stack, then allocate every decoded field
// together. Row counts must fit the remaining wire bytes before that allocation.
#include "FlowChunkCodec.hpp"
#include "FlowChunkPrimitives.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace sentinel::flow::chunks {
namespace {
using detail::In;
constexpr size_t MaxBars = 60, MaxGrids = 16;
constexpr size_t MaxPayload = 16 * 1024 * 1024;
struct Summary {
    std::array<FlowGrid, MaxGrids> grids{};
    std::array<CoverageRun, MaxBars> coverage{};
    std::array<int64_t, MaxBars> open{}, high{}, low{}, close{}, base{}, quote{}, buy{}, sell{};
    std::array<uint32_t, MaxBars> count{};
    std::array<uint32_t, MaxBars + 1> rowBegin{};
    std::array<uint16_t, MaxBars> slot{};
    std::array<uint8_t, MaxBars> grid{};
    size_t ng = 0, nr = 0, n = 0;
};
bool validExtent(int64_t level, uint32_t slotCount) {
    return (level == 60000 && slotCount == 60) || (level == 3600000 && slotCount == 24) ||
           (level == 86400000 && (slotCount == 16 || slotCount == 1));
}
void readHeader(In& in, Summary& s, int64_t level, uint16_t slotCount) {
    s.ng = in.bounded(in.fixed(1), level == 86400000 && slotCount == 16 ? 16 : 1);
    for (size_t i = 0; i < s.ng; ++i) {
        auto& g = s.grids[i];
        g = {int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8))};
        if (g.rowTickUnits <= 0 || g.priceScale <= 0 || g.baseScale <= 0 || g.quoteScale <= 0) In::fail("grid");
    }
    s.nr = in.bounded(in.fixed(1), slotCount);
    uint16_t covered = 0;
    for (size_t i = 0; i < s.nr; ++i) {
        auto& r = s.coverage[i];
        r = {uint16_t(in.fixed(2)), uint16_t(in.fixed(2)), Status(in.fixed(1))};
        if (r.fromSlot != covered || r.toSlot <= r.fromSlot || r.toSlot > slotCount ||
            uint8_t(r.status) < 1 || uint8_t(r.status) > 3) In::fail("coverage");
        covered = r.toSlot;
    }
    if (covered != slotCount) In::fail("coverage");
    s.n = in.bounded(in.var(), slotCount);
    if (s.n && !s.ng) In::fail("grid table");
    // At least nine summary varints and a slot per bar.
    if (s.n > (in.b.size() - in.p) / 10) In::fail("summary truncated");
    int64_t previous = -1;
    for (size_t i = 0; i < s.n; ++i) {
        const auto slot = previous + 1 + int64_t(in.bounded(in.var(), slotCount));
        if (slot >= slotCount) In::fail("slot");
        s.slot[i] = uint16_t(slot); previous = slot;
    }
    if (s.ng > 1)
        for (size_t i = 0; i < s.n; ++i) s.grid[i] = uint8_t(in.bounded(in.fixed(1), s.ng - 1));
}
void readPrices(In& in, Summary& s) {
    // Four field-major streams; only open depends on the preceding close.
    std::array<int64_t, MaxBars> relative{};
    std::array<uint64_t, MaxBars> up{}, down{}, fromLow{};
    for (size_t i = 0; i < s.n; ++i) relative[i] = in.svar();
    for (auto* field : {&up, &down, &fromLow})
        for (size_t i = 0; i < s.n; ++i) (*field)[i] = in.bounded(in.var(), INT64_MAX);
    int64_t previous = 0;
    for (size_t i = 0; i < s.n; ++i) {
        if (__builtin_add_overflow(previous, relative[i], &s.open[i]) ||
            __builtin_add_overflow(s.open[i], int64_t(up[i]), &s.high[i]) ||
            __builtin_sub_overflow(s.open[i], int64_t(down[i]), &s.low[i]) ||
            __builtin_add_overflow(s.low[i], int64_t(fromLow[i]), &s.close[i])) In::fail("price");
        previous = s.close[i];
    }
    for (auto* field : {&s.base, &s.quote, &s.buy, &s.sell})
        for (size_t i = 0; i < s.n; ++i) (*field)[i] = int64_t(in.bounded(in.var(), INT64_MAX));
    for (size_t i = 0; i < s.n; ++i) s.count[i] = uint32_t(in.bounded(in.var(), UINT32_MAX));
    for (size_t i = 0; i < s.n; ++i)
        if (s.low[i] <= 0 || s.high[i] < s.close[i] || s.base[i] <= 0 || !s.count[i] ||
            s.buy[i] > s.base[i] || s.sell[i] > s.base[i] - s.buy[i]) In::fail("bar totals");
}
// Same layout calculation for sizing and binding: all elements are implicit-
// lifetime types, aligned within new[] storage. Public views are read-only.
struct Layout {
    std::byte* data = nullptr;
    size_t used = 0;
    template<class T> std::span<const T> field(size_t count) {
        if (!count) return {};
        used = (used + alignof(T) - 1) & ~(alignof(T) - 1);
        const auto begin = data ? reinterpret_cast<T*>(data + used) : nullptr;
        used += sizeof(T) * count;
        return data ? std::span<const T>(begin, count) : std::span<const T>();
    }
};
void layout(FlowChunk& c, Layout& l, const Summary& s, bool summary) {
    c.grids = l.field<FlowGrid>(s.ng);
    c.row = l.field<FlowRow>(c.rows);
    for (auto* f : {&c.open, &c.high, &c.low, &c.close, &c.baseVol, &c.quoteVol, &c.buyVol, &c.sellVol})
        *f = l.field<int64_t>(s.n);
    c.count = l.field<uint32_t>(s.n);
    c.rowBegin = l.field<uint32_t>(summary ? 0 : s.n + 1);
    c.coverage = l.field<CoverageRun>(s.nr);
    c.slot = l.field<uint16_t>(s.n);
    c.offset = l.field<uint16_t>(c.rows);
    c.grid = l.field<uint8_t>(s.ng > 1 ? s.n : 0);
}
template<class T, size_t N> void copy(std::span<const T> out, const std::array<T, N>& from) {
    if (!out.empty()) std::memcpy(const_cast<T*>(out.data()), from.data(), out.size_bytes());
}
void readRows(In& in, FlowChunk& c, const Summary& s) {
    auto* offsets = const_cast<uint16_t*>(c.offset.data());
    auto* rows = const_cast<FlowRow*>(c.row.data());
    for (size_t i = 0; i < s.n; ++i) {
        int64_t previous = -1;
        for (auto r = s.rowBegin[i]; r < s.rowBegin[i + 1]; ++r) {
            const auto v = previous + 1 + int64_t(in.bounded(in.var(), MaxOffset));
            if (v > MaxOffset) In::fail("offset");
            offsets[r] = uint16_t(v); previous = v;
        }
    }
    for (size_t r = 0; r < c.rows; ++r) rows[r].bidVol = int64_t(in.bounded(in.var(), INT64_MAX));
    for (size_t r = 0; r < c.rows; ++r) rows[r].askVol = int64_t(in.bounded(in.var(), INT64_MAX));
    for (size_t r = 0; r < c.rows; ++r) rows[r].bidCount = uint32_t(in.bounded(in.var(), UINT32_MAX));
    for (size_t r = 0; r < c.rows; ++r) rows[r].askCount = uint32_t(in.bounded(in.var(), UINT32_MAX));
    for (size_t i = 0; i < s.n; ++i) {
        const auto first = s.rowBegin[i], last = s.rowBegin[i + 1];
        int64_t bid = 0, ask = 0;
        uint64_t known = 0;
        for (auto r = first; r < last; ++r) {
            if (__builtin_add_overflow(bid, rows[r].bidVol, &bid) ||
                __builtin_add_overflow(ask, rows[r].askVol, &ask)) In::fail("row volume");
            known += uint64_t(rows[r].bidCount) + rows[r].askCount;
        }
        const auto tick = s.grids[s.grid[i]].rowTickUnits;
        if (bid != s.sell[i] || ask != s.buy[i] || known > s.count[i] || offsets[first] != 0 ||
            offsets[last - 1] != rowAt(s.high[i], tick, c.key.priceStep) - rowAt(s.low[i], tick, c.key.priceStep))
            In::fail("rows");
    }
}
}
FlowChunk decode(std::span<const uint8_t> bytes, FlowChunkKey key, uint16_t slotCount, uint64_t epoch) {
    const bool summary = key.channel == "flow.summary";
    if ((!summary && key.channel != "flow.rows") || !validExtent(key.levelMs, slotCount) ||
        (summary ? key.priceStep != 0 : !validStep(key.priceStep)) || bytes.size() > MaxPayload)
        In::fail("key or payload size");
    if (key.startMs > INT64_MAX - int64_t(slotCount) * key.levelMs) In::fail("key extent overflow");
    In in{bytes};
    Summary s;
    readHeader(in, s, key.levelMs, slotCount);
    readPrices(in, s);
    if (!summary) {
        for (size_t i = 0; i < s.n; ++i) {
            const auto count = in.bounded(in.var(), MaxOffset + 1);
            if (!count) In::fail("bar without rows");
            s.rowBegin[i + 1] = s.rowBegin[i] + uint32_t(count);
        }
        // Each row needs five varints (offset and four quantities).
        if (s.rowBegin[s.n] > (bytes.size() - in.p) / 5) In::fail("rows truncated");
    }
    FlowChunk c;
    c.key = std::move(key); c.epoch = epoch; c.slotCount = slotCount; c.bars = uint16_t(s.n); c.rows = s.rowBegin[s.n];
    Layout size;
    layout(c, size, s, summary);
    c.storageBytes = size.used;
    c.storage = std::make_unique<std::byte[]>(size.used);
    Layout buffer{c.storage.get()};
    layout(c, buffer, s, summary);
    copy(c.grids, s.grids); copy(c.coverage, s.coverage);
    copy(c.open, s.open); copy(c.high, s.high); copy(c.low, s.low); copy(c.close, s.close);
    copy(c.baseVol, s.base); copy(c.quoteVol, s.quote); copy(c.buyVol, s.buy); copy(c.sellVol, s.sell);
    copy(c.count, s.count); copy(c.rowBegin, s.rowBegin); copy(c.slot, s.slot); copy(c.grid, s.grid);
    if (!summary) readRows(in, c, s);
    if (in.p != bytes.size()) In::fail("trailing bytes");
    return c;
}
FlowChunk decode(std::span<const uint8_t> bytes, int64_t level, int64_t start, uint32_t slotCount,
                 bool summary, uint64_t step) {
    if (slotCount > UINT16_MAX) In::fail("slotCount");
    return decode(bytes, {{}, summary ? "flow.summary" : "flow.rows", level, summary ? 0 : step, start}, uint16_t(slotCount));
}
ChunkData toData(const FlowChunk& c) {
    ChunkData d;
    d.levelMs = c.key.levelMs; d.startMs = c.key.startMs; d.slotCount = c.slotCount; d.priceStep = c.key.priceStep;
    const auto assign = [](auto& to, auto from) { to.assign(from.begin(), from.end()); };
    assign(d.grids, c.grids); assign(d.coverage, c.coverage);
    assign(d.open, c.open); assign(d.high, c.high); assign(d.low, c.low); assign(d.close, c.close);
    assign(d.baseVolume, c.baseVol); assign(d.quoteVolume, c.quoteVol); assign(d.buyVol, c.buyVol); assign(d.sellVol, c.sellVol);
    assign(d.count, c.count); assign(d.rowBegin, c.rowBegin); assign(d.slot, c.slot); assign(d.grid, c.grid);
    if (c.grid.empty()) d.grid.assign(c.bars, 0);
    assign(d.rows, c.row); assign(d.offset, c.offset);
    return d;
}
std::vector<FlowBar> toBars(const FlowChunk& c) { return toBars(toData(c)); }
} // namespace sentinel::flow::chunks
