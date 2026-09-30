// Slice B1: process-wide decoded chunk store, whole-chunk tile math, anchoring,
// CPU render-ready cells and invalidation on revised chunks.
#include "heatmap/BinCell.hpp"
#include "heatmap/ChunkStore.hpp"
#include "heatmap/HeatmapTiles.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace heatmap;
using namespace heatmap::tiles;
constexpr int64_t minute = kMinuteMs, hour = kHourMs, day = kDayMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 40 * day; // UTC-day aligned

// Minute i (absolute from epoch): $10 native grid before minute 90, $5 after;
// minutes with i % 17 == 3 are recorder gaps; sizes deterministic. Rows follow
// a drifting mid so extents differ per column.
SparseColumn makeMinute(int64_t i, int64_t seed = 0) {
    const int64_t tickUnits = i < 90 ? 1000 : 500;
    NativeColumn n;
    n.grid = {uint64_t(i < 90 ? 7 : 8), tickUnits, 100};
    n.observedMs = i % 7 ? minute : 31'000;
    const double tick = tickUnits / 100.0;
    const int64_t mid = int64_t((100'000 + 40 * std::sin(double(i) / 11.0) + i) / tick);
    const int64_t lo = mid - 12, hi = mid + 11;
    n.baseRow = lo;
    n.coverage[0] = {{lo + (i % 5 == 0 ? 2 : 0), hi, n.observedMs}};
    n.coverage[1] = {{lo, hi, n.observedMs}};
    for (int64_t row = lo; row <= hi; ++row) {
        if ((row + i) % 6 == 1) continue; // covered zero rows
        const bool ask = row >= mid;
        const double size = 0.01 * (1 + (row * 13 + i * 7 + seed * 5) % 97);
        n.entries.push_back({packRowSide(row, lo, ask), recording::encodeSize(size, n.sizeScale)});
    }
    return {epoch + i * minute, n.observedMs, 0, {std::move(n)}};
}
// One hour chunk of minutes starting at hour h (absolute from epoch).
SparseColumns makeHourChunk(int64_t h, int64_t seed = 0, int64_t scannedMinutes = 60) {
    SparseColumns out{"BTC-USD", "deep", minute, epoch + h * hour, epoch + (h + 1) * hour, {}, {}};
    if (scannedMinutes > 0) out.scannedRanges = {{out.startMs, out.startMs + scannedMinutes * minute}};
    for (int64_t m = 0; m < scannedMinutes; ++m) {
        const int64_t i = h * 60 + m;
        if (i % 17 == 3) continue;
        out.columns.push_back(makeMinute(i, seed));
    }
    validate(out);
    return out;
}
std::shared_ptr<const StoredChunk> stored(SparseColumns columns, uint64_t generation) {
    auto chunk = std::make_shared<StoredChunk>();
    chunk->key = {"BTC-USD", "hmc2.deep", minute, columns.startMs};
    chunk->generation = generation;
    chunk->columns = std::make_shared<const SparseColumns>(std::move(columns));
    return chunk;
}

// ---------------------------------------------------------------- ByteLru
TEST(HeatmapTileLru, EvictsLeastRecentUnprotectedEntriesWithinTheBudget) {
    ByteLru<int, std::string> lru(100);
    lru.insert(1, "a", 40);
    lru.insert(2, "b", 40);
    lru.insert(3, "c", 40);
    EXPECT_EQ(lru.bytes(), 120u);
    ASSERT_NE(lru.find(1), nullptr); // touch: 2 is now the least recent
    auto evicted = lru.evict();
    ASSERT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], 2);
    EXPECT_EQ(lru.bytes(), 80u);
    // A protected entry is skipped; the next least recent goes instead.
    lru.insert(4, "d", 40);
    evicted = lru.evict([](int key) { return key == 3; });
    ASSERT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], 1);
    // Everything protected: over budget is allowed (the view's working set wins).
    lru.insert(5, "e", 90);
    EXPECT_TRUE(lru.evict([](int) { return true; }).empty());
    EXPECT_GT(lru.bytes(), lru.maxBytes());
    // Replacing a key re-accounts its bytes.
    lru.insert(5, "e2", 10);
    EXPECT_EQ(lru.bytes(), 90u);
    EXPECT_EQ(*lru.peek(5), "e2");
    EXPECT_EQ(lru.hits(), 1u);
    EXPECT_EQ(lru.find(42), nullptr);
    EXPECT_EQ(lru.misses(), 1u);
}

