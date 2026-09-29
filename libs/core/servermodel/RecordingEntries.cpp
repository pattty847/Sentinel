#include "RecordingEntries.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iterator>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>

namespace recording {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point then) {
    return std::chrono::duration<double, std::milli>(Clock::now() - then).count();
}
constexpr int64_t minute = 60'000;
struct Entry { uint32_t col; int64_t row; uint32_t side; uint16_t code; };
void finish(RecordingEntries &out, std::vector<Entry> &entries,
            std::vector<std::array<int64_t, 4>> &bounds,
            const std::vector<int64_t> &nativeUnits,
            const std::vector<SizeScale> &columnScales, int64_t commonUnits) {
    for (auto &e : entries) e.row = floorDiv(e.row, commonUnits);
    for (size_t c = 0; c < bounds.size(); ++c) {
        if (!nativeUnits[c]) continue;
        auto &b = bounds[c];
        for (int i : {0, 2}) {
            if (b[i] > b[i + 1]) continue;
            // A common row is valid only if every underlying native row was
            // covered, including the zero-valued rows absent from entries.
            const int64_t loUnits = b[i] * nativeUnits[c];
            const int64_t endUnits = (b[i + 1] + 1) * nativeUnits[c];
            b[i] = -floorDiv(-loUnits, commonUnits);
            b[i + 1] = floorDiv(endUnits, commonUnits) - 1;
        }
    }
    int64_t base = std::numeric_limits<int64_t>::max();
    for (const auto &e : entries) base = std::min(base, e.row);
    for (const auto &b : bounds)
        for (int i : {0, 2}) if (b[i] <= b[i + 1]) base = std::min(base, b[i]);
    out.baseRow = base == std::numeric_limits<int64_t>::max() ? 0 : base;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.col != b.col) return a.col < b.col;
        if (a.row != b.row) return a.row < b.row;
        return a.side < b.side;
    });
    out.rowSide.reserve(entries.size()); out.code.reserve(entries.size());
    out.offsets.resize(out.columns() + 1);
    out.nativeFactor.resize(out.columns());
    out.columnScale = columnScales;
    size_t next = 0;
    for (uint32_t c = 0; c < out.columns(); ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.rowSide.size());
        while (next < entries.size() && entries[next].col == c) {
            const auto &e = entries[next++];
            const auto rel = e.row - out.baseRow;
            if (rel < 0 || rel > 0x7fffffffu)
                throw std::runtime_error("recording native row range exceeds GPU index width");
            const uint32_t key = uint32_t(rel) | (e.side << 31);
            // Retain each native entry. Re-encoding a partial sum here loses
            // precision and can saturate the HMC2 15-bit range.
            out.rowSide.push_back(key);
            out.code.push_back(e.code);
        }
        if (nativeUnits[c] / commonUnits > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("recording native tick factor exceeds GPU width");
        out.nativeFactor[c] = nativeUnits[c] ? uint32_t(nativeUnits[c] / commonUnits) : 0;
        const auto &b = bounds[c];
        auto convert = [&](int i) -> std::pair<int32_t, int32_t> {
            if (b[i] > b[i + 1]) return {1, 0};
            const auto lo = b[i] - out.baseRow, hi = b[i + 1] - out.baseRow;
            if (lo < 0 || hi > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("recording coverage exceeds GPU index width");
            return {static_cast<int32_t>(lo), static_cast<int32_t>(hi)};
        };
        const auto bid = convert(0), ask = convert(2);
        out.coverage[c] = {bid.first, bid.second, ask.first, ask.second};
    }
    out.offsets.back() = static_cast<uint32_t>(out.rowSide.size());
}

uint32_t rowKey(uint32_t packed) { return ((packed & 0x7fffffffu) << 1) | (packed >> 31); }

