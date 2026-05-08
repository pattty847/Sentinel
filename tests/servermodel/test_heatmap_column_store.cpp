#include <gtest/gtest.h>

#include "servermodel/HeatmapColumnStore.hpp"
#include "servermodel/HmcolFormat.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMs1m = 60'000;

fs::path makeTempDir(const std::string& tag) {
    const auto base = fs::temp_directory_path() / "sentinel-hmcol-tests";
    fs::create_directories(base);
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = base / (tag + "-" + std::to_string(t));
    fs::create_directories(p);
    return p;
}

struct ReadbackRecord {
    hmcol::RecordHeader header{};
    std::vector<uint16_t> intensity;
    std::vector<uint16_t> liquidity;
};

bool readSlot(const fs::path& file,
              int64_t slot,
              int32_t gridHeight,
              uint8_t liquidityFormat,
              ReadbackRecord& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const auto offset = hmcol::slotOffset(slot, gridHeight, liquidityFormat);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(&out.header), sizeof(out.header));
    if (!in) return false;
    out.intensity.assign(static_cast<size_t>(gridHeight), 0);
    in.read(reinterpret_cast<char*>(out.intensity.data()), gridHeight * sizeof(uint16_t));
    if (!in) return false;
    if (liquidityFormat == hmcol::kLiquidityFormatU16) {
        out.liquidity.assign(static_cast<size_t>(gridHeight), 0);
        in.read(reinterpret_cast<char*>(out.liquidity.data()), gridHeight * sizeof(uint16_t));
        if (!in) return false;
    } else {
        out.liquidity.clear();
    }
    return true;
}

bool readFileHeader(const fs::path& file, hmcol::FileHeader& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(&out), sizeof(out));
    return static_cast<bool>(in);
}

std::vector<uint16_t> patternBuffer(int32_t n, uint16_t seed) {
    std::vector<uint16_t> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[i] = static_cast<uint16_t>((seed + i * 7u) & 0xFFFFu);
    }
    return v;
}

} // namespace

// ---------- Lock ----------

TEST(HeatmapColumnStoreLock, AcquiresAndReleases) {
    const auto dir = makeTempDir("lock");
    {
        HeatmapColumnStore store(dir);
        EXPECT_FALSE(store.isLocked());
        ASSERT_TRUE(store.acquireLock());
        EXPECT_TRUE(store.isLocked());
    }
    // Lock released — a new store must be able to acquire it.
    HeatmapColumnStore store2(dir);
    EXPECT_TRUE(store2.acquireLock());
}

TEST(HeatmapColumnStoreLock, RejectsSecondHolder) {
    const auto dir = makeTempDir("lock-contend");
    HeatmapColumnStore first(dir);
    ASSERT_TRUE(first.acquireLock());

    HeatmapColumnStore second(dir);
    EXPECT_FALSE(second.acquireLock());
    EXPECT_FALSE(second.isLocked());
}

TEST(HeatmapColumnStoreLock, AppendFailsWithoutLock) {
    const auto dir = makeTempDir("no-lock");
    HeatmapColumnStore store(dir);
    auto cells = patternBuffer(8, 1);

    auto r = store.append("BTC-USD", kMs1m, 8,
                          /*bucketStartMs=*/kMs1m,
                          /*bucketEndMs=*/kMs1m * 2,
                          100.0, 108.0, 1.0,
                          cells.data(), nullptr, 1.0);
    EXPECT_EQ(r, HeatmapColumnStore::AppendResult::IoError);
}

// ---------- Append: success path ----------

