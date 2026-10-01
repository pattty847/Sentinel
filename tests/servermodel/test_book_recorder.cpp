#include "servermodel/BookRecorder.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "servermodel/RecordingLive.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

using namespace recording;
namespace {
// Event offsets remain hand-readable; persistence uses a supported UTC epoch.
constexpr int64_t kEpoch = kHmc2MinMs;
class RecorderTest : public testing::Test {
  protected:
    QTemporaryDir dir;
    int64_t local = 0;
    RecorderConfig config() {
        return {dir.path().toStdString(), 100, {}, {{"near", 100, 0.5, 2, false}}, 0, 2'000'000};
    }
    std::unique_ptr<BookRecorder> make(RecorderConfig c) {
        return std::make_unique<BookRecorder>(std::move(c), [&] { return kEpoch + local; });
    }
    void snap(BookRecorder &r, int64_t t, std::vector<Level> l = {{true, 99, 2}, {false, 101, 4}}) {
        local = t;
        r.onSnapshot("BTC-USD", kEpoch + t, std::move(l));
        r.drainForTest();
    }
    void update(BookRecorder &r, int64_t t, std::vector<Level> l) {
        local = t;
        r.onUpdates("BTC-USD", kEpoch + t, std::move(l));
        r.drainForTest();
    }
    void tick(BookRecorder &r, int64_t t) {
        local = t;
        r.onTick(kEpoch + t);
        r.drainForTest();
    }
    std::vector<Hmc2Record> read(int64_t tf = 60000, const std::string &layer = "near",
                                 int64_t spanMs = 10 * 3'600'000) {
        auto records =
            Hmc2Store::readRange(dir.path().toStdString(), "BTC-USD", layer, tf, kEpoch, kEpoch + spanMs);
        for (auto &r : records)
            r.bucketStartMs -= kEpoch;
        return records;
    }
    void value(const Hmc2Record &r, int64_t row, bool ask, double twap, double peak) {
        const auto it = std::find_if(r.entries.begin(), r.entries.end(),
                                     [&](const auto &e) { return e.row == row && e.isAsk == ask; });
        ASSERT_NE(it, r.entries.end()) << "row=" << row << " ask=" << ask;
        EXPECT_NEAR(decodeSize(it->twapCode), twap, std::max(1e-9, twap * 0.001));
        EXPECT_NEAR(decodeSize(it->peakCode), peak, std::max(1e-9, peak * 0.001));
    }
};
TEST_F(RecorderTest, ExactIntegralsAndAtomicPeak) {
    auto c = config();
    c.layers[0].rowTickUnits = 1000;
    auto r = make(c);
    snap(*r, 0, {{true, 99, 2}, {true, 98, 3}, {false, 101, 4}});
    // Same row: adding 3 before removing 3 must not create a false peak of 8.
    update(*r, 30000, {{true, 99, 5}, {true, 98, 0}, {false, 101, 8}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 60000);
    value(rows[0], 9, false, 5, 5);
    value(rows[0], 10, true, 6, 8);
}
TEST_F(RecorderTest, SideCrossingAndBothSidesWithinRow) {
    auto c = config();
    c.layers[0].rowTickUnits = 1000;
    auto r = make(c);
    snap(*r, 0, {{true, 100, 2}, {false, 101, 4}});
    update(*r, 30000, {{true, 100, 0}, {false, 100, 6}, {true, 99, 3}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    value(rows[0], 10, false, 1, 2);
    value(rows[0], 10, true, 7, 10);
    value(rows[0], 9, false, 1.5, 3);
}
TEST_F(RecorderTest, MovingMidKeepsEarlyOutOfWindowContributions) {
    auto c = config();
    c.layers[0].lowFrac = 0.95;
    c.layers[0].highMult = 1.05;
    auto r = make(c);
    snap(*r, 0, {{true, 99, 2}, {false, 101, 4}, {false, 120, 10}});
    update(*r, 30000, {{true, 99, 0}, {false, 101, 0}, {true, 119, 1}, {false, 121, 1}, {false, 120, 0}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    value(rows[0], 120, true, 5, 10);
    EXPECT_EQ(rows[0].bidRowLo, 95);
    EXPECT_EQ(rows[0].askRowHi, 126);
    EXPECT_DOUBLE_EQ(rows[0].midMin, 100);
    EXPECT_DOUBLE_EQ(rows[0].midMax, 120);
}
TEST_F(RecorderTest, InvalidIntervalPreservesNumeratorAndResyncReplacesBook) {
    auto r = make(config());
    snap(*r, 0);
    r->onInvalid("BTC-USD", kEpoch + 20000, "disconnect");
    r->drainForTest();
    update(*r, 30000, {{true, 99, 200}}); // ignored until snapshot
    snap(*r, 40000, {{true, 99, 8}, {false, 102, 6}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 40000);
    EXPECT_NE(rows[0].flags & kPartial, 0);
    EXPECT_NE(rows[0].flags & kResynced, 0);
    value(rows[0], 99, false, 5, 8);
    value(rows[0], 101, true, 2, 4);
    value(rows[0], 102, true, 3, 6);
}
TEST_F(RecorderTest, SplitsAcrossTwoBoundariesBeforeApplyingMessage) {
    auto r = make(config());
    snap(*r, 10000);
    update(*r, 130000, {{true, 99, 8}});
    tick(*r, 180000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[0].observedMs, 50000);
    EXPECT_EQ(rows[1].observedMs, 60000);
    value(rows[0], 99, false, 2, 2);
    value(rows[1], 99, false, 2, 2);
    value(rows[2], 99, false, 7, 8);
}
TEST_F(RecorderTest, LatenessWatermarkBackwardAndLateEvents) {
    auto c = config();
    c.latenessMs = 2000;
    auto r = make(c);
    snap(*r, 0);
    update(*r, 61000, {{true, 99, 4}});
    EXPECT_TRUE(read().empty());
    update(*r, 59000, {{true, 99, 6}}); // backward, but preceding bucket is not committed yet
    EXPECT_EQ(r->stats().backwardSteps, 1);
    EXPECT_EQ(r->stats().lateEvents, 0);
    update(*r, 62000, {});
    ASSERT_EQ(read().size(), 1);
    update(*r, 10000, {{true, 99, 8}});
    EXPECT_EQ(r->stats().lateEvents, 1);
    EXPECT_EQ(r->stats().backwardSteps, 2);
    update(*r, 122000, {});
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_NE(rows[1].flags & kLateEvents, 0);
    value(rows[1], 99, false, (2.0 * 1000 + 6.0 * 1000 + 8.0 * 58000) / 60000, 8);
}
TEST_F(RecorderTest, IdleCloseUsesLocalMinusEnvelopeOffset) {
    auto c = config();
    c.latenessMs = 2000;
    auto r = make(c);
    local = 1'000'000;
    r->onSnapshot("BTC-USD", kEpoch + 10000, {{true, 99, 2}, {false, 101, 4}});
    r->drainForTest();
    tick(*r, 1'051'999);
    EXPECT_TRUE(read().empty());
    tick(*r, 1'052'000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 50000);
    value(rows[0], 99, false, 2, 2);
}
TEST_F(RecorderTest, UnknownGapsAndRestartNeverFillDowntime) {
    {
        auto r = make(config());
        snap(*r, 0);
        tick(*r, 60000);
        r->onInvalid("", kEpoch + 70000, "disconnect");
        r->drainForTest();
        tick(*r, 5 * 3'600'000);
    }
    {
        auto r = make(config());
        snap(*r, 7 * 3'600'000 + 30000);
        tick(*r, 7 * 3'600'000 + 60000);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[1].bucketStartMs, 60000);
    EXPECT_EQ(rows[1].observedMs, 10000);
    EXPECT_EQ(rows[2].bucketStartMs, 7 * 3'600'000);
    EXPECT_EQ(rows[2].observedMs, 30000);
}
TEST_F(RecorderTest, QueueOverflowInvalidatesUntilAcceptedSnapshot) {
    auto c = config();
    c.maxQueuedLevels = 2;
    auto r = make(c);
    snap(*r, 0);
    update(*r, 20000, {{true, 99, 7}, {false, 101, 8}, {true, 98, 1}});
    EXPECT_EQ(r->stats().queueDrops, 1);
    EXPECT_EQ(r->stats().invalidations, 1);
    update(*r, 30000, {{true, 99, 50}});
    snap(*r, 40000);
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 40000);
    value(rows[0], 99, false, 2, 2);
}
TEST_F(RecorderTest, MalformedBatchIsRejectedAtomically) {
    auto r = make(config());
    snap(*r, 0);
    update(*r, 20000, {{true, 99, 400}, {false, 101, -1}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 20000);
    value(rows[0], 99, false, 2, 2);
}
TEST_F(RecorderTest, QuantizedPricesAndUnderflow) {
    auto c = config();
    c.layers[0] = {"near", 1, 0.1, 4, false};
    auto r = make(c);
    snap(*r, 0, {{true, 0.29, 1e-9}, {false, 0.31, 2}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    ASSERT_EQ(rows[0].entries.size(), 2);
    EXPECT_EQ(rows[0].entries[0].row, 29);
    EXPECT_EQ(rows[0].entries[0].twapCode, 1);
    EXPECT_NE(rows[0].flags & kUnderflow, 0);
    EXPECT_EQ(priceUnits(0.29, 100), 29);
    EXPECT_FALSE(priceUnits(1e300, 100));
    EXPECT_FALSE(priceUnits(std::nan(""), 100));
    EXPECT_FALSE(priceUnits(-1, 100));
    EXPECT_FALSE(priceUnits(0, 100));
    EXPECT_FALSE(priceUnits(0x1p63, 1));
    EXPECT_TRUE(priceUnits(std::nextafter(0x1p63, 0), 1));
}
TEST_F(RecorderTest, HourlyCoverageWeightsDecodedSizesAndRebuildsOnRestart) {
    auto c = config();
    c.layers[0] = {"deep", 100, 0.95, 1.05, true};
    {
        auto r = make(c);
        snap(*r, 0, {{true, 99, 2}, {false, 101, 4}, {false, 104, 8}});
        tick(*r, 60000);
        snap(*r, 60000, {{true, 99, 2}, {false, 101, 4}}); // row 104 covered but zero
        r->onInvalid("", kEpoch + 90000, "gap");
        r->drainForTest();
        tick(*r, 120000);
    }
    {
        auto r = make(c);
        snap(*r, 120000, {{true, 199, 1}, {false, 201, 1}}); // row 104 outside coverage
        tick(*r, 180000);
        r->onInvalid("", kEpoch + 180000, "gap");
        r->drainForTest();
        tick(*r, 3'600'000);
    }
    auto rows = read(3'600'000, "deep");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 150000);
    EXPECT_FALSE(rows[0].flags & kApproximateCoverage);
    ASSERT_FALSE(rows[0].coverage.empty());
    const auto covered104 = std::find_if(rows[0].entries.begin(), rows[0].entries.end(),
        [](const auto &e) { return e.row == 104 && e.isAsk; });
    ASSERT_NE(covered104, rows[0].entries.end());
    EXPECT_EQ(covered104->coveredMs, 90000);
    const double decoded8 = decodeSize(encodeSize(8));
    value(rows[0], 104, true, decoded8 * 60000 / 90000, decoded8);
    value(rows[0], 99, false, decodeSize(encodeSize(2)), decodeSize(encodeSize(2)));
    value(rows[0], 199, false, decodeSize(encodeSize(1)), decodeSize(encodeSize(1)));
}
TEST_F(RecorderTest, ShutdownDropsOpenMinute) {
    {
        auto r = make(config());
        snap(*r, 0);
        update(*r, 30000, {{true, 99, 7}});
    }
    EXPECT_TRUE(read().empty());
}
TEST_F(RecorderTest, ResnapshotReplacesSizesWithoutLosingValidNumerator) {
    auto r = make(config());
    snap(*r, 0);
    snap(*r, 30000, {{true, 98, 6}, {false, 102, 8}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 60000);
    value(rows[0], 99, false, 1, 2);
    value(rows[0], 98, false, 3, 6);
}
TEST_F(RecorderTest, DeepWindowAndIndependentSymbolValidity) {
    auto c = config();
    c.layers.push_back({"deep", 500, 0.25, 4, false});
    auto r = make(c);
    snap(*r, 0, {{true, 99, 2}, {true, 30, 7}, {false, 101, 4}, {false, 390, 8}, {false, 410, 9}});
    local = 0;
    r->onSnapshot("ETH-USD", kEpoch + 0, {{true, 99, 3}, {false, 101, 5}});
    r->drainForTest();
    r->onInvalid("BTC-USD", kEpoch + 20000, "gap");
    r->drainForTest();
    tick(*r, 60000);
    auto deep = read(60000, "deep");
    ASSERT_EQ(deep.size(), 1);
    EXPECT_EQ(deep[0].observedMs, 20000);
    EXPECT_EQ(deep[0].header.rowTickUnits, 500);
    value(deep[0], 6, false, 7, 7);
    value(deep[0], 78, true, 8, 8);
    EXPECT_TRUE(
        std::none_of(deep[0].entries.begin(), deep[0].entries.end(), [](const auto &e) { return e.row == 82; }));
    auto eth = Hmc2Store::readRange(dir.path().toStdString(), "ETH-USD", "near", 60000, kEpoch, kEpoch + 60000);
    ASSERT_EQ(eth.size(), 1);
    EXPECT_EQ(eth[0].observedMs, 60000);
}
TEST_F(RecorderTest, DiskFailureCountedAndNeverBecomesCommittedRollupInput) {
    auto c = config();
    c.layers[0].hourlyRollup = true;
    auto r = make(c);
    snap(*r, 0);
    // Obstruct the symbol directory after initialization, before the first append.
    {
        std::ofstream file(std::filesystem::path(dir.path().toStdString()) / "BTC-USD");
        file << 'x';
    }
    tick(*r, 60000);
    EXPECT_EQ(r->stats().diskErrors, 1);
    EXPECT_EQ(r->stats().columnsWritten, 0);
}
TEST_F(RecorderTest, RejectedResnapshotCannotIntegrateProvisionalLevelsDuringGap) {
    auto r = make(config());
    snap(*r, 0);
    r->onInvalid("BTC-USD", kEpoch + 20000, "disconnect");
    r->drainForTest();
    snap(*r, 30000, {{true, 99, 100}}); // Missing ask: rejected while already invalid.
    snap(*r, 40000, {{true, 99, 8}, {false, 101, 4}});
    tick(*r, 60000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 40000);
    value(rows[0], 99, false, 5, 8);
}
TEST_F(RecorderTest, QueuedLongSymbolOwnsItsLifetime) {
    auto r = make(config());
    const std::string expected(80, 'X');
    std::string callerName = expected;
    r->onSnapshot(callerName, kEpoch + 0, {{true, 99, 2}, {false, 101, 4}});
    callerName.assign("caller-reused-its-storage");
    local = 30000;
    r->onUpdates(expected, kEpoch + 30000, {{true, 99, 6}});
    tick(*r, 60000);
    auto rows = Hmc2Store::readRange(dir.path().toStdString(), expected, "near", 60000, kEpoch, kEpoch + 60000);
    ASSERT_EQ(rows.size(), 1);
    value(rows[0], 99, false, 4, 6);
}
} // namespace

// Removing every level of a row must leave it exactly empty. A running sum of
// float deltas (these three sizes added then removed leave +3.9e-16) leaves a residue, which would
// keep writing a phantom underflow entry for the row every minute.
TEST_F(RecorderTest, EmptiedRowHasNoResidueEntry) {
    auto c = config();
    c.layers[0].rowTickUnits = 1000; // $10 rows
    auto r = make(c);
    snap(*r, 0, {{true, 50, 1}, {false, 101, 1}});
    update(*r, 10000, {{true, 98.1, 1.95481376}, {true, 98.2, 2.36619118}, {true, 98.7, 0.28166937}});
    update(*r, 20000, {{true, 98.1, 0}, {true, 98.2, 0}, {true, 98.7, 0}});
    tick(*r, 120000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 2u);
    value(rows[0], 9, false, 4.60267431 * 10000 / 60000, 4.60267431); // present in minute 0
    for (const auto &e : rows[1].entries) {
        EXPECT_FALSE(e.row == 9 && !e.isAsk) << "phantom entry for an emptied row";
    }
    EXPECT_EQ(rows[1].flags & kUnderflow, 0u);
}

TEST_F(RecorderTest, RestartWritesMissingPreviousHourAndLeavesExistingRollupAlone) {
    constexpr int64_t hour = 3'600'000;
    auto c = config();
    c.layers[0].hourlyRollup = true;
    Hmc2Record minute;
    minute.header = {"BTC-USD", "near", 60000, c.priceScale, 100, c.sizeScale, 0};
    minute.header.configHash = Hmc2Store::configHash(minute.header, 0.5, 2);
    minute.observedMs = 60000;
    minute.bidRowLo = minute.askRowLo = 50;
    minute.bidRowHi = minute.askRowHi = 200;
    minute.midOpen = minute.midClose = minute.midMin = minute.midMax = 100;
    minute.entries = {{99, false, encodeSize(2), encodeSize(2)}, {101, true, encodeSize(4), encodeSize(4)}};
    {
        Hmc2Store store(c.root);
        minute.bucketStartMs = kEpoch + hour - 120000;
        store.append(minute);
        minute.bucketStartMs += 60000;
        minute.entries[0].twapCode = minute.entries[0].peakCode = encodeSize(6);
        store.append(minute); // Simulate crash after minute commit, before hourly write.
    }
    EXPECT_TRUE(read(hour).empty());
    {
        auto r = make(c);
        snap(*r, hour + 10000);
        tick(*r, hour + 60000);
        EXPECT_EQ(r->stats().diskErrors, 0);
    }
    auto hours = read(hour);
    ASSERT_EQ(hours.size(), 1);
    EXPECT_EQ(hours[0].bucketStartMs, 0);
    EXPECT_EQ(hours[0].observedMs, 120000);
    value(hours[0], 99, false, (decodeSize(encodeSize(2)) + decodeSize(encodeSize(6))) / 2, 6);
    auto header = minute.header;
    header.tfMs = hour;
    const auto path = Hmc2Store::filePath(c.root, header, kEpoch);
    const auto size = std::filesystem::file_size(path);
    {
        auto r = make(c);
        snap(*r, hour + 20000);
    }
    EXPECT_EQ(std::filesystem::file_size(path), size); // Existing hour was not duplicated.
    auto minutes = read();
    ASSERT_EQ(minutes.size(), 3);
    EXPECT_EQ(minutes.back().bucketStartMs, hour);
    EXPECT_EQ(minutes.back().observedMs, 50000);
}

TEST_F(RecorderTest, HistoryReadFailureDoesNotInvalidateAnySymbol) {
    auto c = config();
    c.layers[0].hourlyRollup = true;
    std::filesystem::create_directories(c.root / "BTC-USD");
    {
        std::ofstream blocked(c.root / "BTC-USD" / "near-60000");
        blocked << 'x';
    }
    auto r = make(c);
    r->onSnapshot("ETH-USD", kEpoch, {{true, 99, 2}, {false, 101, 4}});
    r->drainForTest();
    snap(*r, 0); // Its history directory is unreadable, but the new book is valid.
    EXPECT_EQ(r->stats().invalidations, 0);
    tick(*r, 60000);
    auto eth = Hmc2Store::readRange(c.root, "ETH-USD", "near", 60000, kEpoch, kEpoch + 60000);
    ASSERT_EQ(eth.size(), 1);
    EXPECT_EQ(eth[0].observedMs, 60000);
    EXPECT_EQ(r->stats().invalidations, 0);
    EXPECT_EQ(r->stats().diskErrors, 1);
    std::filesystem::remove(c.root / "BTC-USD" / "near-60000");
    tick(*r, 120000);
    auto btc = read();
    ASSERT_EQ(btc.size(), 1);
    EXPECT_EQ(btc[0].bucketStartMs, 60000);
    EXPECT_EQ(btc[0].observedMs, 60000);
    EXPECT_EQ(r->stats().diskErrors, 1);
}

#include "Hmc2LegacyFixture.hpp"
TEST_F(RecorderTest, RestartAcrossHourBoundaryRetainsSchemaThreeAndWritesSchemaFourGeneration) {
    constexpr int64_t hour = 3600000;
    auto cfg = config();
    cfg.layers[0].name = "deep"; cfg.layers[0].hourlyRollup = true;
    Hmc2Record old;
    old.header = {"BTC-USD", "deep", 60000, cfg.priceScale, cfg.layers[0].rowTickUnits, cfg.sizeScale, 0};
    old.header.configHash = Hmc2Store::configHash(old.header, cfg.layers[0].lowFrac, cfg.layers[0].highMult);
    old.header.tfMs = hour; old.bucketStartMs = kEpoch; old.observedMs = 60000;
    old.bidRowLo = old.askRowLo = 50; old.bidRowHi = old.askRowHi = 200;
    old.entries = {{99, false, encodeSize(2), encodeSize(2)}};
    const auto path = Hmc2Store::filePath(cfg.root, old.header, kEpoch);
    recording_fixture::schema3Hour(path, old);
    const auto originalSize = std::filesystem::file_size(path);
    {
        auto recorder = make(cfg);
        snap(*recorder, 2 * hour - 60000);
        tick(*recorder, 2 * hour);
        EXPECT_EQ(recorder->stats().diskErrors, 0);
    }
    { // Actual recorder destruction/restart on the other side of the boundary.
        auto recorder = make(cfg);
        snap(*recorder, 2 * hour + 30000);
        tick(*recorder, 3 * hour);
        EXPECT_EQ(recorder->stats().diskErrors, 0);
    }
    const auto hours = read(hour, "deep");
    ASSERT_EQ(hours.size(), 3);
    EXPECT_TRUE(hours[0].flags & kApproximateCoverage);
    for (size_t n = 1; n < hours.size(); ++n) {
        EXPECT_EQ(hours[n].bucketStartMs, n * hour);
        EXPECT_FALSE(hours[n].flags & kApproximateCoverage);
        ASSERT_FALSE(hours[n].coverage.empty());
        EXPECT_GT(hours[n].entries[0].coveredMs, 0);
    }
    EXPECT_EQ(std::filesystem::file_size(path), originalSize);
    std::ifstream legacy(path, std::ios::binary); legacy.seekg(4); EXPECT_EQ(legacy.get(), 3);
    const auto generation = Hmc2Store::filePath(cfg.root, old.header, kEpoch, 1);
    std::ifstream current(generation, std::ios::binary); current.seekg(4); EXPECT_EQ(current.get(), 4);
    EXPECT_FALSE(std::filesystem::exists(Hmc2Store::filePath(cfg.root, old.header, kEpoch, 2)));
}

TEST_F(RecorderTest, PublicationIsWorkerOwnedAndConstantBookMatchesClose) {
    std::vector<std::shared_ptr<const Hmc2Record>> publications;
    const auto producer = std::this_thread::get_id();
    auto c = config();
    c.publisher = [&](auto record) {
        EXPECT_NE(std::this_thread::get_id(), producer);
        publications.push_back(std::move(record));
    };
    auto r = make(c);
    snap(*r, 0);
    tick(*r, 30'000);
    ASSERT_EQ(publications.size(), 1);
    const auto provisional = publications.back();
    EXPECT_TRUE(provisional->flags & kProvisional);
    EXPECT_EQ(provisional->observedMs, 30'000);
    value(*provisional, 99, false, 2, 2);
    value(*provisional, 101, true, 4, 4);
    tick(*r, 30'250);
    EXPECT_EQ(publications.size(), 1); // one-second coalescing
    tick(*r, 60'000);
    ASSERT_EQ(publications.size(), 3); // finished-pending publication, then commit
    const auto committed = publications.back();
    EXPECT_FALSE(committed->flags & kProvisional);
    EXPECT_EQ(committed->observedMs, 60'000);
    EXPECT_EQ(committed->bidRowLo, provisional->bidRowLo);
    EXPECT_EQ(committed->bidRowHi, provisional->bidRowHi);
    for (const auto& e : committed->entries) {
        const auto found = std::find_if(provisional->entries.begin(), provisional->entries.end(),
            [&](const auto& p) { return p.row == e.row && p.isAsk == e.isAsk; });
        ASSERT_NE(found, provisional->entries.end());
        EXPECT_EQ(found->twapCode, e.twapCode);
        EXPECT_EQ(found->peakCode, e.peakCode);
    }
}

TEST_F(RecorderTest, PublicationExcludesInvalidTimeAndResyncDoesNotInventLiquidity) {
    std::vector<std::shared_ptr<const Hmc2Record>> publications;
    auto c = config();
    c.publisher = [&](auto record) { publications.push_back(std::move(record)); };
    auto r = make(c);
    snap(*r, 0);
    local = 10'000;
    r->onInvalid("BTC-USD", kEpoch + local, "test gap");
    r->drainForTest();
    tick(*r, 20'000);
    ASSERT_FALSE(publications.empty());
    EXPECT_EQ(publications.back()->observedMs, 10'000);
    value(*publications.back(), 99, false, 2, 2);
    snap(*r, 25'000, {{true, 99, 100}}); // rejected one-sided snapshot
    tick(*r, 30'000);
    EXPECT_EQ(publications.back()->observedMs, 10'000);
    value(*publications.back(), 99, false, 2, 2);
    snap(*r, 40'000, {{true, 99, 6}, {false, 101, 8}});
    tick(*r, 50'000);
    EXPECT_EQ(publications.back()->observedMs, 20'000);
    EXPECT_TRUE(publications.back()->flags & kResynced);
    value(*publications.back(), 99, false, 4, 6);
    tick(*r, 60'000);
    EXPECT_EQ(publications.back()->observedMs, 30'000);
    EXPECT_FALSE(publications.back()->flags & kProvisional);
    value(*publications.back(), 99, false, 14.0 / 3, 6);
}

TEST_F(RecorderTest, FirstCommitWatermarkPreservesPendingMinuteInColdLiveView) {
    LiveCache cache;
    auto c = config();
    c.latenessMs = 2000;
    c.publisher = [&](auto r) { cache.publish(std::move(r)); };
    auto recorder = make(c);
    Hmc2Reader reader(c.root);
    LiveBuilder builder({"BTC-USD", "near", 300000, {90, 1, 20}, 1});
    snap(*recorder, 0);
    tick(*recorder, 59000);
    auto before = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(before.columns.size(), 1);
    EXPECT_EQ(before.columns[0].observedMs, 59000);
    tick(*recorder, 61000);
    const auto pending = cache.snapshot("BTC-USD", "near");
    EXPECT_EQ(pending.committedThroughMs, kEpoch);
    EXPECT_EQ(pending.provisional.size(), 2);
    EXPECT_TRUE(pending.committed.empty());
    auto atBoundary = builder.build(reader, pending);
    ASSERT_EQ(atBoundary.columns.size(), 1);
    EXPECT_EQ(atBoundary.columns[0].observedMs, 61000);
    tick(*recorder, 62000);
    auto after = builder.build(reader, cache.snapshot("BTC-USD", "near"));
    ASSERT_EQ(after.columns.size(), 1);
    EXPECT_EQ(after.columns[0].observedMs, 62000);
    EXPECT_EQ(after.columns[0].cells, atBoundary.columns[0].cells);
}

TEST_F(RecorderTest, PublisherFailureDoesNotLoseCommittedMinuteOrHourRollup) {
    auto c = config();
    c.layers[0].hourlyRollup = true;
    c.publisher = [](auto) {};
    c.beforePublicationForTest = [](bool provisional) { if (!provisional) throw std::bad_alloc(); };
    auto recorder = make(c);
    snap(*recorder, 3540000);
    tick(*recorder, 3600000);
    EXPECT_EQ(recorder->stats().diskErrors, 0);
    EXPECT_EQ(recorder->stats().columnsWritten, 2);
    EXPECT_EQ(read().size(), 1);
    EXPECT_EQ(read(3600000).size(), 1);
}

// 2026-09-30 incident: an update batch left the ask side empty at 23:59:28.559 UTC.
// The recorder invalidated itself, ignored every later update, and wrote nothing
// for 22 minutes (no next-day file) because no snapshot ever came.
TEST_F(RecorderTest, OneSidedUpdateDoesNotStopRecordingAcrossUtcDay) {
    constexpr int64_t D = 86'400'000; // 2000-01-02T00:00Z relative to kEpoch
    auto r = make(config());
    snap(*r, D - 150000);
    update(*r, D - 31441, {{false, 101, 0}}); // ask side empty
    update(*r, D - 31000, {{false, 101, 4}}); // refilled by the next batch
    update(*r, D + 30000, {{true, 99, 3}});
    tick(*r, D + 180000);
    auto rows = read(60000, "near", 2 * D);
    // Before the fix: 3 rows, observedMs 30000/60000/28559, invalidations=1.
    const std::vector<int64_t> buckets{D - 180000, D - 120000, D - 60000, D, D + 60000, D + 120000};
    const std::vector<uint32_t> observed{30000, 60000, 60000, 60000, 60000, 60000};
    for (size_t i = 0; i < std::min(rows.size(), buckets.size()); ++i) {
        EXPECT_EQ(rows[i].bucketStartMs, buckets[i]) << i;
        EXPECT_EQ(rows[i].observedMs, observed[i]) << i;
    }
    EXPECT_EQ(r->stats().invalidations, 0);
    ASSERT_EQ(rows.size(), 6);
    value(rows[2], 99, false, 2, 2);
    value(rows[2], 101, true, 4.0 * 59559 / 60000, 4);
    EXPECT_DOUBLE_EQ(rows[2].midMin, 100);
    value(rows[3], 99, false, 2.5, 3);
    value(rows[3], 101, true, 4, 4);
    EXPECT_TRUE(std::filesystem::exists(dir.path().toStdString() + "/BTC-USD/near-60000/2000-01-02.hmc2"));
}
// A one-sided book that crosses a minute boundary keeps the last two-sided mid
// for the new window (no mid from an empty side, no UB on the empty map).
TEST_F(RecorderTest, EmptySideAcrossMinuteBoundaryKeepsLastMid) {
    auto r = make(config());
    snap(*r, 0);
    update(*r, 30000, {{false, 101, 0}});
    tick(*r, 60000);
    update(*r, 100000, {{false, 102, 5}});
    tick(*r, 180000);
    auto rows = read();
    ASSERT_EQ(rows.size(), 3);
    for (const auto &row : rows)
        EXPECT_EQ(row.observedMs, 60000);
    EXPECT_EQ(r->stats().invalidations, 0);
    value(rows[0], 101, true, 2, 4);
    EXPECT_DOUBLE_EQ(rows[1].midOpen, 100);
    EXPECT_DOUBLE_EQ(rows[1].midMin, 100);
    EXPECT_DOUBLE_EQ(rows[1].midClose, 100.5);
    value(rows[1], 102, true, 5.0 * 20000 / 60000, 5);
    value(rows[1], 99, false, 2, 2);
    EXPECT_DOUBLE_EQ(rows[2].midOpen, 100.5);
    value(rows[2], 102, true, 5, 5);
}
