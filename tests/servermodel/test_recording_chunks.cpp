#include "heatmap/BinCell.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/RecordingChunks.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>

namespace {
using namespace recording;
using namespace heatmap;
constexpr int64_t epoch = kHmc2MinMs;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
Hmc2Record minuteRecord(int i, const std::string& layer = "deep") {
    Hmc2Record r;
    const int tick = layer == "near" ? 1 : i < 30 ? 1 : 2;
    r.header = {"BTC-USD", layer, kMinuteMs, 100., 100 * tick, {}, uint64_t(tick)};
    r.bucketStartMs = epoch + int64_t(i) * kMinuteMs;
    r.observedMs = i % 7 ? 60000 : 30000;
    r.flags = (r.observedMs < 60000 ? kPartial : 0) | (i == 61 ? kResynced : 0);
    r.bidRowLo = r.askRowLo = 100 / tick;
    r.bidRowHi = r.askRowHi = 106 / tick - 1;
    r.entries = {{101 / tick, false, encodeSize(2. + i / 100.), 0, r.observedMs},
                 {104 / tick, true, encodeSize(3. + i / 100.), 0, r.observedMs}};
    std::sort(r.entries.begin(), r.entries.end(), [](const auto& a, const auto& b) {
        return std::pair(a.row, a.isAsk) < std::pair(b.row, b.isAsk);
    });
    return r;
}
Hmc2Record hourRecord() {
    auto r = minuteRecord(0);
    r.header.tfMs = kHourMs;
    r.bucketStartMs = epoch;
    r.observedMs = 3'000'000;
    r.flags = kPartial | kLateEvents;
    r.entries = {{101, false, encodeSize(2), 0, 2'000'000},
                 {104, true, encodeSize(3), 0, 3'000'000}};
    r.coverage = {{100, 102, false, 2'000'000}, {103, 105, false, 3'000'000},
                  {100, 105, true, 3'000'000}};
    return r;
}
void same(const SparseColumns& a, const SparseColumns& b) {
    ASSERT_EQ(a.symbol, b.symbol); ASSERT_EQ(a.layer, b.layer); ASSERT_EQ(a.tfMs, b.tfMs);
    ASSERT_EQ(a.startMs, b.startMs); ASSERT_EQ(a.endMs, b.endMs);
    ASSERT_EQ(a.scannedRanges, b.scannedRanges);
    ASSERT_EQ(a.columns.size(), b.columns.size());
    for (size_t i = 0; i < a.columns.size(); ++i) {
        const auto& x = a.columns[i]; const auto& y = b.columns[i];
        EXPECT_EQ(x.bucketStartMs, y.bucketStartMs); EXPECT_EQ(x.observedMs, y.observedMs);
        EXPECT_EQ(x.flags, y.flags); ASSERT_EQ(x.native.size(), y.native.size());
        for (size_t j = 0; j < x.native.size(); ++j) {
            const auto& n = x.native[j]; const auto& m = y.native[j];
            EXPECT_EQ(n.grid.configHash, m.grid.configHash);
            EXPECT_EQ(n.grid.rowTickUnits, m.grid.rowTickUnits);
            EXPECT_EQ(n.grid.priceScale, m.grid.priceScale);
            EXPECT_EQ(n.sizeScale.floor, m.sizeScale.floor);
            EXPECT_EQ(n.sizeScale.codesPerOctave, m.sizeScale.codesPerOctave);
            EXPECT_EQ(n.baseRow, m.baseRow); EXPECT_EQ(n.observedMs, m.observedMs);
            EXPECT_EQ(n.composed, m.composed);
            for (int side = 0; side < 2; ++side) {
                ASSERT_EQ(n.coverage[side].size(), m.coverage[side].size());
                for (size_t k = 0; k < n.coverage[side].size(); ++k) {
                    EXPECT_EQ(n.coverage[side][k].lo, m.coverage[side][k].lo);
                    EXPECT_EQ(n.coverage[side][k].hi, m.coverage[side][k].hi);
                    EXPECT_EQ(n.coverage[side][k].coveredMs, m.coverage[side][k].coveredMs);
                }
            }
            ASSERT_EQ(n.entries.size(), m.entries.size());
            for (size_t k = 0; k < n.entries.size(); ++k) {
                EXPECT_EQ(n.entries[k].rowSide, m.entries[k].rowSide);
                EXPECT_EQ(n.entries[k].code, m.entries[k].code);
            }
            EXPECT_EQ(n.entryCoveredMs, m.entryCoveredMs);
            EXPECT_EQ(n.numerators, m.numerators);
        }
    }
}
class ChunkTest : public testing::Test {
protected:
    QTemporaryDir temp;
    std::filesystem::path root() const { return temp.path().toStdString(); }
    void seed() {
        Hmc2Store store(root());
        for (int i = 0; i < 120; ++i) {
            if (i == 9 || (i >= 40 && i < 45) || i == 68) continue;
            store.append(minuteRecord(i)); store.append(minuteRecord(i, "near"));
        }
        store.append(hourRecord());
    }
};
TEST_F(ChunkTest, RoundTripGapsGridChangesCoverageAndOpenSealedState) {
    seed(); Hmc2Reader reader(root());
    ChunkKey first{"BTC-USD", "deep", kMinuteMs, epoch};
    ChunkKey second{"BTC-USD", "deep", kMinuteMs, epoch + kHourMs};
    const auto a = buildChunk(reader, first), b = buildChunk(reader, second);
    ASSERT_EQ(a.scannedRanges.size(), 1); EXPECT_EQ(a.scannedRanges[0].startMs, epoch);
    EXPECT_EQ(a.scannedRanges[0].endMs, epoch + kHourMs);
    EXPECT_EQ(bucketState(a, epoch + 9 * kMinuteMs), BucketState::Gap);
    EXPECT_EQ(bucketState(a, epoch + 10 * kMinuteMs), BucketState::Present);
    EXPECT_EQ(bucketState(a, epoch + kHourMs), BucketState::NotLoaded);
    const auto prefix = buildChunk(reader, first, epoch + 30*kMinuteMs + 1);
    ASSERT_EQ(prefix.scannedRanges.size(), 1);
    EXPECT_EQ(prefix.scannedRanges[0].endMs, epoch + 30*kMinuteMs);
    EXPECT_EQ(bucketState(prefix, epoch + 31*kMinuteMs), BucketState::NotLoaded);
    EXPECT_NE(a.columns.front().native.front().grid.configHash,
              b.columns.front().native.front().grid.configHash);
    auto open = chunkState(first, epoch + 30*kMinuteMs + 1, 17, 5000);
    EXPECT_FALSE(open.sealed); EXPECT_EQ(open.revision, 17);
    ChunkFrame f{ChunkKind::Chunk, 77, first, open, prefix};
    auto encoded = encodeChunk(f); auto decoded = decodeChunk(encoded);
    same(prefix, decoded.columns);
    EXPECT_EQ(decoded.requestId, 77); EXPECT_EQ(decoded.state.committedThroughMs, open.committedThroughMs);
    EXPECT_EQ(decoded.state.revision, 17); EXPECT_FALSE(decoded.state.sealed);
    EXPECT_NE(decoded.contentHash, 0);
    auto sealed = chunkState(first, epoch + kHourMs + 5000, 19, 5000);
    EXPECT_TRUE(sealed.sealed); EXPECT_EQ(sealed.revision, 0);
    f.state = sealed; f.columns = a;
    auto sealedWire = encodeChunk(f);
    same(a, decodeChunk(sealedWire).columns);
    ChunkFrame other{ChunkKind::Chunk, 0, second, chunkState(second, epoch + 2*kHourMs, 1, 0), b};
    auto otherWire = encodeChunk(other);
    EncodedChunkLru cache(std::max(sealedWire.size(), otherWire.size()) + 10);
    EXPECT_THROW(cache.put({ChunkKind::Chunk, 0, first, open, prefix}, encoded), std::invalid_argument);
    cache.put(f, std::move(sealedWire)); EXPECT_TRUE(cache.get(first));
    cache.put(other, std::move(otherWire)); EXPECT_FALSE(cache.get(first));
    auto version = encoded; version[4] = 2;
    EXPECT_THROW(decodeChunk(version), std::invalid_argument);
    encoded.back() ^= 1;
    EXPECT_THROW(decodeChunk(encoded), std::invalid_argument);
}
TEST_F(ChunkTest, HourCoverageSidecarsAndComposedNumeratorsRoundTrip) {
    seed(); Hmc2Reader reader(root());
    ChunkKey hourKey{"BTC-USD", "deep", kHourMs, epoch};
    auto hours = buildChunk(reader, hourKey);
    ASSERT_EQ(hours.columns.size(), 1);
    EXPECT_EQ(hours.columns[0].native[0].entryCoveredMs.size(), 2);
    EXPECT_EQ(hours.columns[0].native[0].coverage[0].size(), 2);
    ChunkFrame f{ChunkKind::Chunk, 0, hourKey, chunkState(hourKey, epoch+kDayMs, 0, 0), hours};
    std::vector<uint8_t> hourWire;
    ASSERT_NO_THROW(hourWire = encodeChunk(f));
    ASSERT_NO_THROW(same(hours, decodeChunk(hourWire).columns));
    auto minutes = buildChunk(reader, {"BTC-USD", "deep", kMinuteMs, epoch});
    auto composed = compose(minutes, 5 * kMinuteMs);
    ASSERT_FALSE(composed.columns.empty());
    EXPECT_TRUE(composed.columns.front().native.front().composed);
    // Codec's chunk identity is native-only; exercise exact composed payload
    // through the same binary layout by a full 60-minute composition.
    auto composedHour = compose(minutes, kHourMs);
    ASSERT_EQ(composedHour.columns.size(), 1);
    EXPECT_EQ(composedHour.startMs, epoch);
    EXPECT_EQ(composedHour.endMs, epoch + kHourMs);
    EXPECT_NO_THROW(validate(composedHour));
    composedHour.endMs = epoch + kDayMs; // partial scan inside a day-sized hour chunk
    ChunkFrame cf{ChunkKind::Chunk, 0, {"BTC-USD", "deep", kHourMs, epoch}, {}, composedHour};
    std::vector<uint8_t> composedWire;
    ASSERT_NO_THROW(composedWire = encodeChunk(cf));
    auto decoded = decodeChunk(composedWire);
    same(composedHour, decoded.columns);
    EXPECT_TRUE(decoded.columns.columns[0].native[0].composed);
}
TEST_F(ChunkTest, DecodeEncodeBuildChunkMatchesBuildPage) {
    seed(); Hmc2Reader reader(root());
    std::array<SparseColumns, 2> chunks{
        buildChunk(reader, {"BTC-USD", "deep", kMinuteMs, epoch}),
        buildChunk(reader, {"BTC-USD", "deep", kMinuteMs, epoch+kHourMs})};
    for (auto& c : chunks) {
        ChunkKey key{c.symbol, c.layer, c.tfMs, c.startMs};
        c = decodeChunk(encodeChunk({ChunkKind::Chunk, 0, key, {}, c})).columns;
    }
    for (int64_t tf : {kMinuteMs, 5*kMinuteMs}) {
        BuildRequest q;
        q.symbol = "BTC-USD"; q.tfMs = tf; q.endMs = epoch + 2*kHourMs - tf;
        q.count = uint32_t(2*kHourMs/tf); q.priceLo = 100; q.priceHi = 106;
        q.rows = 3; q.displayTick = 2.; q.budgets = {};
        const auto page = buildPage(reader, q);
        ASSERT_EQ(page.status, BuildStatus::Complete) << page.message;
        ASSERT_EQ(page.layer, "deep");
        const auto composed = compose(chunks, tf);
        std::map<int64_t, const SparseColumn*> byTime;
        for (const auto& c : composed.columns) byTime[c.bucketStartMs] = &c;
        for (const auto& expected : page.columns) {
            auto it = byTime.find(expected.bucketStartMs);
            if (it == byTime.end()) continue; // page padding is a proven gap
            const auto& actual = *it->second;
            EXPECT_EQ(actual.observedMs, expected.observedMs);
            EXPECT_EQ(actual.flags, expected.flags);
            const auto cells = binColumn(actual, page.band.lo,
                page.band.lo + page.band.tick * page.band.rows, page.band.tick, page.sizeScale);
            ASSERT_EQ(cells.size(), expected.cells.size());
            for (size_t row = 0; row < cells.size(); ++row) {
                EXPECT_EQ(cells[row].code, expected.cells[row]);
                EXPECT_EQ(cells[row].valid, bool((expected.validity[row/8] >> (row%8)) & 1));
            }
        }
    }
}
TEST(ChunkBench, LastComplete24Hours) {
    if (!std::getenv("SENTINEL_CHUNK_BENCH") ||
        std::string(std::getenv("SENTINEL_CHUNK_BENCH")) != "1") GTEST_SKIP();
    const auto root = std::filesystem::path("/Volumes/T7/sentinel-data/recording");
    ASSERT_TRUE(std::filesystem::exists(root));
    for (const std::string layer : {"deep", "near"}) {
        Hmc2Reader availabilityReader(root); ReadControl control;
        const auto avail = availabilityReader.availability("BTC-USD", layer, kMinuteMs, control);
        ASSERT_EQ(control.status, ReadStatus::Complete);
        ASSERT_TRUE(avail.latestMs);
        const auto end = floorDiv(*avail.latestMs, kHourMs) * kHourMs;
        ASSERT_GE(end - 24*kHourMs, kHmc2MinMs);
        size_t totalBytes = 0; double coldMs = 0, encMs = 0, decMs = 0;
        for (int i = 0; i < 24; ++i) {
            ChunkKey key{"BTC-USD", layer, kMinuteMs, end - (24-i)*kHourMs};
            const auto start = Clock::now();
            Hmc2Reader cold(root);
            const auto chunk = buildChunk(cold, key);
            const auto readEnd = Clock::now();
            auto wire = encodeChunk({ChunkKind::Chunk, 0, key, {true, end, 0}, chunk});
            const auto encoded = Clock::now();
            auto decoded = decodeChunk(wire);
            const auto decodedAt = Clock::now();
            ASSERT_EQ(decoded.columns.columns.size(), chunk.columns.size());
            totalBytes += wire.size(); coldMs += ms(start, readEnd);
            encMs += ms(readEnd, encoded); decMs += ms(encoded, decodedAt);
        }
        std::cout << "CHUNK_BENCH layer=" << layer << " hours=24 bytes_per_hour=" << totalBytes/24
                  << " encode_ms_per_hour=" << encMs/24 << " decode_ms_per_hour=" << decMs/24
                  << " cold_reader_ms_per_hour=" << coldMs/24 << '\n';
    }
}
} // namespace