TEST(HeatmapColumnStoreAppend, WritesRecordReadbackMatches) {
    const auto dir = makeTempDir("write-readback");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 32;
    const int64_t day = 1'714'176'000'000; // some UTC midnight
    const int64_t bucketStart = day + 5 * kMs1m;
    const int64_t bucketEnd   = bucketStart + kMs1m;

    auto intensity = patternBuffer(gridHeight, 0xA5A5);
    auto liquidity = patternBuffer(gridHeight, 0x5A5A);

    const auto r = store.append("BTC-USD", kMs1m, gridHeight,
                                bucketStart, bucketEnd,
                                100000.0, 100032.0, 1.0,
                                intensity.data(), liquidity.data(), 2.5);
    EXPECT_EQ(r, HeatmapColumnStore::AppendResult::Written);
    store.flush();

    // Locate the file deterministically by walking the dir.
    fs::path file;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (p.path().extension() == ".hmcol") { file = p.path(); break; }
    }
    ASSERT_FALSE(file.empty()) << "no .hmcol file produced";

    hmcol::FileHeader fh{};
    ASSERT_TRUE(readFileHeader(file, fh));
    EXPECT_TRUE(hmcol::verifyFileHeader(fh));
    EXPECT_EQ(fh.timeframeMs, kMs1m);
    EXPECT_EQ(fh.gridHeight, gridHeight);
    EXPECT_EQ(fh.dayStartMs, day);
    EXPECT_EQ(fh.intensityFormat, hmcol::kIntensityFormatU16);
    EXPECT_EQ(fh.liquidityFormat, hmcol::kLiquidityFormatU16);
    EXPECT_STREQ(std::string(fh.symbol, std::strlen(fh.symbol)).c_str(), "BTC-USD");

    ReadbackRecord rec;
    ASSERT_TRUE(readSlot(file, /*slot=*/5, gridHeight, hmcol::kLiquidityFormatU16, rec));
    EXPECT_EQ(rec.header.bucketStartMs, bucketStart);
    EXPECT_EQ(rec.header.bucketEndMs, bucketEnd);
    EXPECT_DOUBLE_EQ(rec.header.minPrice, 100000.0);
    EXPECT_DOUBLE_EQ(rec.header.maxPrice, 100032.0);
    EXPECT_DOUBLE_EQ(rec.header.tickSize, 1.0);
    EXPECT_DOUBLE_EQ(rec.header.liquidityScale, 2.5);
    EXPECT_TRUE(rec.header.flags & hmcol::kFlagHasLiquidity);
    EXPECT_EQ(rec.intensity, intensity);
    EXPECT_EQ(rec.liquidity, liquidity);

    // Per-record CRC must verify against the actual payload.
    std::vector<uint8_t> payload;
    payload.insert(payload.end(),
                   reinterpret_cast<const uint8_t*>(rec.intensity.data()),
                   reinterpret_cast<const uint8_t*>(rec.intensity.data() + rec.intensity.size()));
    payload.insert(payload.end(),
                   reinterpret_cast<const uint8_t*>(rec.liquidity.data()),
                   reinterpret_cast<const uint8_t*>(rec.liquidity.data() + rec.liquidity.size()));
    EXPECT_TRUE(hmcol::verifyRecord(rec.header, payload.data(), payload.size()));
}

// ---------- Append: idempotency ----------

TEST(HeatmapColumnStoreAppend, RewritingSameBucketIsAlreadyPresent) {
    const auto dir = makeTempDir("idempotent");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 16;
    const int64_t day = 1'714'176'000'000;
    const int64_t bucketStart = day + 0 * kMs1m;
    auto cells = patternBuffer(gridHeight, 1);

    auto r1 = store.append("ETH-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           1000.0, 1016.0, 1.0,
                           cells.data(), nullptr, 1.0);
    EXPECT_EQ(r1, HeatmapColumnStore::AppendResult::Written);

    auto r2 = store.append("ETH-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           1000.0, 1016.0, 1.0,
                           cells.data(), nullptr, 1.0);
    EXPECT_EQ(r2, HeatmapColumnStore::AppendResult::AlreadyPresent);

    EXPECT_EQ(store.stats().written, 1u);
    EXPECT_EQ(store.stats().alreadyPresent, 1u);
}

// ---------- Append: sparse slots remain empty ----------

TEST(HeatmapColumnStoreAppend, SparseSlotsReadbackAsEmpty) {
    const auto dir = makeTempDir("sparse");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 8;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 42);

    // Write only slot 7. Slots 0..6 must remain empty.
    const int64_t bucketStart = day + 7 * kMs1m;
    auto r = store.append("BTC-USD", kMs1m, gridHeight,
                          bucketStart, bucketStart + kMs1m,
                          0.0, 8.0, 1.0,
                          cells.data(), nullptr, 1.0);
    ASSERT_EQ(r, HeatmapColumnStore::AppendResult::Written);
    store.flush();

    fs::path file;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (p.path().extension() == ".hmcol") { file = p.path(); break; }
    }
    ASSERT_FALSE(file.empty());

    // Slots 0..6 should report empty (bucketStartMs == 0).
    for (int64_t s = 0; s <= 6; ++s) {
        ReadbackRecord rec;
        ASSERT_TRUE(readSlot(file, s, gridHeight, hmcol::kLiquidityFormatNone, rec));
        EXPECT_EQ(rec.header.bucketStartMs, hmcol::kEmptySlotSentinel) << "slot=" << s;
    }
    // Slot 7 has the record.
    ReadbackRecord rec;
    ASSERT_TRUE(readSlot(file, 7, gridHeight, hmcol::kLiquidityFormatNone, rec));
    EXPECT_EQ(rec.header.bucketStartMs, bucketStart);
}

