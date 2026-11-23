// Pure sealed flow payload codec; byte layout is docs/FLOW_CHUNKS.md section 4.
#include "FlowChunkCodec.hpp"
#include "FlowChunkPrimitives.hpp"
#include <algorithm>
#include <cmath>
namespace sentinel::flow::chunks {
namespace {
void validate(const ChunkData& c, bool summaryOnly) {
    const auto nBars = c.bars();
    if (nBars > c.slotCount || (c.grids.size() > (c.levelMs == 86400000 && c.slotCount == 16 ? 16u : 1u)) || c.coverage.size() > 60 ||
        c.grid.size() != nBars || c.count.size() != nBars || (!summaryOnly &&
        (c.rowBegin.size() != nBars + 1 || c.rowBegin.front() != 0 || c.rowBegin.back() != c.rows.size() ||
         c.offset.size() != c.rows.size() || !validStep(c.priceStep))))
        throw std::invalid_argument("flow chunk field lengths");
    for (const auto* f : {&c.open, &c.high, &c.low, &c.close, &c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol})
        if (f->size() != nBars) throw std::invalid_argument("flow chunk field length");
    for (size_t i = 0; i < nBars; ++i) {
        if (c.grid[i] >= c.grids.size() || c.low[i] <= 0 || c.low[i] > c.open[i] || c.open[i] > c.high[i] ||
            c.close[i] < c.low[i] || c.close[i] > c.high[i] || (!summaryOnly && c.rowBegin[i] > c.rowBegin[i + 1]))
            throw std::invalid_argument("flow chunk bar");
    }
    if (!((c.levelMs == 60000 && c.slotCount == 60) || (c.levelMs == 3600000 && c.slotCount == 24) ||
          (c.levelMs == 86400000 && (c.slotCount == 16 || c.slotCount == 1))))
        throw std::invalid_argument("flow chunk extent");
    for (const auto& g : c.grids)
        if (g.rowTickUnits <= 0 || g.priceScale <= 0 || g.baseScale <= 0 || g.quoteScale <= 0)
            throw std::invalid_argument("flow chunk grid");
    uint16_t covered = 0;
    for (const auto& r : c.coverage) {
        if (r.fromSlot != covered || r.toSlot <= r.fromSlot || r.toSlot > c.slotCount ||
            uint8_t(r.status) < 1 || uint8_t(r.status) > 3)
            throw std::invalid_argument("flow chunk coverage is not sealed");
        covered = r.toSlot;
    }
    if (covered != c.slotCount) throw std::invalid_argument("flow chunk coverage extent");
    for (size_t i = 0; i < nBars; ++i) {
        if (c.slot[i] >= c.slotCount || (i && c.slot[i] <= c.slot[i - 1]) || c.baseVolume[i] <= 0 ||
            c.quoteVolume[i] < 0 || c.buyVol[i] < 0 || c.sellVol[i] < 0 || !c.count[i] ||
            c.buyVol[i] > c.baseVolume[i] || c.sellVol[i] > c.baseVolume[i] - c.buyVol[i])
            throw std::invalid_argument("flow chunk summary");
        if (summaryOnly) continue;
        const auto first = c.rowBegin[i], last = c.rowBegin[i + 1];
        if (first == last || last - first > 65535) throw std::invalid_argument("flow chunk row count");
        int64_t bid = 0, ask = 0;
        uint64_t known = 0;
        for (auto r = first; r < last; ++r) {
            const auto& row = c.rows[r];
            if (c.offset[r] > MaxOffset || (r > first && c.offset[r] <= c.offset[r - 1]) ||
                row.bidVol < 0 || row.askVol < 0 || row.bidVol > c.sellVol[i] - bid || row.askVol > c.buyVol[i] - ask)
                throw std::invalid_argument("flow chunk row");
            bid += row.bidVol; ask += row.askVol; known += uint64_t(row.bidCount) + row.askCount;
        }
        const auto tick = c.grids[c.grid[i]].rowTickUnits;
        if (bid != c.sellVol[i] || ask != c.buyVol[i] || known > c.count[i] || c.offset[first] != 0 ||
            c.offset[last - 1] != rowAt(c.high[i], tick, c.priceStep) - rowAt(c.low[i], tick, c.priceStep))
            throw std::invalid_argument("flow chunk row totals or extent");
    }
}
int64_t toUnits(double price, int64_t scale) {
    const auto units = recording::priceUnits(price, double(scale));
    if (!units) throw std::invalid_argument("flow chunk price not representable");
    return *units;
}

} // namespace

// Explicit barGrids names each bar's original recorder grid; nativeTick may
// carry a coarser step. Without it, input ticks are treated as recorder grids.
ChunkData fromBars(std::span<const FlowBar> bars, int64_t levelMs, int64_t startMs, uint32_t slotCount,
                   Stats* stats, std::span<const FlowGrid> barGrids, std::span<const CoverageRun> coverage) {
    if (levelMs <= 0 || slotCount == 0 || slotCount > 60 || (!barGrids.empty() && barGrids.size() != bars.size()))
        throw std::invalid_argument("flow chunk input extent");
    ChunkData c;
    c.levelMs = levelMs; c.startMs = startMs; c.slotCount = slotCount;
    if (coverage.empty()) c.coverage.push_back({0, uint16_t(slotCount), Status::Complete});
    else c.coverage.assign(coverage.begin(), coverage.end());
    std::vector<uint64_t> steps;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const auto tick = tickUnits(b.nativeTick, b.priceScale);
        FlowGrid g = barGrids.empty() ? FlowGrid{tick, b.priceScale, b.baseScale, b.quoteScale} : barGrids[i];
        if (g.rowTickUnits <= 0 || g.priceScale != b.priceScale || g.baseScale != b.baseScale ||
            g.quoteScale != b.quoteScale || tick % g.rowTickUnits || !validStep(uint64_t(tick / g.rowTickUnits)))
            throw std::invalid_argument("flow chunk bar grid");
        const auto step = uint64_t(tick / g.rowTickUnits);
        steps.push_back(step);
        c.priceStep = std::max(c.priceStep, stepFor(toUnits(b.low, b.priceScale), toUnits(b.high, b.priceScale), g.rowTickUnits, step));
        if (stats && double(tick) / double(b.priceScale) != b.nativeTick) ++stats->nativeTickMismatch;
        size_t gi = 0;
        while (gi < c.grids.size() && !(c.grids[gi] == g)) ++gi;
        if (gi == c.grids.size()) c.grids.push_back(g);
        if (gi >= 16 || (levelMs != 86400000 && gi)) throw std::invalid_argument("flow chunk grid table");
        c.grid.push_back(uint8_t(gi));
    }
    c.rowBegin.push_back(0);
    int64_t previous = -1;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        int64_t delta;
        if (b.timeframeMs != levelMs || __builtin_sub_overflow(b.startMs, startMs, &delta) || delta < 0 || delta % levelMs)
            throw std::invalid_argument("flow chunk bar outside chunk");
        const auto slot = delta / levelMs;
        if (slot >= int64_t(slotCount) || slot <= previous) throw std::invalid_argument("flow chunk slot");
        previous = slot; c.slot.push_back(uint16_t(slot));
        c.open.push_back(toUnits(b.open, b.priceScale)); c.high.push_back(toUnits(b.high, b.priceScale));
        c.low.push_back(toUnits(b.low, b.priceScale)); c.close.push_back(toUnits(b.close, b.priceScale));
        const auto& g = c.grids[c.grid[i]];
        const auto oldLow = rowAt(c.low.back(), g.rowTickUnits, steps[i]);
        if (stats && oldLow != b.lowRow) ++stats->lowRowMismatch;
        if (oldLow != b.lowRow) throw std::invalid_argument("flow chunk low row");
        c.baseVolume.push_back(b.baseVolume); c.quoteVolume.push_back(b.quoteVolume);
        c.buyVol.push_back(b.buyVol); c.sellVol.push_back(b.sellVol);
        if (b.count > UINT32_MAX) throw std::invalid_argument("flow chunk count");
        c.count.push_back(uint32_t(b.count));
        const auto ratio = int64_t(c.priceStep / steps[i]);
        const auto newLow = recording::floorDiv(oldLow, ratio);
        int64_t last = -1;
        for (const auto& r : b.rows) {
            if (r.offset > INT64_MAX - oldLow) throw std::invalid_argument("flow chunk row overflow");
            const auto offset = recording::floorDiv(oldLow + r.offset, ratio) - newLow;
            if (offset > MaxOffset || offset < last) throw std::invalid_argument("flow chunk offset");
            if (offset == last) {
                auto& d = c.rows.back();
                if (__builtin_add_overflow(d.bidVol, r.bidVol, &d.bidVol) ||
                    __builtin_add_overflow(d.askVol, r.askVol, &d.askVol) ||
                    __builtin_add_overflow(d.bidCount, r.bidCount, &d.bidCount) ||
                    __builtin_add_overflow(d.askCount, r.askCount, &d.askCount))
                    throw std::overflow_error("flow chunk row totals");
            } else {
                c.offset.push_back(uint16_t(offset)); c.rows.push_back({r.bidVol, r.askVol, r.bidCount, r.askCount});
                last = offset;
            }
        }
        c.rowBegin.push_back(uint32_t(c.rows.size()));
    }
    return c;
}