// ---------------------------------------------------------------- ChunkStore
TEST(HeatmapChunkStore, SharesOneDecodePerKeyAndEvictsByBytes) {
    std::atomic<int> loads{0};
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        ++loads;
        return ChunkStore::Loaded{makeHourChunk((key.startMs - epoch) / hour), true, 0};
    });
    const ChunkKey a{"BTC-USD", "hmc2.deep", minute, epoch}, b{"BTC-USD", "hmc2.deep", minute, epoch + hour};
    const auto first = store.get(a);
    const auto again = store.get(a);
    EXPECT_EQ(first, again);
    EXPECT_EQ(loads.load(), 1);
    EXPECT_EQ(store.stats().hits, 1u);
    EXPECT_EQ(store.stats().misses, 1u);
    EXPECT_EQ(store.cached(b), nullptr);
    EXPECT_EQ(store.stats().misses, 1u) << "cached() is inspection only";
    const auto second = store.get(b);
    EXPECT_EQ(store.stats().entries, 2u);
    EXPECT_EQ(store.stats().bytes, first->bytes + second->bytes);
    EXPECT_GT(first->bytes, 24u * 50u * 8u) << "entries dominate the estimate";
    // Budget below both: the least recent (a) goes, the newest stays.
    store.setMaxBytes(second->bytes + 1);
    EXPECT_EQ(store.stats().entries, 1u);
    EXPECT_EQ(store.cached(a), nullptr);
    EXPECT_NE(store.cached(b), nullptr);
    EXPECT_EQ(store.stats().evictions, 1u);
    EXPECT_EQ(first->columns->columns.size(), makeHourChunk(0).columns.size()) << "holders keep evicted versions";
}

// Blocks a loader until the test opens it (deterministic overlap, no sleeps).
struct Gate {
    std::promise<void> opened;
    std::shared_future<void> wait = opened.get_future().share();
    bool isOpen = false;
    void open() {
        if (!isOpen) opened.set_value();
        isOpen = true;
    }
};
// Opens the gate and joins the threads on every exit path: a failed ASSERT
// returns early, and destroying a joinable std::thread calls std::terminate.
struct GateThreads {
    std::shared_ptr<Gate> &gate;
    std::vector<std::thread> &threads;
    ~GateThreads() {
        gate->open();
        for (auto &t : threads)
            if (t.joinable()) t.join();
    }
};
// Spins (bounded) until pred() holds; the store's counters say when every
// caller is waiting on the shared load.
template <class Pred> bool eventually(Pred pred) {
    for (int i = 0; i < 20'000'000 && !pred(); ++i) std::this_thread::yield();
    return pred();
}

TEST(HeatmapChunkStore, ConcurrentChartsWaitOnOneLoadAndShareFailures) {
    std::atomic<int> loads{0};
    std::atomic<bool> fail{false};
    auto gate = std::make_shared<Gate>();
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        ++loads;
        gate->wait.wait();
        if (fail) throw std::runtime_error("disk gone");
        return ChunkStore::Loaded{makeHourChunk((key.startMs - epoch) / hour), true, 0};
    });
    const ChunkKey key{"BTC-USD", "hmc2.deep", minute, epoch + 2 * hour};
    const ChunkKey other{"BTC-USD", "hmc2.deep", minute, epoch + 3 * hour};
    std::atomic<int> errors{0};
    std::vector<std::shared_ptr<const StoredChunk>> results(4);
    std::vector<std::thread> threads;
    const GateThreads guard{gate, threads}; // declared after everything the threads touch
    for (int i = 0; i < 4; ++i) threads.emplace_back([&, i] { results[i] = store.get(key); });
    ASSERT_TRUE(eventually([&] { return store.stats().sharedLoads == 3; })) << "three charts wait on the first";
    gate->open();
    for (auto &t : threads) t.join();
    EXPECT_EQ(loads.load(), 1) << "four charts, one decode";
    for (const auto &r : results) EXPECT_EQ(r, results[0]);

    fail = true;
    gate = std::make_shared<Gate>(); // the loader and the guard read `gate` itself
    threads.clear();
    for (int i = 0; i < 3; ++i)
        threads.emplace_back([&] {
            try { store.get(other); } catch (const std::runtime_error &) { ++errors; }
        });
    ASSERT_TRUE(eventually([&] { return store.stats().sharedLoads == 5; }));
    gate->open();
    for (auto &t : threads) t.join();
    EXPECT_EQ(errors.load(), 3) << "waiters rethrow the owner's failure";
    EXPECT_FALSE(store.contains(other));
}