void buildLod(RecordingEntries &out) {
    auto &lod = out.lod;
    const uint32_t count = (out.columns() + RecordingEntries::kLodMinutes - 1) / RecordingEntries::kLodMinutes;
    lod.offsets.reserve(count + 1); lod.coverage.reserve(count); lod.observedMs.reserve(count);
    lod.rowSide.reserve(out.rowSide.size() / RecordingEntries::kLodMinutes + 1024);
    lod.weightedSize.reserve(lod.rowSide.capacity());
    struct Cursor { uint32_t packed, index, end, column; };
    auto later = [](const Cursor &a, const Cursor &b) { return rowKey(a.packed) > rowKey(b.packed); };
    std::vector<Cursor> storage;
    storage.reserve(RecordingEntries::kLodMinutes);
    for (uint32_t g = 0; g < count; ++g) {
        const uint32_t first = g * RecordingEntries::kLodMinutes;
        const uint32_t end = std::min(first + RecordingEntries::kLodMinutes, out.columns());
        lod.offsets.push_back(uint32_t(lod.rowSide.size()));
        RecordingEntries::Coverage cov;
        bool haveCoverage = false;
        uint32_t duration = 0;
        std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> queue(later, std::move(storage));
        for (uint32_t c = first; c < end; ++c) {
            if (out.observedMs[c]) {
                const auto &v = out.coverage[c];
                if (!haveCoverage) { cov = v; haveCoverage = true; }
                else {
                    cov.bidLo = std::max(cov.bidLo, v.bidLo); cov.bidHi = std::min(cov.bidHi, v.bidHi);
                    cov.askLo = std::max(cov.askLo, v.askLo); cov.askHi = std::min(cov.askHi, v.askHi);
                }
            }
            duration += out.observedMs[c];
            if (out.offsets[c] < out.offsets[c + 1])
                queue.push({out.rowSide[out.offsets[c]], out.offsets[c], out.offsets[c + 1], c});
        }
        uint32_t current = 0;
        double weighted = 0;
        bool hasCurrent = false;
        while (!queue.empty()) {
            auto item = queue.top(); queue.pop();
            if (hasCurrent && item.packed != current) {
                lod.rowSide.push_back(current); lod.weightedSize.push_back(float(weighted)); weighted = 0;
            }
            current = item.packed; hasCurrent = true;
            weighted += decodeSize(uint16_t(out.code[item.index]), out.columnScale[item.column]) * out.observedMs[item.column];
            if (++item.index < item.end) {
                item.packed = out.rowSide[item.index]; queue.push(item);
            }
        }
        if (hasCurrent) { lod.rowSide.push_back(current); lod.weightedSize.push_back(float(weighted)); }
        lod.coverage.push_back(cov); lod.observedMs.push_back(duration);
    }
    lod.offsets.push_back(uint32_t(lod.rowSide.size()));
}