// Inverse of fromBars, without notables. lowRow, delta and maxRowVol are derived.
// The FlowBar tick carries the chunk step.
std::vector<FlowBar> toBars(const ChunkData& c) {
    validate(c, false);
    std::vector<FlowBar> out;
    out.reserve(c.bars());
    for (size_t i = 0; i < c.bars(); ++i) {
        const auto& g = c.grids.at(c.grid[i]);
        FlowBar b;
        b.startMs = c.startMs + int64_t(c.slot[i]) * c.levelMs;
        b.timeframeMs = c.levelMs;
        b.nativeTick = double(g.rowTickUnits) * double(c.priceStep) / double(g.priceScale);
        b.priceScale = g.priceScale; b.baseScale = g.baseScale; b.quoteScale = g.quoteScale;
        const double scale = double(g.priceScale);
        b.open = double(c.open[i]) / scale; b.high = double(c.high[i]) / scale;
        b.low = double(c.low[i]) / scale; b.close = double(c.close[i]) / scale;
        b.lowRow = rowAt(c.low[i], g.rowTickUnits, c.priceStep);
        b.baseVolume = c.baseVolume[i]; b.quoteVolume = c.quoteVolume[i];
        b.buyVol = c.buyVol[i]; b.sellVol = c.sellVol[i]; b.delta = b.buyVol - b.sellVol;
        b.count = c.count[i];
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            const auto& rec = c.rows[r];
            b.rows.push_back({uint16_t(c.offset[r]), rec.bidVol, rec.askVol, rec.bidCount, rec.askCount});
            b.maxRowVol = std::max(b.maxRowVol, rec.bidVol + rec.askVol);
        }
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<uint8_t> encode(const ChunkData& c, bool summaryOnly, uint32_t* summaryLength) {
    validate(c, summaryOnly);
    detail::Out o;
    o.fixed(c.grids.size(), 1);
    for (const auto& g : c.grids)
        for (auto v : {g.rowTickUnits, g.priceScale, g.baseScale, g.quoteScale}) o.fixed(uint64_t(v), 8);
    o.fixed(c.coverage.size(), 1);
    for (const auto& r : c.coverage) {
        if (r.status == Status::Building) throw std::invalid_argument("flow chunk unsealed coverage");
        o.fixed(r.fromSlot, 2); o.fixed(r.toSlot, 2); o.fixed(uint8_t(r.status), 1);
    }
    const size_t n = c.bars();
    o.var(n);
    int64_t prev = -1;
    for (size_t i = 0; i < n; ++i) { o.var(uint64_t(c.slot[i] - prev - 1)); prev = c.slot[i]; }
    if (c.grids.size() > 1) for (size_t i = 0; i < n; ++i) o.fixed(c.grid[i], 1);
    {
        int64_t prevClose = 0;
        for (size_t i = 0; i < n; ++i) { o.svar(c.open[i] - prevClose); prevClose = c.close[i]; }
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.high[i] - c.open[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.open[i] - c.low[i]));
        for (size_t i = 0; i < n; ++i) o.var(uint64_t(c.close[i] - c.low[i]));
    }
    for (const auto* f : {&c.baseVolume, &c.quoteVolume, &c.buyVol, &c.sellVol}) for (auto v : *f) o.var(uint64_t(v));
    for (auto v : c.count) o.var(v);
    if (summaryLength) *summaryLength = uint32_t(o.b.size());
    if (summaryOnly) return std::move(o.b);
    for (size_t i = 0; i < n; ++i) o.var(c.rowBegin[i + 1] - c.rowBegin[i]);
    for (size_t i = 0; i < n; ++i) {
        int64_t prevOffset = -1;
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            o.var(uint64_t(c.offset[r] - prevOffset - 1));
            prevOffset = c.offset[r];
        }
    }
    for (const auto& r : c.rows) o.var(uint64_t(r.bidVol));
    for (const auto& r : c.rows) o.var(uint64_t(r.askVol));
    for (const auto& r : c.rows) o.var(r.bidCount);
    for (const auto& r : c.rows) o.var(r.askCount);
    if (o.b.size() > 16 * 1024 * 1024) throw std::length_error("flow chunk payload too large");
    return std::move(o.b);
}