TEST(HeatmapChunkStore, ASlowLoadNeverReplacesARevisionThatArrivedDuringIt) {
    auto gate = std::make_shared<Gate>();
    std::atomic<int> loads{0};
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        const int n = ++loads;
        if (n == 1) gate->wait.wait(); // the stale read (30 minutes) blocks
        return ChunkStore::Loaded{makeHourChunk((key.startMs - epoch) / hour, 0, n == 1 ? 30 : 50), false, 0};
    });
    const ChunkKey key{"BTC-USD", "hmc2.deep", minute, epoch};
    std::shared_ptr<const StoredChunk> fromGet;
    std::vector<std::thread> slow;
    const GateThreads guard{gate, slow};
    slow.emplace_back([&] { fromGet = store.get(key); });
    ASSERT_TRUE(eventually([&] { return loads.load() == 1; }));
    const auto revised = store.revise(key, {makeHourChunk(0, 0, 45), false, 7});
    gate->open();
    slow[0].join();
    EXPECT_EQ(fromGet, revised) << "the older load returns the newer revision";
    EXPECT_EQ(store.cached(key), revised);
    EXPECT_EQ(store.generationOf(key), revised->generation);
    // A reload started after the in-flight get also wins over it.
    auto gate2 = std::make_shared<Gate>();
    std::atomic<int> loads2{0};
    ChunkStore other(1ull << 30, [&](const ChunkKey &k) {
        const int n = ++loads2;
        if (n == 1) gate2->wait.wait();
        return ChunkStore::Loaded{makeHourChunk((k.startMs - epoch) / hour, 0, n == 1 ? 30 : 55), false, 0};
    });
    std::vector<std::thread> slow2;
    const GateThreads guard2{gate2, slow2};
    slow2.emplace_back([&] { fromGet = other.get(key); });
    ASSERT_TRUE(eventually([&] { return loads2.load() == 1; }));
    const auto reloaded = other.reload(key); // second loader call: 55 minutes
    gate2->open();
    slow2[0].join();
    EXPECT_EQ(fromGet, reloaded);
    EXPECT_EQ(other.cached(key)->columns->scannedRanges.back().endMs, epoch + 55 * minute);
}

TEST(HeatmapChunkStore, LatestGenerationSurvivesEvictionAndSealedReloadsKeepIt) {
    std::atomic<int> loads{0};
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        ++loads;
        const int64_t h = (key.startMs - epoch) / hour;
        return ChunkStore::Loaded{makeHourChunk(h), h != 5, 0}; // hour 5 is the open chunk
    });
    const ChunkKey sealedKey{"BTC-USD", "hmc2.deep", minute, epoch + hour}, openKey{"BTC-USD", "hmc2.deep", minute, epoch + 5 * hour};
    const auto sealed = store.get(sealedKey);
    const auto open = store.get(openKey);
    const auto revised = store.revise(openKey, {makeHourChunk(5, 1), false, 2});
    store.setMaxBytes(1); // evict everything but the newest
    EXPECT_EQ(store.cached(sealedKey), nullptr);
    EXPECT_EQ(store.generationOf(sealedKey), sealed->generation) << "eviction does not forget the version";
    EXPECT_EQ(store.generationOf(openKey), revised->generation);
    EXPECT_NE(store.generationOf(openKey), open->generation) << "a tile built from the first version is stale";
    store.setMaxBytes(1ull << 30);
    EXPECT_EQ(store.get(sealedKey)->generation, sealed->generation) << "sealed content never changes";
    store.setMaxBytes(1);
    store.get(sealedKey); // evicts the open chunk
    ASSERT_EQ(store.cached(openKey), nullptr);
    EXPECT_NE(store.get(openKey)->generation, revised->generation) << "an open chunk re-read from disk is a new version";
}