// ---------- Append: file is full-day sized ----------

TEST(HeatmapColumnStoreAppend, FileIsSparseExtendedToFullDay) {
    const auto dir = makeTempDir("fullsize");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 4;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 9);

    auto r = store.append("BTC-USD", kMs1m, gridHeight,
                          day + 0 * kMs1m, day + 1 * kMs1m,
                          0.0, 4.0, 1.0,
                          cells.data(), nullptr, 1.0);
    ASSERT_EQ(r, HeatmapColumnStore::AppendResult::Written);

    fs::path file;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (p.path().extension() == ".hmcol") { file = p.path(); break; }
    }
    ASSERT_FALSE(file.empty());

    const std::size_t expected =
        sizeof(hmcol::FileHeader) +
        1440u * hmcol::recordStride(gridHeight, hmcol::kLiquidityFormatNone);
    EXPECT_EQ(fs::file_size(file), expected);
}

// ---------- Reopen: same store object across appends ----------

TEST(HeatmapColumnStoreAppend, SecondAppendOpensExistingFileAndDetectsAlreadyPresent) {
    // Reopen-after-destroy idempotency: the second store object must read the
    // existing file's header, validate it, and treat re-presented buckets as no-ops.
    const auto dir = makeTempDir("reopen");
    constexpr int32_t gridHeight = 16;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 0xCAFE);
    const int64_t bucketStart = day + 3 * kMs1m;

    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        auto r = store.append("BTC-USD", kMs1m, gridHeight,
                              bucketStart, bucketStart + kMs1m,
                              50000.0, 50016.0, 1.0,
                              cells.data(), nullptr, 1.0);
        ASSERT_EQ(r, HeatmapColumnStore::AppendResult::Written);
    }

    HeatmapColumnStore store2(dir);
    ASSERT_TRUE(store2.acquireLock());
    auto r = store2.append("BTC-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           50000.0, 50016.0, 1.0,
                           cells.data(), nullptr, 1.0);
    EXPECT_EQ(r, HeatmapColumnStore::AppendResult::AlreadyPresent);
}

// ---------- SlotConflict (corruption scenario) ----------

TEST(HeatmapColumnStoreAppend, SlotConflictDetectedAfterFileCorruption) {
    const auto dir = makeTempDir("slot-conflict");
    constexpr int32_t gridHeight = 8;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 0xBEEF);
    const int64_t bucketStart = day + 5 * kMs1m;

    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        ASSERT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                               bucketStart, bucketStart + kMs1m,
                               0.0, 8.0, 1.0,
                               cells.data(), nullptr, 1.0),
                  HeatmapColumnStore::AppendResult::Written);
    }

    // Locate file and corrupt slot 5's bucketStartMs to a different (still valid) value.
    fs::path file;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (p.path().extension() == ".hmcol") { file = p.path(); break; }
    }
    ASSERT_FALSE(file.empty());
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        const auto offset = hmcol::slotOffset(5, gridHeight, hmcol::kLiquidityFormatNone);
        f.seekp(static_cast<std::streamoff>(offset));
        const int64_t rogue = bucketStart + kMs1m; // some other valid bucket
        f.write(reinterpret_cast<const char*>(&rogue), sizeof(rogue));
    }

    HeatmapColumnStore store2(dir);
    ASSERT_TRUE(store2.acquireLock());
    auto r = store2.append("BTC-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           0.0, 8.0, 1.0,
                           cells.data(), nullptr, 1.0);
    EXPECT_EQ(r, HeatmapColumnStore::AppendResult::SlotConflict);
    EXPECT_EQ(store2.stats().slotConflicts, 1u);
    EXPECT_EQ(store2.stats().written, 0u);
}

// ---------- BadInput / safety ----------

