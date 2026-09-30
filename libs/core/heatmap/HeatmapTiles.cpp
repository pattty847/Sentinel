#include "HeatmapTiles.hpp"
#include "BinCell.hpp"
#include "TimeComposer.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace heatmap::tiles {
namespace {
using recording::floorDiv;
// Native row tick in the target price scale's integer units (0 if not integral).
int64_t nativeUnits(const GridIdentity &grid, double priceScale) {
    if (grid.priceScale == priceScale) return grid.rowTickUnits;
    const double units = double(grid.rowTickUnits) * priceScale / grid.priceScale;
    const double rounded = std::round(units);
    return std::abs(units - rounded) < 1e-9 && rounded > 0 ? int64_t(rounded) : 0;
}
} // namespace

TileRange tilesCovering(double timeLoMs, double timeHiMs, int64_t tfMs, int64_t marginTiles) {
    if (tfMs <= 0 || !std::isfinite(timeLoMs) || !std::isfinite(timeHiMs) || !(timeHiMs > timeLoMs)) return {};
    const auto first = int64_t(std::floor(timeLoMs / double(tfMs)));
    const auto end = int64_t(std::ceil(timeHiMs / double(tfMs)));
    return {tileOfBucket(first) - marginTiles, tileOfBucket(end - 1) + 1 + marginTiles};
}

std::vector<ChunkKey> chunksFor(const std::string &symbol, const std::string &source, int64_t tfMs,
                                int64_t startMs, int64_t endMs, const Availability &availability) {
    std::vector<ChunkKey> out;
    const int64_t lo = std::max(startMs, availability.oldestMs), hi = std::min(endMs, availability.endMs);
    if (hi <= lo || tfMs <= 0) return out;
    const int64_t minuteOldest = std::max(availability.oldestMs, availability.minuteOldestMs);
    auto minutes = [&](int64_t from, int64_t to) {
        from = std::max(from, minuteOldest);
        if (to <= from) return;
        for (int64_t s = floorDiv(from, kHourMs) * kHourMs; s < to; s += kHourMs)
            out.push_back({symbol, source, kMinuteMs, s});
    };
    const auto *src = findChunkSource(source);
    const bool hours = src && src->hourLevel && tfMs % kHourMs == 0 && availability.hourThroughMs > 0;
    if (!hours) {
        minutes(lo, hi);
        return out;
    }
    int64_t hourFrom = lo; // legacy: hours from the start of the range
    if (availability.hourOldestMs > 0) {
        const int64_t day = floorDiv(availability.hourOldestMs, kDayMs) * kDayMs;
        // Minutes reach the first hour: they compose that partial day. Otherwise
        // the hour chunk does (minutes cannot supply its hours).
        hourFrom = minuteOldest <= availability.hourOldestMs && day != availability.hourOldestMs ? day + kDayMs : day;
    }
    const int64_t hourLo = std::clamp(hourFrom, lo, hi);
    const int64_t hourHi = std::clamp(availability.hourThroughMs, hourLo, hi);
    minutes(lo, hourLo);
    if (hourHi > hourLo)
        for (int64_t s = floorDiv(hourLo, kDayMs) * kDayMs; s < hourHi; s += kDayMs)
            out.push_back({symbol, source, kHourMs, s});
    minutes(std::max(hourHi, hourLo), hi);
    return out;
}

SparseColumns composeChunks(std::span<const std::shared_ptr<const StoredChunk>> chunks, const std::string &symbol,
                            const std::string &layer, int64_t tfMs, int64_t startMs, int64_t endMs) {
    std::vector<const SparseColumns *> levels;
    levels.reserve(chunks.size());
    for (const auto &chunk : chunks)
        if (chunk && chunk->columns) levels.push_back(chunk->columns.get());
    if (levels.empty()) return SparseColumns{symbol, layer, tfMs, startMs, endMs, {}, {}};
    ComposeOptions options;
    options.startMs = startMs;
    options.endMs = endMs;
    options.trustedInputs = true; // ChunkStore validated each chunk on load
    return compose(std::span<const SparseColumns *const>(levels), tfMs, options);
}

uint64_t combineGenerations(std::span<const std::shared_ptr<const StoredChunk>> chunks) {
    std::vector<uint64_t> generations;
    for (const auto &chunk : chunks) if (chunk) generations.push_back(chunk->generation);
    std::sort(generations.begin(), generations.end());
    uint64_t h = 1469598103934665603ull;
    for (const uint64_t g : generations) {
        h ^= g;
        h *= 1099511628211ull;
    }
    return h;
}

int64_t commonUnitsIn(std::span<const std::shared_ptr<const StoredChunk>> chunks, double startMs, double endMs,
                      int64_t tfMs) {
    if (tfMs <= 0 || !(endMs > startMs)) return 0;
    const double first = std::floor(startMs / double(tfMs)) * double(tfMs);
    const double end = std::ceil(endMs / double(tfMs)) * double(tfMs);
    int64_t lcm = 0;
    for (const auto &chunk : chunks) {
        if (!chunk || !chunk->columns) continue;
        for (const auto &column : chunk->columns->columns) {
            const double bucket = double(floorDiv(column.bucketStartMs, tfMs) * tfMs);
            if (bucket < first || bucket >= end) continue;
            for (const auto &native : column.native) {
                const int64_t units = native.grid.rowTickUnits;
                if (units > 0) lcm = lcm ? std::lcm(lcm, units) : units;
            }
        }
    }
    return lcm;
}