TEST(HeatmapChunkStore, RevisionListenerHearsEveryRevision) {
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        return ChunkStore::Loaded{makeHourChunk((key.startMs - epoch) / hour), false, 0};
    });
    std::vector<int64_t> heard;
    store.setRevisionListener([&](const ChunkKey &key) { heard.push_back(key.startMs); });
    const ChunkKey key{"BTC-USD", "hmc2.deep", minute, epoch};
    store.get(key);
    EXPECT_TRUE(heard.empty()) << "a first load is not a revision";
    store.reload(key);
    store.revise(key, {makeHourChunk(0, 3), false, 1});
    EXPECT_EQ(heard.size(), 2u);
    EXPECT_EQ(store.revisionCount(), 2u);
}

// Re-review fix: get() re-reading an evicted open chunk stores a new generation;
// a chart that did not ask must hear it. First loads and sealed reloads are silent.
TEST(HeatmapChunkStore, AGetThatReReadsAnEvictedOpenChunkIsARevision) {
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        const int64_t h = (key.startMs - epoch) / hour;
        return ChunkStore::Loaded{makeHourChunk(h), h != 5, 0}; // hour 5 is the open chunk
    });
    std::vector<int64_t> heard;
    store.setRevisionListener([&](const ChunkKey &key) { heard.push_back(key.startMs); });
    const ChunkKey sealedKey{"BTC-USD", "hmc2.deep", minute, epoch + hour}, openKey{"BTC-USD", "hmc2.deep", minute, epoch + 5 * hour};
    const auto open = store.get(openKey);
    store.get(sealedKey);
    EXPECT_TRUE(heard.empty()) << "first loads are not revisions";
    store.setMaxBytes(1);
    ASSERT_EQ(store.cached(openKey), nullptr);
    const auto again = store.get(openKey); // evicts the sealed chunk
    ASSERT_NE(again->generation, open->generation);
    ASSERT_EQ(heard.size(), 1u) << "the new generation of an open chunk is announced";
    EXPECT_EQ(heard[0], openKey.startMs);
    ASSERT_EQ(store.cached(sealedKey), nullptr);
    store.get(sealedKey);
    EXPECT_EQ(heard.size(), 1u) << "a sealed reload keeps its generation and stays silent";
    EXPECT_EQ(store.revisionCount(), 1u);
}

TEST(HeatmapChunkStore, RevisionGetsANewGenerationAndOldHoldersKeepTheirVersion) {
    ChunkStore store(1ull << 30, [&](const ChunkKey &key) {
        return ChunkStore::Loaded{makeHourChunk((key.startMs - epoch) / hour, 0, 30), false, 1};
    });
    const ChunkKey key{"BTC-USD", "hmc2.deep", minute, epoch};
    const auto open = store.get(key);
    EXPECT_FALSE(open->sealed);
    const auto revised = store.revise(key, {makeHourChunk(0, 0, 45), false, 2});
    EXPECT_NE(revised->generation, open->generation);
    EXPECT_EQ(store.generationOf(key), revised->generation);
    EXPECT_EQ(store.stats().revisions, 1u);
    EXPECT_LT(open->columns->columns.size(), revised->columns->columns.size());
    EXPECT_EQ(store.get(key), revised);
    const auto reloaded = store.reload(key);
    EXPECT_NE(reloaded->generation, revised->generation);
}

// ---------------------------------------------------------------- tile math
TEST(HeatmapTiles, TilesAreEpochAlignedSixtyFourColumnSpans) {
    const int64_t tf = 5 * minute;
    EXPECT_EQ(tileOfBucket(0), 0);
    EXPECT_EQ(tileOfBucket(63), 0);
    EXPECT_EQ(tileOfBucket(64), 1);
    EXPECT_EQ(tileOfBucket(-1), -1);
    EXPECT_EQ(tileStartMs(3, tf), 3 * 64 * tf);
    EXPECT_EQ(tileEndMs(3, tf), tileStartMs(4, tf));
    // A view inside one tile touches that tile; the half-open end is exclusive.
    const auto r = tilesCovering(double(tileStartMs(3, tf)) + tf, double(tileEndMs(3, tf)), tf);
    EXPECT_EQ(r.first, 3);
    EXPECT_EQ(r.end, 4);
    const auto margin = tilesCovering(double(tileStartMs(3, tf)), double(tileEndMs(4, tf)) + 1, tf, 1);
    EXPECT_EQ(margin.first, 2);
    EXPECT_EQ(margin.end, 7);
}

