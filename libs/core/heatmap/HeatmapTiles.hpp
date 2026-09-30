#pragma once
// Whole-chunk render-ready tiles (slice B1, mode W) and the chunk planning both
// B1 preparation modes share. Pure functions and value types; any thread; no Qt.
//
// A tile is kTileColumns columns of the selected timeframe, epoch aligned (tile
// x covers buckets [64x, 64x + 64), MarketLens-style), binned over the WHOLE
// useful price extent of that span at the selected tick: every display bin that
// holds a coverage run or an entry of some column, plus one sentinel row above
// and below that carries each column's "outside the book" state (veil for a
// present column, loading, no data). Bins are anchored to the absolute price
// lattice: bin b is [b * tick, (b + 1) * tick) (spec rule 3), so a tile's rows
// never depend on the view, on neighbouring tiles or on load order.
//
// Cells use the GPU binner's output word: bits 0..14 log code, bit 15 ask,
// bits 16..17 state (0 no data, 1 loading, 2 veil, 3 valid). Row 0 is the top.
#include "ChunkStore.hpp"
#include "../servermodel/RecordingCodec.hpp"
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <unordered_map>

namespace heatmap::tiles {

inline constexpr int64_t kTileColumns = 64;
// A tile keeps its whole useful extent; the renderer splits it into row blocks
// of at most kTileBlockRows (the GPU binner's grid limit is 16384 rows). The
// deep BTC book spans about $21k-$335k, i.e. 62.8k rows at $5.
inline constexpr uint32_t kTileBlockRows = 8192;
inline constexpr uint32_t kMaxTileRows = 1u << 18; // safety cap only
enum CellState : uint32_t { kCellNoData = 0, kCellLoading = 1, kCellVeil = 2, kCellValid = 3 };
inline uint32_t cellWord(uint32_t state, uint16_t code = 0, bool ask = false) {
    return (state << 16) | (ask ? 0x8000u : 0u) | (code & 0x7fffu);
}
inline uint32_t cellState(uint32_t word) { return (word >> 16) & 3u; }

// ------------------------------------------------------------------ time
inline int64_t tileOfBucket(int64_t bucket) { return recording::floorDiv(bucket, kTileColumns); }
inline int64_t tileFirstBucket(int64_t tile) { return tile * kTileColumns; }
inline int64_t tileStartMs(int64_t tile, int64_t tfMs) { return tileFirstBucket(tile) * tfMs; }
inline int64_t tileEndMs(int64_t tile, int64_t tfMs) { return tileFirstBucket(tile + 1) * tfMs; }
struct TileRange {
    int64_t first = 0, end = 0; // tiles [first, end)
    bool contains(int64_t tile) const { return tile >= first && tile < end; }
    int64_t count() const { return end > first ? end - first : 0; }
};
// Tiles the half-open time range touches, widened by `marginTiles` on each side.
TileRange tilesCovering(double timeLoMs, double timeHiMs, int64_t tfMs, int64_t marginTiles = 0);

// ------------------------------------------------------------------ chunks
struct Availability {
    int64_t oldestMs = 0;    // oldest recorded time of the source (any level)
    int64_t endMs = 0;       // exclusive end of the newest recorded minute
    int64_t hourThroughMs = 0; // hour rollups exist before this (exclusive; 0 = none)
    // Per-level lower bounds (0 = unknown: oldestMs, and hour chunks from the
    // start of the range, the lab's single-interval behaviour).
    int64_t minuteOldestMs = 0, hourOldestMs = 0;
};
// Native chunk keys needed to compose [startMs, endMs) at tfMs, restricted to the
// available range. `source` is a chunk source id (ChunkCodec kChunkSources). For a
// source with an hour level, hour-multiple timeframes use hour chunks (one UTC day)
// only inside the advertised hour interval [hourOldestMs, hourThroughMs) and minute
// chunks (one UTC hour) before and after it (the open tail). An hour chunk scans
// its whole day, so its hours before hourOldestMs read as recorder gaps, and gaps
// of a coarser level supersede minutes: the day holding hourOldestMs therefore
// uses minutes when minute history starts at or before hourOldestMs. Every other timeframe composes
// minutes. Ascending.
std::vector<ChunkKey> chunksFor(const std::string &symbol, const std::string &source, int64_t tfMs,
                                int64_t startMs, int64_t endMs, const Availability &availability);
// Composes [startMs, endMs) (multiples of tfMs) from stored chunks in any order.
// No chunks: an empty source with nothing scanned (every bucket NotLoaded).
SparseColumns composeChunks(std::span<const std::shared_ptr<const StoredChunk>> chunks, const std::string &symbol,
                            const std::string &layer, int64_t tfMs, int64_t startMs, int64_t endMs);
// Order-independent identity of the chunk versions a derived object was built from.
uint64_t combineGenerations(std::span<const std::shared_ptr<const StoredChunk>> chunks);
// LCM of the native tick units of every column in [startMs, endMs) (0 if none).
// Auto tick input computed from native chunks: the same set of ticks as the
// composed columns touching the range (commonTickInView).
int64_t commonUnitsIn(std::span<const std::shared_ptr<const StoredChunk>> chunks, double startMs, double endMs,
                      int64_t tfMs);

// ------------------------------------------------------------------ price
struct BinExtent {
    int64_t firstBin = 0, endBin = 0; // absolute display bins [firstBin, endBin)
    bool empty() const { return endBin <= firstBin; }
    int64_t rows() const { return endBin - firstBin; }
};
// Useful price extent in integer price units [lo, end): every coverage run and
// entry of the columns in [startMs, endMs). Tick independent, so it is computed
// once per composed span. (Every native grid of BTC uses one price scale; a grid
// with another scale is converted exactly when possible, else skipped.)
struct UnitExtent {
    int64_t lo = 0, end = 0;
    bool empty() const { return end <= lo; }
};
UnitExtent usefulUnits(const SparseColumns &columns, double priceScale, int64_t startMs = INT64_MIN,
                       int64_t endMs = INT64_MAX);
// Absolute display bins of tickUnits that intersect the extent.
BinExtent binsOf(const UnitExtent &units, int64_t tickUnits);
inline BinExtent usefulExtent(const SparseColumns &columns, int64_t tickUnits, double priceScale,
                              int64_t startMs = INT64_MIN, int64_t endMs = INT64_MAX) {
    return binsOf(usefulUnits(columns, priceScale, startMs, endMs), tickUnits);
}

struct TileGrid {
    int64_t tfMs = 0, tile = 0;
    int64_t firstBucket = 0;
    uint32_t columns = kTileColumns;
    int64_t tickUnits = 0;
    double tick = 0, priceScale = 100;
    int64_t firstBin = 0; // bottom row (a sentinel row)
    uint32_t rows = 0;    // useful extent + 2 sentinel rows
    bool operator==(const TileGrid &) const = default;
    uint64_t cellBytes() const { return uint64_t(columns) * rows * 4; }
};
// nullopt when the extent is empty; `clipped` is set when it exceeded maxRows
// (the tile then keeps the maxRows - 2 bins nearest `centerBin`).
std::optional<TileGrid> tileGrid(int64_t tile, int64_t tfMs, int64_t tickUnits, double priceScale,
                                 const BinExtent &extent, int64_t centerBin, bool *clipped = nullptr,
                                 uint32_t maxRows = kMaxTileRows);

struct CellOptions {
    int64_t availableStartMs = INT64_MIN, availableEndMs = INT64_MAX;
    recording::SizeScale outputScale{};
};
// CPU render-ready cells for one tile (MarketLens-style CPU aggregation), with
// exactly the states and codes of the GPU kernel (heatmap_bin.comp) for the
// same grid: binColumn per present column; incompatible native grids veil the
// column; unscanned buckets draw loading; outside availability draws no data.
std::vector<uint32_t> buildCells(const SparseColumns &composed, const TileGrid &grid, const CellOptions &options);
// The fill-pass rule of the GPU kernel (S5 owner decision 1): the coarsest source
// that builds the tick is binned first; each finer source then replaces only the
// cells still veiled, and only with valid ones. `fill` holds the finer source's
// cells on the same grid (buildCells). Test oracle for heatmap_bin.comp's fill pass.
void fillVeiled(std::vector<uint32_t> &cells, const std::vector<uint32_t> &fill);

// ------------------------------------------------------------------ caches
struct TileKey {
    std::string symbol, source;
    int64_t tfMs = 0, tickUnits = 0, tile = 0;
    uint64_t sourceGeneration = 0; // combineGenerations of the chunks it was built from
    bool operator==(const TileKey &) const = default;
};
struct TileKeyHash {
    size_t operator()(const TileKey &key) const;
};

// Byte-bounded LRU. find() touches; evict() drops the least recent entries that
// `keep` does not protect (visible tiles, a tile still drawn as a fallback)
// until the budget holds, so the working set of the current view is never
// evicted by prefetch. Not thread-safe (one owner).
template <class Key, class Value, class Hash = std::hash<Key>>
class ByteLru {
public:
    explicit ByteLru(size_t maxBytes) : maxBytes_(maxBytes) {}
    Value *find(const Key &key) {
        const auto it = map_.find(key);
        if (it == map_.end()) { ++misses_; return nullptr; }
        ++hits_;
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->value;
    }
    const Value *peek(const Key &key) const {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second->value;
    }
    Value &insert(const Key &key, Value value, size_t bytes) {
        erase(key);
        order_.push_front({key, std::move(value), bytes});
        map_[key] = order_.begin();
        bytes_ += bytes;
        return order_.front().value;
    }
    bool erase(const Key &key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return false;
        bytes_ -= it->second->bytes;
        order_.erase(it->second);
        map_.erase(it);
        return true;
    }
    std::vector<Key> evict(const std::function<bool(const Key &)> &keep = {}) {
        std::vector<Key> evicted;
        for (auto it = order_.end(); bytes_ > maxBytes_ && it != order_.begin();) {
            --it;
            if (keep && keep(it->key)) continue;
            evicted.push_back(it->key);
            bytes_ -= it->bytes;
            map_.erase(it->key);
            it = order_.erase(it);
            ++evictions_;
        }
        return evicted;
    }
    template <class F> void forEach(F &&f) const { for (const auto &e : order_) f(e.key, e.value, e.bytes); }
    template <class F> void forEachMutable(F &&f) { for (auto &e : order_) f(e.key, e.value, e.bytes); }
    void clear() { order_.clear(); map_.clear(); bytes_ = 0; }
    size_t bytes() const { return bytes_; }
    size_t size() const { return map_.size(); }
    size_t maxBytes() const { return maxBytes_; }
    void setMaxBytes(size_t bytes) { maxBytes_ = bytes; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t evictions() const { return evictions_; }
    void resetStats() { hits_ = misses_ = evictions_ = 0; }
private:
    struct Node { Key key; Value value; size_t bytes; };
    size_t maxBytes_ = 0, bytes_ = 0;
    uint64_t hits_ = 0, misses_ = 0, evictions_ = 0;
    std::list<Node> order_; // front = most recent
    std::unordered_map<Key, typename std::list<Node>::iterator, Hash> map_;
};
} // namespace heatmap::tiles