// Re-bins every bar of a chunk to a coarser step (exact: each step divides the next).
ChunkData rebin(const ChunkData& c, uint64_t step) {
    validate(c, false);
    if (!validStep(step) || !validStep(c.priceStep) || step < c.priceStep)
        throw std::invalid_argument("flow chunk rebin step");
    const auto ratio = int64_t(step / c.priceStep);
    ChunkData out = c;
    out.priceStep = step; out.offset.clear(); out.rows.clear(); out.rowBegin.assign(1, 0);
    for (size_t i = 0; i < c.bars(); ++i) {
        const auto low = rowAt(c.low[i], c.grids.at(c.grid[i]).rowTickUnits, c.priceStep);
        const auto newLow = recording::floorDiv(low, ratio);
        int64_t last = -1;
        for (uint32_t r = c.rowBegin[i]; r < c.rowBegin[i + 1]; ++r) {
            const auto offset = recording::floorDiv(low + c.offset[r], ratio) - newLow;
            if (offset > MaxOffset) throw std::invalid_argument("flow chunk rebin range");
            if (offset == last) {
                auto& d = out.rows.back();
                if (__builtin_add_overflow(d.bidVol, c.rows[r].bidVol, &d.bidVol) ||
                    __builtin_add_overflow(d.askVol, c.rows[r].askVol, &d.askVol) ||
                    __builtin_add_overflow(d.bidCount, c.rows[r].bidCount, &d.bidCount) ||
                    __builtin_add_overflow(d.askCount, c.rows[r].askCount, &d.askCount))
                    throw std::overflow_error("flow chunk rebin totals");
            } else {
                out.offset.push_back(uint16_t(offset)); out.rows.push_back(c.rows[r]); last = offset;
            }
        }
        out.rowBegin.push_back(uint32_t(out.rows.size()));
    }
    return out;
}