TEST(HeatmapTiles, ChunkPlanUsesHoursBeforeTheWatermarkAndMinutesAfter) {
    Availability a{epoch + 30 * minute, epoch + 2 * day + 90 * minute, epoch + 2 * day};
    // Deep 1h: day chunks of hour rollups through the watermark, then minute chunks.
    auto keys = chunksFor("BTC-USD", "hmc2.deep", hour, epoch, epoch + 3 * day, a);
    ASSERT_EQ(keys.size(), 4u);
    EXPECT_EQ(keys[0].levelMs, hour);
    EXPECT_EQ(keys[0].startMs, epoch);
    EXPECT_EQ(keys[1].startMs, epoch + day);
    EXPECT_EQ(keys[2].levelMs, minute);
    EXPECT_EQ(keys[2].startMs, epoch + 2 * day);
    EXPECT_EQ(keys[3].startMs, epoch + 2 * day + hour);
    // Near (no rollups) and sub-hour timeframes compose minutes only; the range
    // is clipped to the available data.
    keys = chunksFor("BTC-USD", "hmc2.near", hour, epoch - day, epoch + 3 * hour, a);
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0].startMs, epoch);
    EXPECT_EQ(keys[0].levelMs, minute);
    keys = chunksFor("BTC-USD", "hmc2.deep", 5 * minute, epoch + 2 * day + 2 * hour, epoch + 3 * day, a);
    EXPECT_TRUE(keys.empty()) << "nothing recorded after availability.endMs";
}

TEST(HeatmapTiles, ClippedCompositionEqualsTheSameBucketsOfTheWholeComposition) {
    const auto c0 = makeHourChunk(0), c1 = makeHourChunk(1);
    const std::vector<std::shared_ptr<const StoredChunk>> chunks{stored(c1, 2), stored(c0, 1)}; // any order
    const int64_t tf = 5 * minute;
    const int64_t start = epoch + 20 * minute, end = epoch + 85 * minute;
    const auto clipped = composeChunks(chunks, "BTC-USD", "deep", tf, start, end);
    validate(clipped);
    const std::vector<SparseColumns> both{c0, c1};
    const auto whole = compose(both, tf);
    ASSERT_EQ(clipped.scannedRanges.size(), 1u);
    EXPECT_EQ(clipped.scannedRanges[0].startMs, start);
    EXPECT_EQ(clipped.scannedRanges[0].endMs, end);
    size_t compared = 0;
    for (const auto &column : whole.columns) {
        const bool inside = column.bucketStartMs >= start && column.bucketStartMs < end;
        const auto it = std::find_if(clipped.columns.begin(), clipped.columns.end(),
                                     [&](const auto &c) { return c.bucketStartMs == column.bucketStartMs; });
        ASSERT_EQ(it != clipped.columns.end(), inside);
        if (!inside) continue;
        ++compared;
        const auto a = binColumn(column, 99'900, 100'200, 10);
        const auto b = binColumn(*it, 99'900, 100'200, 10);
        for (size_t r = 0; r < a.size(); ++r) {
            EXPECT_EQ(a[r].valid, b[r].valid);
            EXPECT_EQ(a[r].code, b[r].code);
        }
    }
    EXPECT_EQ(compared, 13u);
    // No chunks: an unscanned (loading) span, never a gap.
    const auto none = composeChunks({}, "BTC-USD", "deep", tf, start, end);
    EXPECT_EQ(bucketState(none, start), BucketState::NotLoaded);
}