TEST(HeatmapColumnStoreAppend, RejectsBadInputs) {
    const auto dir = makeTempDir("bad-input");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 4;
    auto cells = patternBuffer(gridHeight, 1);
    const int64_t day = kMs1m * 1440 * 100;
    const int64_t bucketStart = day + kMs1m;

    // Null intensity.
    EXPECT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           0, 4.0, 1.0,
                           nullptr, nullptr, 1.0),
              HeatmapColumnStore::AppendResult::BadInput);

    // Non-divisible timeframe (7s does not divide a UTC day evenly).
    EXPECT_EQ(store.append("BTC-USD", 7'000, gridHeight,
                           bucketStart, bucketStart + 7'000,
                           0, 4.0, 1.0,
                           cells.data(), nullptr, 1.0),
              HeatmapColumnStore::AppendResult::BadInput);

    // bucketEnd <= bucketStart.
    EXPECT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                           bucketStart, bucketStart,
                           0, 4.0, 1.0,
                           cells.data(), nullptr, 1.0),
              HeatmapColumnStore::AppendResult::BadInput);

    // Empty symbol.
    EXPECT_EQ(store.append("", kMs1m, gridHeight,
                           bucketStart, bucketStart + kMs1m,
                           0, 4.0, 1.0,
                           cells.data(), nullptr, 1.0),
              HeatmapColumnStore::AppendResult::BadInput);
}

// ---------- loadRecent (Phase 2 reader) ----------

TEST(HeatmapColumnStoreLoad, EmptyDirReturnsFalse) {
    const auto dir = makeTempDir("load-empty");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    std::vector<HeatmapColumnStore::LoadedColumn> out;
    EXPECT_FALSE(store.loadRecent("BTC-USD", kMs1m, 10, out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(store.hasAnyFile("BTC-USD", kMs1m));
}

TEST(HeatmapColumnStoreLoad, ReturnsMostRecentInChronologicalOrder) {
    const auto dir = makeTempDir("load-order");
    constexpr int32_t gridHeight = 8;
    const int64_t day = 1'714'176'000'000;

    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    // Write five columns at slots 100, 105, 110, 115, 120 with distinct payloads.
    const int slots[] = {100, 105, 110, 115, 120};
    for (int s : slots) {
        auto cells = patternBuffer(gridHeight, static_cast<uint16_t>(s));
        const int64_t bs = day + s * kMs1m;
        ASSERT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                               bs, bs + kMs1m,
                               1000.0, 1008.0, 1.0,
                               cells.data(), nullptr, 1.0),
                  HeatmapColumnStore::AppendResult::Written);
    }

    EXPECT_TRUE(store.hasAnyFile("BTC-USD", kMs1m));

    std::vector<HeatmapColumnStore::LoadedColumn> out;
    ASSERT_TRUE(store.loadRecent("BTC-USD", kMs1m, /*maxCount=*/3, out));
    ASSERT_EQ(out.size(), 3u);

    // Should be the three latest in chronological order: 110, 115, 120.
    EXPECT_EQ(out[0].bucketStartMs, day + 110 * kMs1m);
    EXPECT_EQ(out[1].bucketStartMs, day + 115 * kMs1m);
    EXPECT_EQ(out[2].bucketStartMs, day + 120 * kMs1m);

    // Payload for slot 110 should match patternBuffer(gridHeight, 110).
    auto expected = patternBuffer(gridHeight, 110);
    ASSERT_EQ(out[0].intensity.size(), expected.size() * sizeof(uint16_t));
    EXPECT_EQ(std::memcmp(out[0].intensity.data(), expected.data(),
                          expected.size() * sizeof(uint16_t)), 0);

    // gridHeight + price band metadata round-trip.
    EXPECT_EQ(out[0].gridHeight, gridHeight);
    EXPECT_DOUBLE_EQ(out[0].minPrice, 1000.0);
    EXPECT_DOUBLE_EQ(out[0].maxPrice, 1008.0);
}

TEST(HeatmapColumnStoreLoad, MaxCountLargerThanAvailableReturnsAll) {
    const auto dir = makeTempDir("load-overshoot");
    constexpr int32_t gridHeight = 4;
    const int64_t day = 1'714'176'000'000;

    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    auto cells = patternBuffer(gridHeight, 1);
    for (int s : {0, 7, 14}) {
        const int64_t bs = day + s * kMs1m;
        ASSERT_EQ(store.append("X", kMs1m, gridHeight,
                               bs, bs + kMs1m, 0.0, 4.0, 1.0,
                               cells.data(), nullptr, 1.0),
                  HeatmapColumnStore::AppendResult::Written);
    }

    std::vector<HeatmapColumnStore::LoadedColumn> out;
    ASSERT_TRUE(store.loadRecent("X", kMs1m, /*maxCount=*/100, out));
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].bucketStartMs, day + 0 * kMs1m);
    EXPECT_EQ(out[1].bucketStartMs, day + 7 * kMs1m);
    EXPECT_EQ(out[2].bucketStartMs, day + 14 * kMs1m);
}