// Section 5 assembly of a 16-day 1d chunk from day pieces (entry 25 of each
// day file: one-slot payloads keyed by the day). pieces[i] is the piece of
// start + i days, or empty when that day has none; such days get `absent`.
// steps[i] is piece i's price_step (all 1 when empty); the chunk takes the
// largest and finer pieces are re-binned to it.
ChunkData assembleDayChunk(int64_t startMs, std::span<const std::vector<uint8_t>> pieces, Status absent,
                                  std::span<const uint64_t> steps) {
    constexpr int64_t Day = 86'400'000;
    if (pieces.size() != 16) throw std::invalid_argument("flow chunk assembly needs sixteen days");
    if (!steps.empty() && steps.size() != pieces.size()) throw std::invalid_argument("flow chunk piece steps");
    const auto stepOf = [&](size_t day) { return steps.empty() ? uint64_t(1) : steps[day]; };
    ChunkData out;
    out.levelMs = Day; out.startMs = startMs; out.slotCount = uint32_t(pieces.size());
    for (size_t day = 0; day < pieces.size(); ++day)
        if (!pieces[day].empty()) out.priceStep = std::max(out.priceStep, stepOf(day));
    out.rowBegin.push_back(0);
    for (size_t day = 0; day < pieces.size(); ++day) {
        Status status = absent;
        if (!pieces[day].empty()) {
            const auto p = rebin(toData(decode(pieces[day], Day, startMs + int64_t(day) * Day, 1, false,
                                        stepOf(day))), out.priceStep);
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
}