TEST(HeatmapTiles, CommonUnitsAndGenerationsAreOrderIndependent) {
    const auto a = stored(makeHourChunk(1), 11), b = stored(makeHourChunk(2), 12);
    const std::vector<std::shared_ptr<const StoredChunk>> ab{a, b}, ba{b, a};
    EXPECT_EQ(combineGenerations(ab), combineGenerations(ba));
    const auto revised = stored(makeHourChunk(2, 1), 13);
    const std::vector<std::shared_ptr<const StoredChunk>> ar{a, revised};
    EXPECT_NE(combineGenerations(ab), combineGenerations(ar)) << "a revised chunk changes the tile identity";
    // Minutes < 90 are on $10, later on $5: the LCM in view is $10 wherever $10 data is visible.
    EXPECT_EQ(commonUnitsIn(ab, double(epoch + 60 * minute), double(epoch + 80 * minute), minute), 1000);
    EXPECT_EQ(commonUnitsIn(ab, double(epoch + 120 * minute), double(epoch + 140 * minute), minute), 500);
    EXPECT_EQ(commonUnitsIn(ab, double(epoch + 80 * minute), double(epoch + 100 * minute), minute), 1000);
    EXPECT_EQ(commonUnitsIn(ab, double(epoch + 180 * minute), double(epoch + 200 * minute), minute), 0);
}

// ---------------------------------------------------------------- anchoring + cells
TEST(HeatmapTiles, TileRowsAreAbsoluteAndCellsMatchBinColumn) {
    const auto c0 = makeHourChunk(0), c1 = makeHourChunk(1), c2 = makeHourChunk(2);
    const std::vector<std::shared_ptr<const StoredChunk>> all{stored(c0, 1), stored(c1, 2), stored(c2, 3)};
    const int64_t tf = minute;
    const int64_t tile = tileOfBucket((epoch + 70 * minute) / tf);
    const int64_t start = tileStartMs(tile, tf), end = tileEndMs(tile, tf);
    const auto composed = composeChunks(all, "BTC-USD", "deep", tf, start, end);
    CellOptions options; // availability ends inside the tile at both edges: no data there
    options.availableStartMs = start + 5 * minute;
    options.availableEndMs = end - 7 * minute;
    for (const int64_t tickUnits : {1000, 2000, 5000, 750}) {
        const auto extent = usefulExtent(composed, tickUnits, 100);
        ASSERT_FALSE(extent.empty());
        // Every covered price lies inside the extent (absolute bins, floor/ceil).
        const auto units = usefulUnits(composed, 100);
        EXPECT_LE(extent.firstBin * tickUnits, units.lo);
        EXPECT_GE(extent.endBin * tickUnits, units.end);
        EXPECT_GT((extent.firstBin + 1) * tickUnits, units.lo);
        const auto grid = tileGrid(tile, tf, tickUnits, 100, extent, (extent.firstBin + extent.endBin) / 2);
        ASSERT_TRUE(grid);
        EXPECT_EQ(grid->rows, uint32_t(extent.rows() + 2));
        EXPECT_EQ(grid->firstBin, extent.firstBin - 1);
        const auto cells = buildCells(composed, *grid, options);
        ASSERT_EQ(cells.size(), size_t(grid->columns) * grid->rows);
        uint32_t valid = 0, veil = 0, noData = 0;
        for (uint32_t x = 0; x < grid->columns; ++x) {
            const int64_t bucketMs = (grid->firstBucket + x) * tf;
            const bool outside = bucketMs < options.availableStartMs || bucketMs >= options.availableEndMs;
            const auto state = bucketState(composed, bucketMs);
            std::vector<BinCell> expected;
            if (state == BucketState::Present && !outside) {
                const auto it = std::find_if(composed.columns.begin(), composed.columns.end(),
                                             [&](const auto &c) { return c.bucketStartMs == bucketMs; });
                expected = binColumn(*it, double(grid->firstBin) * grid->tick,
                                     double(grid->firstBin + grid->rows) * grid->tick, grid->tick);
            }
            for (uint32_t y = 0; y < grid->rows; ++y) {
                const uint32_t word = cells[size_t(y) * grid->columns + x];
                if (outside) { EXPECT_EQ(cellState(word), kCellNoData); ++noData; continue; }
                if (state == BucketState::Gap) { EXPECT_EQ(cellState(word), kCellVeil); ++veil; continue; }
                ASSERT_EQ(state, BucketState::Present);
                const bool isValid = expected[y].valid;
                EXPECT_EQ(cellState(word), isValid ? kCellValid : kCellVeil);
                if (isValid) {
                    ++valid;
                    EXPECT_EQ(word & 0x7fffu, expected[y].code & 0x7fffu);
                    EXPECT_EQ((word >> 15) & 1u, uint32_t(expected[y].dominantAsk));
                } else {
                    ++veil;
                }
                // Sentinel rows (outside the useful extent) are never valid data.
                if (y == 0 || y + 1 == grid->rows) EXPECT_NE(cellState(word), kCellValid);
            }
        }
        if (tickUnits == 750) EXPECT_EQ(valid, 0u) << "$7.50 cannot be built on $10/$5 grids: veil";
        else EXPECT_GT(valid, 0u);
        EXPECT_GT(veil, 0u);
        EXPECT_GT(noData, 0u);
    }
}