UnitExtent usefulUnits(const SparseColumns &data, double priceScale, int64_t startMs, int64_t endMs) {
    UnitExtent out{INT64_MAX, INT64_MIN};
    for (const auto &column : data.columns) {
        if (column.bucketStartMs < startMs || column.bucketStartMs >= endMs) continue;
        for (const auto &native : column.native) {
            const int64_t units = nativeUnits(native.grid, priceScale);
            if (units <= 0) continue;
            int64_t rowLo = INT64_MAX, rowHi = INT64_MIN;
            for (const auto &runs : native.coverage)
                if (!runs.empty()) {
                    rowLo = std::min(rowLo, runs.front().lo);
                    rowHi = std::max(rowHi, runs.back().hi);
                }
            if (!native.entries.empty()) {
                rowLo = std::min(rowLo, native.baseRow + int64_t(native.entries.front().row()));
                rowHi = std::max(rowHi, native.baseRow + int64_t(native.entries.back().row()));
            }
            if (rowLo > rowHi) continue;
            out.lo = std::min(out.lo, rowLo * units);
            out.end = std::max(out.end, (rowHi + 1) * units);
        }
    }
    return out.empty() ? UnitExtent{} : out;
}

BinExtent binsOf(const UnitExtent &units, int64_t tickUnits) {
    if (units.empty() || tickUnits <= 0) return {};
    return {floorDiv(units.lo, tickUnits), floorDiv(units.end - 1, tickUnits) + 1};
}

std::optional<TileGrid> tileGrid(int64_t tile, int64_t tfMs, int64_t tickUnits, double priceScale,
                                 const BinExtent &extent, int64_t centerBin, bool *clipped, uint32_t maxRows) {
    if (clipped) *clipped = false;
    if (extent.empty() || tickUnits <= 0 || tfMs <= 0 || maxRows < 3) return std::nullopt;
    TileGrid grid;
    grid.tfMs = tfMs;
    grid.tile = tile;
    grid.firstBucket = tileFirstBucket(tile);
    grid.tickUnits = tickUnits;
    grid.priceScale = priceScale;
    grid.tick = double(tickUnits) / priceScale;
    int64_t first = extent.firstBin, count = extent.rows();
    if (count > int64_t(maxRows) - 2) {
        if (clipped) *clipped = true;
        count = int64_t(maxRows) - 2;
        first = std::clamp(centerBin - count / 2, extent.firstBin, extent.endBin - count);
    }
    grid.firstBin = first - 1;
    grid.rows = uint32_t(count + 2);
    return grid;
}

std::vector<uint32_t> buildCells(const SparseColumns &composed, const TileGrid &grid, const CellOptions &options) {
    const int64_t tf = grid.tfMs;
    if (tf <= 0 || composed.tfMs != tf || !grid.rows || !grid.columns || grid.tickUnits <= 0)
        throw std::invalid_argument("invalid heatmap tile grid");
    std::vector<uint32_t> cells(size_t(grid.columns) * grid.rows, cellWord(kCellNoData));
    const int64_t availableFirst = options.availableStartMs == INT64_MIN ? INT64_MIN : floorDiv(options.availableStartMs, tf);
    const int64_t availableEnd = options.availableEndMs == INT64_MAX ? INT64_MAX : floorDiv(options.availableEndMs + tf - 1, tf);
    auto fillColumn = [&](uint32_t x, uint32_t word) {
        for (uint32_t y = 0; y < grid.rows; ++y) cells[size_t(y) * grid.columns + x] = word;
    };
    auto column = composed.columns.begin();
    for (uint32_t x = 0; x < grid.columns; ++x) {
        const int64_t bucket = grid.firstBucket + x;
        if (bucket < availableFirst || bucket >= availableEnd) continue; // no data
        const int64_t bucketMs = bucket * tf;
        const auto state = bucketState(composed, bucketMs);
        if (state == BucketState::NotLoaded) { fillColumn(x, cellWord(kCellLoading)); continue; }
        if (state == BucketState::Gap) { fillColumn(x, cellWord(kCellVeil)); continue; }
        column = std::lower_bound(column, composed.columns.end(), bucketMs,
                                  [](const SparseColumn &c, int64_t ms) { return c.bucketStartMs < ms; });
        // binColumn bins at most 16384 rows per call: slice from the top.
        for (uint32_t top = 0; top < grid.rows; top += kTileBlockRows) {
            const uint32_t count = std::min(kTileBlockRows, grid.rows - top);
            const int64_t endBin = grid.firstBin + int64_t(grid.rows) - int64_t(top);
            const auto binned = binColumn(*column, double(endBin - count) * grid.tick, double(endBin) * grid.tick,
                                          grid.tick, options.outputScale);
            for (uint32_t y = 0; y < count && y < binned.size(); ++y) {
                const auto &cell = binned[y];
                cells[size_t(top + y) * grid.columns + x] = cell.valid ? cellWord(kCellValid, cell.code, cell.dominantAsk)
                                                                       : cellWord(kCellVeil);
            }
        }
    }
    return cells;
}

size_t TileKeyHash::operator()(const TileKey &key) const {
    size_t h = std::hash<std::string>{}(key.symbol);
    auto mix = [&](size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); };
    mix(std::hash<std::string>{}(key.source));
    mix(std::hash<int64_t>{}(key.tfMs));
    mix(std::hash<int64_t>{}(key.tickUnits));
    mix(std::hash<int64_t>{}(key.tile));
    mix(std::hash<uint64_t>{}(key.sourceGeneration));
    return h;
}
void fillVeiled(std::vector<uint32_t> &cells, const std::vector<uint32_t> &fill) {
    for (size_t i = 0; i < cells.size() && i < fill.size(); ++i)
        if (cellState(cells[i]) == kCellVeil && cellState(fill[i]) == kCellValid) cells[i] = fill[i];
}
} // namespace heatmap::tiles