TEST(HeatmapColumnStoreLoad, RoundTripWithLiquidity) {
    const auto dir = makeTempDir("load-liq");
    constexpr int32_t gridHeight = 16;
    const int64_t day = 1'714'176'000'000;
    auto intensity = patternBuffer(gridHeight, 0xA5A5);
    auto liquidity = patternBuffer(gridHeight, 0x5A5A);
    const int64_t bs = day + 42 * kMs1m;

    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        ASSERT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                               bs, bs + kMs1m, 1.0, 17.0, 1.0,
                               intensity.data(), liquidity.data(), 3.5),
                  HeatmapColumnStore::AppendResult::Written);
    }

    HeatmapColumnStore reader(dir);
    ASSERT_TRUE(reader.acquireLock());
    std::vector<HeatmapColumnStore::LoadedColumn> out;
    ASSERT_TRUE(reader.loadRecent("BTC-USD", kMs1m, 10, out));
    ASSERT_EQ(out.size(), 1u);

    EXPECT_EQ(out[0].bucketStartMs, bs);
    EXPECT_DOUBLE_EQ(out[0].liquidityScale, 3.5);
    ASSERT_EQ(out[0].liquidity.size(), liquidity.size() * sizeof(uint16_t));
    EXPECT_EQ(std::memcmp(out[0].liquidity.data(), liquidity.data(),
                          liquidity.size() * sizeof(uint16_t)), 0);
    EXPECT_EQ(std::memcmp(out[0].intensity.data(), intensity.data(),
                          intensity.size() * sizeof(uint16_t)), 0);
}

TEST(HeatmapColumnStoreLoad, SkipsRecordsWithBadCrc) {
    const auto dir = makeTempDir("load-corrupt");
    constexpr int32_t gridHeight = 4;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 7);

    // Write three records.
    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        for (int s : {1, 2, 3}) {
            const int64_t bs = day + s * kMs1m;
            ASSERT_EQ(store.append("BTC-USD", kMs1m, gridHeight,
                                   bs, bs + kMs1m, 0.0, 4.0, 1.0,
                                   cells.data(), nullptr, 1.0),
                      HeatmapColumnStore::AppendResult::Written);
        }
    }

    // Corrupt the intensity payload of slot 2 (so its CRC no longer matches).
    fs::path file;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (p.path().extension() == ".hmcol") { file = p.path(); break; }
    }
    ASSERT_FALSE(file.empty());
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        const auto offset = hmcol::slotOffset(2, gridHeight, hmcol::kLiquidityFormatNone);
        // Skip the 56-byte record header, hit the first byte of intensity.
        f.seekp(static_cast<std::streamoff>(offset + sizeof(hmcol::RecordHeader)));
        const uint16_t flipped = 0xDEAD;
        f.write(reinterpret_cast<const char*>(&flipped), sizeof(flipped));
    }

    HeatmapColumnStore reader(dir);
    ASSERT_TRUE(reader.acquireLock());
    std::vector<HeatmapColumnStore::LoadedColumn> out;
    ASSERT_TRUE(reader.loadRecent("BTC-USD", kMs1m, 10, out));
    // Slot 2 corrupted; only 1 and 3 should come back.
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].bucketStartMs, day + 1 * kMs1m);
    EXPECT_EQ(out[1].bucketStartMs, day + 3 * kMs1m);
}

// ---------- Stats ----------

TEST(HeatmapColumnStoreStats, CountsResultsCorrectly) {
    const auto dir = makeTempDir("stats");
    HeatmapColumnStore store(dir);
    ASSERT_TRUE(store.acquireLock());

    constexpr int32_t gridHeight = 8;
    const int64_t day = 1'714'176'000'000;
    auto cells = patternBuffer(gridHeight, 0x1234);

    // Three writes, two unique buckets.
    store.append("X", kMs1m, gridHeight, day, day + kMs1m, 0, 8.0, 1.0,
                 cells.data(), nullptr, 1.0);
    store.append("X", kMs1m, gridHeight, day + kMs1m, day + 2 * kMs1m, 0, 8.0, 1.0,
                 cells.data(), nullptr, 1.0);
    store.append("X", kMs1m, gridHeight, day, day + kMs1m, 0, 8.0, 1.0,
                 cells.data(), nullptr, 1.0);

    auto s = store.stats();
    EXPECT_EQ(s.written, 2u);
    EXPECT_EQ(s.alreadyPresent, 1u);
    EXPECT_EQ(s.slotConflicts, 0u);
    EXPECT_EQ(s.ioErrors, 0u);
    EXPECT_EQ(s.badInputs, 0u);
}
