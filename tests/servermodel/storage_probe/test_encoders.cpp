#include "Encoders.hpp"
#include "servermodel/BookRecorder.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <bit>
#include <fstream>

using namespace storage_probe;
using namespace recording;
namespace {
constexpr int64_t epoch = kHmc2MinMs;
Event snap(int64_t t = 0) { return {epoch + t, Kind::Snapshot, {{true, 99, 2}, {false, 101, 4}}}; }
const Hmc2Entry* find(const Hmc2Record& r, int64_t row, bool ask = false) {
    for (const auto& e : r.entries) if (e.row == row && e.isAsk == ask) return &e;
    return nullptr;
}
void value(const Hmc2Record& r, int64_t row, bool ask, double mean, double peak) {
    const auto* e = find(r, row, ask);
    ASSERT_NE(e, nullptr);
    EXPECT_NEAR(decodeSize(e->twapCode), mean, std::max(1e-9, mean * .001));
    EXPECT_NEAR(decodeSize(e->peakCode), peak, std::max(1e-9, peak * .001));
}
}
TEST(StorageRaw, LosslessRoundTripAcrossBlocksAndCorruption) {
    std::vector<Bytes> blocks;
    RawEncoder raw([&](const auto& frame) { blocks.push_back(frame); });
    std::vector<Event> events{snap(), {epoch + 17, Kind::Delta,
        {{true, 99.01, .1234567891234567}, {true, 99, 0}, {false, 101, 7}}},
        {epoch + 17, Kind::Delta, {}}, {epoch + 1000, Kind::Invalid, {}}, snap(1700)};
    for (const auto& e : events) raw.add(e);
    raw.flush();
    ASSERT_EQ(blocks.size(), 2);
    std::vector<Event> decoded;
    uint64_t bytes = 32;
    for (const auto& block : blocks) {
        bytes += block.size();
        const auto got = RawEncoder::decode(block);
        decoded.insert(decoded.end(), got.begin(), got.end());
    }
    EXPECT_EQ(bytes, raw.stats().bytes);
    ASSERT_EQ(decoded.size(), events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(decoded[i].timeMs, events[i].timeMs);
        EXPECT_EQ(decoded[i].kind, events[i].kind);
        ASSERT_EQ(decoded[i].levels.size(), events[i].levels.size());
        for (size_t j = 0; j < events[i].levels.size(); ++j) {
            EXPECT_EQ(decoded[i].levels[j].bid, events[i].levels[j].bid);
            EXPECT_EQ(std::bit_cast<uint64_t>(decoded[i].levels[j].price), std::bit_cast<uint64_t>(events[i].levels[j].price));
            EXPECT_EQ(std::bit_cast<uint64_t>(decoded[i].levels[j].size), std::bit_cast<uint64_t>(events[i].levels[j].size));
        }
    }
    blocks[0].back() ^= 1;
    EXPECT_THROW(RawEncoder::decode(blocks[0]), std::runtime_error);
    EXPECT_THROW(RawEncoder::decode(Bytes{1, 2}), std::runtime_error);
    EXPECT_THROW(raw.add(snap()), std::runtime_error);
}
TEST(StorageColumns, AllResolutionsIntegrateTimeInsteadOfSampleCount) {
    for (auto tf : {100, 1000, 10'000, 60'000}) {
        std::vector<Hmc2Record> records;
        Columns c(tf, true, [&](auto r) { if (r.header.layer == "near") records.push_back(r); });
        c.add(snap());
        c.add({epoch + tf / 4, Kind::Delta, {{true, 99, 6}}});
        c.add({epoch + tf / 4, Kind::Delta, {{true, 99, 6}}});
        c.finish(epoch + tf);
        ASSERT_EQ(records.size(), 1);
        EXPECT_EQ(records[0].observedMs, tf);
        value(records[0], 99, false, 5, 6);
        value(records[0], 101, true, 4, 4);
        EXPECT_EQ(c.codecs()[0].stats().blocks, 1);
    }
}
TEST(StorageColumns, AtomicBatchDoesNotInventPeakAndSidesRemainSeparate) {
    std::vector<Hmc2Record> records;
    Columns c(100, true, [&](auto r) { if (r.header.layer == "deep") records.push_back(r); });
    c.add({epoch, Kind::Snapshot, {{true, 99, 2}, {true, 98, 3}, {false, 101, 4}}});
    c.add({epoch + 50, Kind::Delta, {{true, 99, 5}, {true, 98, 0}, {false, 99, 7}}});
    c.finish(epoch + 100);
    ASSERT_EQ(records.size(), 1);
    value(records[0], 19, false, 5, 5);
    value(records[0], 19, true, 3.5, 7);
}
TEST(StorageColumns, GapsAreUnknownAndSnapshotResumesWithoutLosingEarlierIntegral) {
    std::vector<Hmc2Record> records;
    Columns c(100, true, [&](auto r) { if (r.header.layer == "near") records.push_back(r); });
    c.add(snap());
    c.add({epoch + 20, Kind::Invalid, {}});
    c.add({epoch + 30, Kind::Delta, {{true, 99, 999}}});
    c.add({epoch + 60, Kind::Snapshot, {{true, 99, 8}, {false, 101, 4}}});
    c.add({epoch + 100, Kind::Invalid, {}});
    c.advance(epoch + 60000);
    c.finish(epoch + 60000);
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records[0].observedMs, 60);
    EXPECT_TRUE(records[0].flags & kPartial);
    value(records[0], 99, false, 6, 8);
    EXPECT_EQ(c.ignoredDeltas(), 1);
}
TEST(StorageColumns, EpochAlignmentSilentBookAndPartialEdges) {
    std::vector<Hmc2Record> records;
    Columns c(100, true, [&](auto r) { if (r.header.layer == "near") records.push_back(r); });
    c.add(snap(75)); c.finish(epoch + 250);
    ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records[0].bucketStartMs, epoch);
    EXPECT_EQ(records[0].observedMs, 25);
    EXPECT_EQ(records[1].observedMs, 100);
    EXPECT_EQ(records[2].observedMs, 50);
    for (const auto& r : records) value(r, 99, false, 2, 2);
    EXPECT_EQ(c.codecs()[0].stats().partialColumns, 2);
    EXPECT_EQ(c.codecs()[0].stats().observedMs, 175);
}
TEST(StorageColumns, MalformedAndOneSidedBooksInvalidateUntilSnapshot) {
    Columns c(100);
    c.add(snap());
    c.add({epoch + 10, Kind::Delta, {{true, 99, std::numeric_limits<double>::quiet_NaN()}}});
    c.add({epoch + 20, Kind::Delta, {{true, 99, 5}}});
    c.add({epoch + 30, Kind::Snapshot, {{true, 99, 4}}});
    c.finish(epoch + 100);
    EXPECT_EQ(c.invalidations(), 2);
    EXPECT_EQ(c.ignoredDeltas(), 1);
    EXPECT_EQ(c.codecs()[0].stats().observedMs, 10);
    EXPECT_THROW(c.advance(epoch + 200), std::runtime_error);
}
TEST(StorageColumns, MovingWindowMatchesProductionEnvelopeAndDoesNotLoseWall) {
    std::vector<Hmc2Record> records;
    Columns c(100, true, [&](auto r) { if (r.header.layer == "near") records.push_back(r); });
    c.add({epoch, Kind::Snapshot, {{true, 99, 2}, {false, 101, 4}, {false, 120, 10}}});
    c.add({epoch + 50, Kind::Delta,
        {{true, 99, 0}, {false, 101, 0}, {true, 119, 1}, {false, 121, 1}, {false, 120, 0}}});
    c.finish(epoch + 100);
    ASSERT_EQ(records.size(), 1);
    value(records[0], 120, true, 5, 10);
    EXPECT_EQ(records[0].bidRowLo, 95);
    EXPECT_EQ(records[0].askRowHi, 126);
}
TEST(StorageColumns, PeakOnlyForcesKeyframeAndNoPeakOmitsIt) {
    for (bool peak : {true, false}) {
        std::vector<Hmc2Record> records;
        Columns c(100, peak, [&](auto r) { if (r.header.layer == "near") records.push_back(r); });
        c.add(snap()); c.advance(epoch + 100);
        c.add({epoch + 110, Kind::Delta, {{true, 98, 9}}});
        c.add({epoch + 110, Kind::Delta, {{true, 98, 0}}});
        c.finish(epoch + 200);
        ASSERT_EQ(records.size(), 2);
        if (peak) {
            value(records[1], 98, false, 0, 9);
            EXPECT_EQ(c.codecs()[0].stats().keyframes, 2);
        } else {
            EXPECT_EQ(find(records[1], 98), nullptr);
            EXPECT_EQ(c.codecs()[0].stats().deltas, 1);
        }
    }
}
TEST(StorageCodec, ByteParityWithProductionIncludingRemovalKeyframesAndDayRollover) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Hmc2Store store(dir.path().toStdString());
    Bytes frames;
    ColumnCodec codec([&](const Bytes& b) { frames.insert(frames.end(), b.begin(), b.end()); });
    uint64_t physicalBytes = 0;
    for (int day = 0; day < 2; ++day) {
        frames.clear();
        Hmc2Record r;
        r.header = {"BTC-USD", "near", 60'000, 100, 100, {}, 0};
        r.observedMs = 60'000; r.bidRowLo = r.askRowLo = 95; r.bidRowHi = r.askRowHi = 105;
        r.midOpen = r.midClose = r.midMin = r.midMax = 100;
        for (int i = 0; i < 17; ++i) {
            r.bucketStartMs = epoch + day * 86'400'000LL + i * 60'000;
            r.entries = {{99, false, encodeSize(2 + i % 2), encodeSize(4)}, {101, true, encodeSize(4), encodeSize(4)}};
            if (i == 2) r.entries.erase(r.entries.begin());
            if (i == 3) r.entries[0].twapCode = 0;
            store.append(r); codec.add(r);
        }
        const auto path = Hmc2Store::filePath(dir.path().toStdString(), r.header, r.bucketStartMs);
        std::ifstream in(path, std::ios::binary);
        Bytes disk((std::istreambuf_iterator<char>(in)), {});
        ASSERT_GT(disk.size(), frames.size());
        EXPECT_TRUE(std::equal(frames.begin(), frames.end(), disk.end() - frames.size()));
        physicalBytes += disk.size();
        auto decoded = Hmc2Store::readRange(dir.path().toStdString(), "BTC-USD", "near", 60000,
            epoch + day * 86'400'000LL, epoch + day * 86'400'000LL + 17 * 60'000);
        ASSERT_EQ(decoded.size(), 17);
        EXPECT_EQ(decoded[3].entries[0].twapCode, 0);
    }
    EXPECT_EQ(codec.stats().bytes, physicalBytes);
    EXPECT_EQ(codec.stats().keyframes, 6); // day start, peak-only, 15m boundary, each day
}
TEST(StorageColumns, MinuteIntegrationMatchesProductionRecorder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    int64_t local = epoch;
    RecorderConfig cfg{dir.path().toStdString(), 100, {},
        {{"near", 100, .95, 1.05, false}, {"deep", 500, .25, 4, false}}, 0, 2'000'000};
    BookRecorder recorder(cfg, [&] { return local; });
    std::vector<Hmc2Record> records;
    Columns c(60'000, true, [&](auto r) { records.push_back(r); });
    const std::vector<Event> events{snap(13000), {epoch + 25000, Kind::Delta, {{true, 99, 8}}},
        {epoch + 45000, Kind::Invalid, {}}, snap(52000),
        {epoch + 83000, Kind::Delta, {{false, 101, 9}}}};
    for (const auto& e : events) {
        local = e.timeMs;
        c.add(e);
        std::vector<recording::Level> levels;
        for (auto l : e.levels) levels.push_back({l.bid, l.price, l.size});
        if (e.kind == Kind::Snapshot) recorder.onSnapshot("BTC-USD", local, levels);
        else if (e.kind == Kind::Delta) recorder.onUpdates("BTC-USD", local, levels);
        else recorder.onInvalid("BTC-USD", local, "fixture");
        recorder.drainForTest();
    }
    local = epoch + 120000; c.finish(local); recorder.onTick(local); recorder.drainForTest();
    for (const auto& layer : {"near", "deep"}) {
        auto disk = Hmc2Store::readRange(dir.path().toStdString(), "BTC-USD", layer, 60000, epoch, local);
        std::vector<Hmc2Record> simulated;
        for (auto r : records) if (r.header.layer == layer) simulated.push_back(r);
        ASSERT_EQ(disk.size(), simulated.size());
        for (size_t i = 0; i < disk.size(); ++i) {
            const auto& a = disk[i]; const auto& b = simulated[i];
            EXPECT_EQ(a.observedMs, b.observedMs); EXPECT_EQ(a.flags, b.flags);
            EXPECT_EQ(a.bidRowLo, b.bidRowLo); EXPECT_EQ(a.askRowHi, b.askRowHi);
            ASSERT_EQ(a.entries.size(), b.entries.size());
            for (size_t j = 0; j < a.entries.size(); ++j) {
                EXPECT_EQ(a.entries[j].row, b.entries[j].row);
                EXPECT_EQ(a.entries[j].isAsk, b.entries[j].isAsk);
                EXPECT_EQ(a.entries[j].twapCode, b.entries[j].twapCode);
                EXPECT_EQ(a.entries[j].peakCode, b.entries[j].peakCode);
            }
        }
    }
}
