#include "servermodel/BookRecorder.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <cmath>
#include <fstream>
#include <limits>

using namespace recording;
namespace {
class RecorderTest : public testing::Test {
  protected:
    QTemporaryDir dir;
    int64_t local = 0;
    RecorderConfig config() {
        return {dir.path().toStdString(), 100, {}, {{"near", 100, 0.5, 2, false}}, 0, 2'000'000};
    }
    std::unique_ptr<BookRecorder> make(RecorderConfig c) {
        return std::make_unique<BookRecorder>(std::move(c), [&] { return local; });
    }
    void snap(BookRecorder &r, int64_t t, std::vector<Level> l = {{true, 99, 2}, {false, 101, 4}}) {
        local = t;
        r.onSnapshot("BTC-USD", t, std::move(l));
        r.drainForTest();
    }
    void update(BookRecorder &r, int64_t t, std::vector<Level> l) {
        local = t;
        r.onUpdates("BTC-USD", t, std::move(l));
        r.drainForTest();
    }
    void tick(BookRecorder &r, int64_t t) {
        local = t;
        r.onTick(t);
        r.drainForTest();
    }
    std::vector<Hmc2Record> read(int64_t tf = 60000, const std::string &layer = "near") {
        return Hmc2Store::readRange(dir.path().toStdString(), "BTC-USD", layer, tf, 0, 10 * 3'600'000);
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
    r->onInvalid("BTC-USD", 20000, "disconnect");
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
    r->onSnapshot("BTC-USD", 10000, {{true, 99, 2}, {false, 101, 4}});
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
        r->onInvalid("", 70000, "disconnect");
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
        r->onInvalid("", 90000, "gap");
        r->drainForTest();
        tick(*r, 120000);
    }
    {
        auto r = make(c);
        snap(*r, 120000, {{true, 199, 1}, {false, 201, 1}}); // row 104 outside coverage
        tick(*r, 180000);
        r->onInvalid("", 180000, "gap");
        r->drainForTest();
        tick(*r, 3'600'000);
    }
    auto rows = read(3'600'000, "deep");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 150000);
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
    c.layers.push_back({"deep", 1000, 0.25, 4, false});
    auto r = make(c);
    snap(*r, 0, {{true, 99, 2}, {true, 30, 7}, {false, 101, 4}, {false, 390, 8}, {false, 410, 9}});
    local = 0;
    r->onSnapshot("ETH-USD", 0, {{true, 99, 3}, {false, 101, 5}});
    r->drainForTest();
    r->onInvalid("BTC-USD", 20000, "gap");
    r->drainForTest();
    tick(*r, 60000);
    auto deep = read(60000, "deep");
    ASSERT_EQ(deep.size(), 1);
    EXPECT_EQ(deep[0].observedMs, 20000);
    value(deep[0], 3, false, 7, 7);
    value(deep[0], 39, true, 8, 8);
    EXPECT_TRUE(
        std::none_of(deep[0].entries.begin(), deep[0].entries.end(), [](const auto &e) { return e.row == 41; }));
    auto eth = Hmc2Store::readRange(dir.path().toStdString(), "ETH-USD", "near", 60000, 0, 60000);
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
    r->onInvalid("BTC-USD", 20000, "disconnect");
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
    r->onSnapshot(callerName, 0, {{true, 99, 2}, {false, 101, 4}});
    callerName.assign("caller-reused-its-storage");
    local = 30000;
    r->onUpdates(expected, 30000, {{true, 99, 6}});
    tick(*r, 60000);
    auto rows = Hmc2Store::readRange(dir.path().toStdString(), expected, "near", 60000, 0, 60000);
    ASSERT_EQ(rows.size(), 1);
    value(rows[0], 99, false, 4, 6);
}
} // namespace