TEST(HeatmapTiles, SameBucketGivesIdenticalCellsWhateverChunksOrTileExtentSurroundIt) {
    // One tile is built once from chunks {0,1,2} and once from {0,1,2,3} (the
    // fourth lies outside the tile) with a deliberately different row extent; a
    // column shared by both builds must be bit-identical in every common bin.
    const auto c0 = makeHourChunk(0), c1 = makeHourChunk(1), c2 = makeHourChunk(2), c3 = makeHourChunk(3);
    const int64_t tf = minute, tile = tileOfBucket(epoch / minute) + 1, tickUnits = 1000;
    const std::vector<std::shared_ptr<const StoredChunk>> two{stored(c0, 1), stored(c1, 2), stored(c2, 3)};
    const std::vector<std::shared_ptr<const StoredChunk>> three{stored(c3, 4), stored(c0, 1), stored(c1, 2), stored(c2, 3)};
    const auto a = composeChunks(two, "BTC-USD", "deep", tf, tileStartMs(tile, tf), tileEndMs(tile, tf));
    const auto b = composeChunks(three, "BTC-USD", "deep", tf, tileStartMs(tile, tf), tileEndMs(tile, tf));
    const auto gridA = *tileGrid(tile, tf, tickUnits, 100, usefulExtent(a, tickUnits, 100), 0);
    // A deliberately different (taller) extent for the second build.
    auto extentB = usefulExtent(b, tickUnits, 100);
    extentB.firstBin -= 7;
    extentB.endBin += 3;
    const auto gridB = *tileGrid(tile, tf, tickUnits, 100, extentB, 0);
    const auto cellsA = buildCells(a, gridA, {}), cellsB = buildCells(b, gridB, {});
    size_t compared = 0;
    for (uint32_t x = 0; x < gridA.columns; ++x)
        for (int64_t bin = gridA.firstBin + 1; bin < gridA.firstBin + int64_t(gridA.rows) - 1; ++bin) {
            const auto ya = size_t(gridA.firstBin + gridA.rows - 1 - bin);
            const auto yb = size_t(gridB.firstBin + gridB.rows - 1 - bin);
            ASSERT_LT(yb, gridB.rows);
            EXPECT_EQ(cellsA[ya * gridA.columns + x], cellsB[yb * gridB.columns + x]) << "bin " << bin;
            ++compared;
        }
    EXPECT_GT(compared, 500u);
}

TEST(HeatmapTiles, TallExtentsAreClippedAroundTheCentreAndEmptySpansStillGetAGrid) {
    const BinExtent extent{1000, 40'000};
    bool clipped = false;
    const auto grid = tileGrid(5, minute, 100, 100, extent, 20'000, &clipped, 1026);
    ASSERT_TRUE(grid);
    EXPECT_TRUE(clipped);
    EXPECT_EQ(grid->rows, 1026u);
    EXPECT_EQ(grid->firstBin + 1, 20'000 - 512);
    const auto edge = tileGrid(5, minute, 100, 100, extent, 500, &clipped, 1026);
    EXPECT_EQ(edge->firstBin + 1, 1000) << "clip window stays inside the extent";
    EXPECT_FALSE(tileGrid(5, minute, 100, 100, BinExtent{}, 0));
    // A span with only a gap: a 1-bin extent gives a grid whose sentinels draw the veil.
    const int64_t t = tileOfBucket(epoch / minute);
    SparseColumns gapOnly{"BTC-USD", "deep", minute, tileStartMs(t, minute), tileEndMs(t, minute), {},
                          {{tileStartMs(t, minute), tileEndMs(t, minute)}}};
    const auto g = tileGrid(t, minute, 1000, 100, {100, 101}, 100);
    const auto cells = buildCells(gapOnly, *g, {});
    for (const uint32_t w : cells) EXPECT_EQ(cellState(w), kCellVeil);
}
} // namespace