struct DenseColumn {
    int64_t base = 0;
    uint32_t group = 0;
    std::vector<std::array<double, 2>> sums;
    DenseColumn(const RecordingEntries &data, const std::vector<uint32_t> &rows,
                uint32_t start, uint32_t end, uint32_t nativeRows) : group(nativeRows) {
        if (start == end) return;
        base = floorDiv(data.baseRow + (rows[start] & 0x7fffffffu), group);
        const int64_t last = floorDiv(data.baseRow + (rows[end - 1] & 0x7fffffffu), group);
        if (base < std::numeric_limits<int32_t>::min() || last > std::numeric_limits<int32_t>::max() ||
            last - base > 1'000'000) throw std::runtime_error("dense price LOD range exceeds GPU index width");
        sums.resize(size_t(last - base + 1), {0, 0});
    }
    void add(const RecordingEntries &data, uint32_t rowSide, double size) {
        const auto index = floorDiv(data.baseRow + (rowSide & 0x7fffffffu), group) - base;
        sums[size_t(index)][rowSide >> 31] += size;
    }
    void append(RecordingEntries::DensePriceLod &dst) const {
        if (dst.sums.size() + sums.size() > uint64_t(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("dense price LOD exceeds GPU index width");
        dst.meta.push_back({int32_t(dst.sums.size()), int32_t(base)});
        for (const auto &pair : sums) dst.sums.push_back({float(pair[0]), float(pair[1])});
    }
};

void buildPriceLod(RecordingEntries &out) {
    auto &lod = out.priceLod;
    lod.offsets.reserve(out.columns() + 1);
    lod.rowSide.reserve(out.rowSide.size() / RecordingEntries::kPriceBlockRows + 1024);
    lod.size.reserve(lod.rowSide.capacity());
    out.dense10.meta.reserve(out.columns() + 1); out.dense40.meta.reserve(out.columns() + 1);
    out.dense100.meta.reserve(out.columns() + 1);
    for (uint32_t c = 0; c < out.columns(); ++c) {
        lod.offsets.push_back(uint32_t(lod.rowSide.size()));
        DenseColumn dense40(out, out.rowSide, out.offsets[c], out.offsets[c + 1], 40);
        DenseColumn dense10(out, out.rowSide, out.offsets[c], out.offsets[c + 1], 10);
        DenseColumn dense100(out, out.rowSide, out.offsets[c], out.offsets[c + 1], 100);
        uint32_t current = 0;
        double bid = 0, ask = 0;
        bool seen = false;
        auto flush = [&] {
            if (!seen) return;
            if (bid > 0) { lod.rowSide.push_back(current); lod.size.push_back(float(bid)); }
            if (ask > 0) { lod.rowSide.push_back(current | 0x80000000u); lod.size.push_back(float(ask)); }
        };
        for (uint32_t i = out.offsets[c]; i < out.offsets[c + 1]; ++i) {
            const uint32_t block = (out.rowSide[i] & 0x7fffffffu) / RecordingEntries::kPriceBlockRows;
            if (seen && block != current) { flush(); bid = ask = 0; }
            current = block; seen = true;
            const double size = decodeSize(uint16_t(out.code[i]), out.columnScale[c]);
            (out.rowSide[i] & 0x80000000u ? ask : bid) += size;
            dense40.add(out, out.rowSide[i], size);
            dense10.add(out, out.rowSide[i], size);
            dense100.add(out, out.rowSide[i], size);
        }
        flush();
        dense10.append(out.dense10); dense40.append(out.dense40); dense100.append(out.dense100);
    }
    lod.offsets.push_back(uint32_t(lod.rowSide.size()));
    out.dense40.meta.push_back({int32_t(out.dense40.sums.size()), 0});
    out.dense10.meta.push_back({int32_t(out.dense10.sums.size()), 0});
    out.dense100.meta.push_back({int32_t(out.dense100.sums.size()), 0});
}

void buildTimePriceLod(RecordingEntries &out) {
    auto &lod = out.timePriceLod;
    const auto &source = out.lod;
    lod.offsets.reserve(source.coverage.size() + 1);
    lod.rowSide.reserve(source.rowSide.size() / RecordingEntries::kPriceBlockRows + 1024);
    lod.size.reserve(lod.rowSide.capacity());
    out.timeDense40.meta.reserve(source.coverage.size() + 1);
    out.timeDense10.meta.reserve(source.coverage.size() + 1);
    out.timeDense100.meta.reserve(source.coverage.size() + 1);
    for (uint32_t c = 0; c < source.coverage.size(); ++c) {
        lod.offsets.push_back(uint32_t(lod.rowSide.size()));
        DenseColumn dense40(out, source.rowSide, source.offsets[c], source.offsets[c + 1], 40);
        DenseColumn dense10(out, source.rowSide, source.offsets[c], source.offsets[c + 1], 10);
        DenseColumn dense100(out, source.rowSide, source.offsets[c], source.offsets[c + 1], 100);
        uint32_t current = 0;
        double bid = 0, ask = 0;
        bool seen = false;
        auto flush = [&] {
            if (!seen) return;
            if (bid > 0) { lod.rowSide.push_back(current); lod.size.push_back(float(bid)); }
            if (ask > 0) { lod.rowSide.push_back(current | 0x80000000u); lod.size.push_back(float(ask)); }
        };
        for (uint32_t i = source.offsets[c]; i < source.offsets[c + 1]; ++i) {
            const uint32_t block = (source.rowSide[i] & 0x7fffffffu) / RecordingEntries::kPriceBlockRows;
            if (seen && block != current) { flush(); bid = ask = 0; }
            current = block; seen = true;
            const double size = source.weightedSize[i];
            (source.rowSide[i] & 0x80000000u ? ask : bid) += size;
            dense40.add(out, source.rowSide[i], size);
            dense10.add(out, source.rowSide[i], size);
            dense100.add(out, source.rowSide[i], size);
        }
        flush();
        dense10.append(out.timeDense10); dense40.append(out.timeDense40); dense100.append(out.timeDense100);
    }
    lod.offsets.push_back(uint32_t(lod.rowSide.size()));
    out.timeDense40.meta.push_back({int32_t(out.timeDense40.sums.size()), 0});
    out.timeDense10.meta.push_back({int32_t(out.timeDense10.sums.size()), 0});
    out.timeDense100.meta.push_back({int32_t(out.timeDense100.sums.size()), 0});
}

struct ScanChunk {
    std::vector<Entry> entries;
    std::vector<std::array<int64_t, 4>> bounds;
    std::vector<int64_t> nativeUnits;
    std::vector<uint32_t> observedMs;
    std::vector<SizeScale> columnScale;
    double priceScale = 0, decodeMs = 0;
    SizeScale sizeScale;
    int64_t commonUnits = 0;
};

ScanChunk scanChunk(const std::filesystem::path &root, const std::string &symbol,
                    const std::string &layer, int64_t rangeStart, int64_t chunkStart,
                    int64_t chunkEnd, uint32_t columns) {
    ScanChunk chunk;
    chunk.bounds.resize(columns, {1, 0, 1, 0});
    chunk.nativeUnits.resize(columns);
    chunk.observedMs.resize(columns);
    chunk.columnScale.resize(columns);
    std::string failure;
    Hmc2Reader reader(root); // reader/index/cache belongs to this worker
    ReadControl control;
    const auto scan = reader.visit(symbol, layer, minute, chunkStart, chunkEnd, [&](const Hmc2Record &r) {
        if (!failure.empty()) return;
        const auto decoded = Clock::now();
        const double tick = r.header.rowTickUnits / r.header.priceScale;
        if (!std::isfinite(tick) || tick <= 0 || !std::isfinite(r.header.priceScale) ||
            r.header.priceScale <= 0) { failure = "bad recording native grid"; return; }
        if (!chunk.commonUnits) chunk.priceScale = r.header.priceScale;
        else if (r.header.priceScale != chunk.priceScale) { failure = "mixed price scales in recording range"; return; }
        chunk.sizeScale = r.header.sizeScale;
        const int64_t units = r.header.rowTickUnits;
        chunk.commonUnits = chunk.commonUnits ? std::gcd(chunk.commonUnits, units) : units;
        const auto col = static_cast<uint32_t>((r.bucketStartMs - rangeStart) / minute);
        if (col >= columns || !r.observedMs) return;
        chunk.bounds[col] = {r.bidRowLo, r.bidRowHi, r.askRowLo, r.askRowHi};
        chunk.nativeUnits[col] = units;
        chunk.observedMs[col] = r.observedMs;
        chunk.columnScale[col] = r.header.sizeScale;
        for (const auto &e : r.entries) {
            const auto lo = e.isAsk ? r.askRowLo : r.bidRowLo;
            const auto hi = e.isAsk ? r.askRowHi : r.bidRowHi;
            if (e.row < lo || e.row > hi) continue;
            const double decodedSize = decodeSize(e.twapCode, r.header.sizeScale);
            if (!std::isfinite(decodedSize) || decodedSize < 0 ||
                decodedSize > std::numeric_limits<float>::max()) {
                failure = "nonfinite recording size"; return;
            }
            if (decodedSize > 0) {
                if (e.row > std::numeric_limits<int64_t>::max() / units ||
                    e.row < std::numeric_limits<int64_t>::min() / units) {
                    failure = "native price unit overflow"; return;
                }
                chunk.entries.push_back({col, e.row * units, uint32_t(e.isAsk), uint16_t(e.twapCode & kMaxCode)});
            }
        }
        chunk.decodeMs += elapsed(decoded);
    }, control);
    if (!failure.empty()) throw std::runtime_error(failure);
    if (scan.status != ReadStatus::Complete)
        throw std::runtime_error("recording reader did not complete range scan");
    return chunk;
}
} // namespace

RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs) {
    if ((layer != "near" && layer != "deep") || symbol.empty() || startMs >= endMs ||
        startMs < kHmc2MinMs || endMs > kHmc2EndMs || startMs % minute || endMs % minute ||
        (endMs - startMs) / minute > 100'000)
        throw std::invalid_argument("invalid recording entry range");
    const auto started = Clock::now();
    RecordingEntries out;
    out.startMs = startMs;
    out.coverage.resize(static_cast<size_t>((endMs - startMs) / minute));
    out.observedMs.resize(out.columns());
    std::vector<std::array<int64_t, 4>> bounds(out.columns(), {1, 0, 1, 0});
    std::vector<int64_t> nativeUnits(out.columns(), 0);
    std::vector<SizeScale> columnScales(out.columns());
    std::vector<Entry> entries;
    int64_t commonUnits = 0;
    const uint32_t totalMinutes = out.columns();
    const uint32_t workers = std::min<uint32_t>(4, std::max<uint32_t>(1, totalMinutes / 360));
    const uint32_t chunkMinutes = (totalMinutes + workers - 1) / workers;
    std::vector<std::future<ScanChunk>> tasks;
    tasks.reserve(workers);
    for (uint32_t worker = 0; worker < workers; ++worker) {
        const auto from = startMs + int64_t(worker * chunkMinutes) * minute;
        const auto to = std::min(endMs, from + int64_t(chunkMinutes) * minute);
        if (from >= to) break;
        tasks.push_back(std::async(std::launch::async, [&, from, to] {
            return scanChunk(root, symbol, layer, startMs, from, to, totalMinutes);
        }));
    }
    bool selectedScale = false;
    for (auto &task : tasks) {
        auto chunk = task.get();
        if (chunk.commonUnits) {
            if (!selectedScale) { out.priceScale = chunk.priceScale; selectedScale = true; }
            else if (out.priceScale != chunk.priceScale) throw std::runtime_error("mixed price scales in recording range");
            out.sizeScale = chunk.sizeScale;
            commonUnits = commonUnits ? std::gcd(commonUnits, chunk.commonUnits) : chunk.commonUnits;
        }
        out.decodeMs += chunk.decodeMs;
        for (uint32_t c = 0; c < totalMinutes; ++c) if (chunk.nativeUnits[c]) {
            bounds[c] = chunk.bounds[c]; nativeUnits[c] = chunk.nativeUnits[c];
            out.observedMs[c] = chunk.observedMs[c];
            columnScales[c] = chunk.columnScale[c];
        }
        entries.insert(entries.end(), std::make_move_iterator(chunk.entries.begin()),
                       std::make_move_iterator(chunk.entries.end()));
    }
    if (commonUnits) out.nativeTick = commonUnits / out.priceScale;
    finish(out, entries, bounds, nativeUnits, columnScales, commonUnits ? commonUnits : 1);
    buildLod(out);
    buildPriceLod(out);
    buildTimePriceLod(out);
    out.loadMs = elapsed(started);
    return out;
}

RecordingEntries joinRecordingEntries(const RecordingEntries &older, const RecordingEntries &recent) {
    const auto started = Clock::now();
    if (older.startMs + int64_t(older.columns()) * minute != recent.startMs ||
        older.columns() + uint64_t(recent.columns()) > 100'000 ||
        (older.nativeTick > 0 && recent.nativeTick > 0 && older.priceScale != recent.priceScale))
        throw std::invalid_argument("recording entry ranges are not adjacent compatible layers");
    RecordingEntries out;
    out.startMs = older.startMs;
    out.priceScale = recent.nativeTick > 0 ? recent.priceScale : older.priceScale;
    out.sizeScale = recent.nativeTick > 0 ? recent.sizeScale : older.sizeScale;
    auto units = [&](const RecordingEntries &source) -> int64_t {
        if (!(source.nativeTick > 0)) return 0;
        const double value = source.nativeTick * out.priceScale;
        if (!std::isfinite(value) || value < 1 || value > double(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("recording common-grid units out of range");
        return std::llround(value);
    };
    const int64_t olderUnits = units(older), recentUnits = units(recent);
    const int64_t commonUnits = olderUnits && recentUnits ? std::gcd(olderUnits, recentUnits) :
                                (olderUnits ? olderUnits : recentUnits);
    if (!commonUnits) throw std::invalid_argument("both recording entry ranges are empty");
    out.nativeTick = commonUnits / out.priceScale;
    const int64_t olderRatio = olderUnits ? olderUnits / commonUnits : 1;
    const int64_t recentRatio = recentUnits ? recentUnits / commonUnits : 1;
    const auto checkedAdd = [](int64_t a, int64_t b) -> int64_t {
        if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
            throw std::runtime_error("joined recording row addition overflows int64");
        return a + b;
    };
    const auto checkedScale = [](int64_t row, int64_t ratio) -> int64_t {
        if (ratio < 1 || row > std::numeric_limits<int64_t>::max() / ratio ||
            row < std::numeric_limits<int64_t>::min() / ratio)
            throw std::runtime_error("joined recording row scaling overflows int64");
        return row * ratio;
    };
    const auto checkedSub = [](int64_t a, int64_t b) -> int64_t {
        if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
            (b < 0 && a > std::numeric_limits<int64_t>::max() + b))
            throw std::runtime_error("joined recording row subtraction overflows int64");
        return a - b;
    };
    const auto scaledRow = [&](int64_t base, int64_t relative, int64_t ratio) {
        return checkedScale(checkedAdd(base, relative), ratio);
    };
    out.baseRow = olderUnits && recentUnits ?
        std::min(checkedScale(older.baseRow, olderRatio),
                 checkedScale(recent.baseRow, recentRatio)) :
        (olderUnits ? checkedScale(older.baseRow, olderRatio) :
                      checkedScale(recent.baseRow, recentRatio));
    const size_t columns = size_t(older.columns()) + recent.columns();
    out.offsets.resize(columns + 1);
    out.coverage.reserve(columns); out.observedMs.reserve(columns);
    out.nativeFactor.reserve(columns); out.columnScale.reserve(columns);
    out.rowSide.reserve(older.rowSide.size() + recent.rowSide.size());
    out.code.reserve(older.code.size() + recent.code.size());
    uint32_t outputColumn = 0;
    auto append = [&](const RecordingEntries &source, int64_t ratio) {
        if (source.offsets.size() != size_t(source.columns()) + 1 ||
            source.code.size() != source.rowSide.size() ||
            source.observedMs.size() != source.columns() ||
            source.nativeFactor.size() != source.columns() ||
            source.columnScale.size() != source.columns())
            throw std::invalid_argument("invalid source recording entry arrays");
        auto project = [&](int32_t lo, int32_t hi) -> std::pair<int32_t, int32_t> {
            if (lo > hi) return {1, 0};
            const auto first = checkedSub(scaledRow(source.baseRow, lo, ratio), out.baseRow);
            const auto last = checkedSub(checkedSub(scaledRow(source.baseRow, int64_t(hi) + 1, ratio),
                                                   out.baseRow), 1);
            if (first < 0 || last > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("joined recording coverage exceeds GPU row width");
            return {int32_t(first), int32_t(last)};
        };
        for (uint32_t c = 0; c < source.columns(); ++c, ++outputColumn) {
            out.offsets[outputColumn] = uint32_t(out.rowSide.size());
            const auto &cov = source.coverage[c];
            const auto bid = project(cov.bidLo, cov.bidHi), ask = project(cov.askLo, cov.askHi);
            out.coverage.push_back({bid.first, bid.second, ask.first, ask.second});
            out.observedMs.push_back(source.observedMs[c]);
            const uint64_t factor = uint64_t(source.nativeFactor[c]) * ratio;
            if (factor > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("joined recording native factor exceeds GPU width");
            out.nativeFactor.push_back(uint32_t(factor));
            out.columnScale.push_back(source.columnScale[c]);
            for (uint32_t i = source.offsets[c]; i < source.offsets[c + 1]; ++i) {
                const auto row = checkedSub(scaledRow(source.baseRow, source.rowSide[i] & 0x7fffffffu, ratio),
                                            out.baseRow);
                if (row < 0 || row > 0x7fffffffu)
                    throw std::runtime_error("joined recording entry exceeds GPU row width");
                out.rowSide.push_back(uint32_t(row) | (source.rowSide[i] & 0x80000000u));
                out.code.push_back(source.code[i]);
            }
        }
    };
    append(older, olderRatio);
    append(recent, recentRatio);
    out.offsets.back() = uint32_t(out.rowSide.size());
    buildLod(out); buildPriceLod(out); buildTimePriceLod(out);
    out.decodeMs = older.decodeMs + recent.decodeMs;
    out.loadMs = older.loadMs + recent.loadMs + elapsed(started);
    return out;
}

RecordingEntries syntheticRecordingEntries(uint32_t count) {
    const auto started = Clock::now();
    if (!count || count > 100'000'000) throw std::invalid_argument("synthetic entry count out of range");
    RecordingEntries out;
    out.startMs = kHmc2MinMs;
    out.nativeTick = 1; out.priceScale = 100;
    constexpr uint32_t cols = 1000;
    const uint32_t rows = (count + cols - 1) / cols;
    out.coverage.resize(cols, {0, int32_t(rows - 1), 0, int32_t(rows - 1)});
    out.observedMs.resize(cols, 60'000);
    out.nativeFactor.resize(cols, 1);
    out.columnScale.resize(cols, out.sizeScale);
    out.offsets.resize(cols + 1);
    out.rowSide.reserve(count); out.code.reserve(count);
    for (uint32_t c = 0; c < cols; ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.rowSide.size());
        for (uint32_t r = 0; r < rows && out.rowSide.size() < count; ++r) {
            out.rowSide.push_back(r | ((r > rows / 2 ? 1u : 0u) << 31));
            const bool wall = r == rows / 3 || r == rows * 2 / 3 || r == rows / 2 + 41;
            out.code.push_back(encodeSize(wall ? 116'000.0 : 0.01 + ((r * 17u + c * 13u) % 100u) * 0.003,
                                          out.sizeScale));
        }
    }
    out.offsets.back() = count;
    buildLod(out);
    buildPriceLod(out);
    buildTimePriceLod(out);
    out.loadMs = elapsed(started);
    return out;
}

BinCell binRecordingCell(const RecordingEntries &data, uint32_t first, uint32_t end,
                         uint32_t rowLo, uint32_t rowHi, bool useLod, bool usePriceLod) {
    if (first >= end || end > data.columns() || rowLo > rowHi) return {};
    double bid = 0, ask = 0, duration = 0;
    bool valid = true;
    auto visit = [&](uint32_t column, uint32_t start, uint32_t stop, const std::vector<uint32_t> &rowSide,
                     const RecordingEntries::Coverage &cov,
                     uint32_t ms, bool coarse) {
        if (!ms) return;
        valid &= cov.bidLo <= int64_t(rowLo) && cov.bidHi >= int64_t(rowHi) &&
                 cov.askLo <= int64_t(rowLo) && cov.askHi >= int64_t(rowHi);
        duration += ms;
        const bool spatial = usePriceLod && rowHi - rowLo + 1 >= RecordingEntries::kPriceBlockRows;
        const uint32_t blockRows = RecordingEntries::kPriceBlockRows;
        const uint32_t fullLo = (rowLo + blockRows - 1) / blockRows, fullEnd = (rowHi + 1) / blockRows;
        for (uint32_t i = start; i < stop; ++i) {
            const auto row = rowSide[i] & 0x7fffffffu;
            if (row < rowLo || row > rowHi) continue;
            if (spatial && row / blockRows >= fullLo && row / blockRows < fullEnd) continue;
            const double amount = coarse ? data.lod.weightedSize[i] :
                decodeSize(uint16_t(data.code[i]), data.columnScale[column]) * ms;
            (rowSide[i] & 0x80000000u ? ask : bid) += amount;
        }
        const auto &spatialSource = coarse ? data.timePriceLod : data.priceLod;
        if (spatial) for (uint32_t i = spatialSource.offsets[column]; i < spatialSource.offsets[column + 1]; ++i) {
            const uint32_t block = spatialSource.rowSide[i] & 0x7fffffffu;
            if (block < fullLo || block >= fullEnd) continue;
            (spatialSource.rowSide[i] & 0x80000000u ? ask : bid) += spatialSource.size[i] * (coarse ? 1.0 : ms);
        }
    };
    for (uint32_t c = first; c < end;) {
        if (useLod && c % RecordingEntries::kLodMinutes == 0 &&
            c + RecordingEntries::kLodMinutes <= end) {
            const uint32_t g = c / RecordingEntries::kLodMinutes;
            visit(g, data.lod.offsets[g], data.lod.offsets[g + 1], data.lod.rowSide,
                  data.lod.coverage[g], data.lod.observedMs[g], true);
            c += RecordingEntries::kLodMinutes;
        } else {
            visit(c, data.offsets[c], data.offsets[c + 1], data.rowSide,
                  data.coverage[c], data.observedMs[c], false);
            ++c;
        }
    }
    if (!duration) return {};
    const uint32_t group = rowHi - rowLo + 1;
    for (uint32_t c = first; c < end; ++c) {
        if (!data.observedMs[c]) continue;
        const uint32_t factor = data.nativeFactor[c];
        if (!factor || group % factor || (data.baseRow + rowLo) % factor) return {};
    }
    if (!valid && end - first > 1) {
        bid = ask = 0;
        for (uint32_t c = first; c < end; ++c) {
            if (!data.observedMs[c]) continue;
            for (uint32_t i = data.offsets[c]; i < data.offsets[c + 1]; ++i) {
                const uint32_t row = data.rowSide[i] & 0x7fffffffu;
                if (row < rowLo || row > rowHi) continue;
                const bool isAsk = (data.rowSide[i] & 0x80000000u) != 0;
                double covered = 0, gridMs = 0;
                for (uint32_t d = first; d < end; ++d) {
                    if (!data.observedMs[d] || data.nativeFactor[d] != data.nativeFactor[c]) continue;
                    gridMs += data.observedMs[d];
                    const auto &cov = data.coverage[d];
                    if (isAsk ? (cov.askLo <= int64_t(row) && cov.askHi >= int64_t(row)) :
                                (cov.bidLo <= int64_t(row) && cov.bidHi >= int64_t(row))) covered += data.observedMs[d];
                }
                if (covered > 0)
                    (isAsk ? ask : bid) += decodeSize(uint16_t(data.code[i]), data.columnScale[c]) *
                        data.observedMs[c] / covered * gridMs;
            }
        }
    }
    return {float(bid / duration), float(ask / duration), valid};
}
} // namespace recording
