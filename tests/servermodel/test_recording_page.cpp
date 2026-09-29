#include "servermodel/RecordingPage.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <chrono>
#include <iostream>
#include <fstream>
using namespace recording;
namespace {
constexpr int64_t epoch = kHmc2MinMs, minute = 60000, hour = 3600000;
class PageTest : public testing::Test {
  protected:
    QTemporaryDir dir;
    std::filesystem::path root() { return dir.path().toStdString(); }
    Hmc2Record record(int64_t offset = 0, std::string layer = "near", int64_t tf = minute) {
        Hmc2Record r;
        r.header = {"BTC-USD", layer, tf, 100, layer == "near" ? 100 : 500, {}, 42};
        r.bucketStartMs = epoch + offset;
        r.observedMs = static_cast<uint32_t>(tf);
        r.bidRowLo = r.askRowLo = 0;
        r.bidRowHi = r.askRowHi = 100;
        r.entries = {{10, false, encodeSize(2), encodeSize(9), r.observedMs},
                     {10, true, encodeSize(3), encodeSize(10), r.observedMs}};
        if (tf == hour)
            r.coverage = {{0, 100, false, r.observedMs}, {0, 100, true, r.observedMs}};
        return r;
    }
    BuildRequest request(double lo = 10, double hi = 12, uint32_t rows = 2, int64_t tf = minute) {
        BuildRequest q;
        q.symbol = "BTC-USD";
        q.priceLo = lo;
        q.priceHi = hi;
        q.rows = rows;
        q.tfMs = tf;
        q.budgets = {};
        return q;
    }
    void write(std::vector<Hmc2Record> records) {
        Hmc2Store store(root());
        for (const auto &r : records) store.append(r);
    }
    static double quantity(const ServedColumn &c, size_t row) { return c.quantities[row] * c.quantityScale; }
    static bool valid(const ServedColumn &c, size_t row) { return (c.validity[row / 8] >> (row % 8)) & 1; }
};
TEST_F(PageTest, TickChoiceClipsNeitherUpperEdgeNorExactGridMultiples) {
    write({record(), record(0, "deep")});
    auto q = request(9.5, 11.5, 2);
    auto result = buildPage(root(), q);
    EXPECT_EQ(result.status, BuildStatus::Complete);
    EXPECT_EQ(result.band.tick, 2);
    EXPECT_EQ(result.band.lo, 8);
    EXPECT_EQ(result.band.rows, 2);
    q = request(10, 12, 2);
    result = buildPage(root(), q);
    EXPECT_EQ(result.band.tick, 1);
    ASSERT_EQ(result.columns.size(), 1);
    EXPECT_EQ(result.columns[0].cells[0], 0); // [11,12)
    EXPECT_TRUE(isAsk(result.columns[0].cells[1])); // exactly 10 -> [10,11)
    q = request(9.5, 11.5, 2);
    q.displayTick = 1;
    EXPECT_EQ(buildPage(root(), q).status, BuildStatus::InvalidRequest);
}
TEST_F(PageTest, LayerSelectionAndAbsoluteGrid) {
    write({record(), record(0, "deep")});
    auto q = request(95, 115, 2);
    auto r = buildPage(root(), q);
    EXPECT_EQ(r.layer, "deep");
    EXPECT_EQ(r.band.tick, 20);
    EXPECT_EQ(r.band.lo, 80);
    q = request(10, 14, 2);
    EXPECT_EQ(buildPage(root(), q).layer, "near");
    q.displayTick = 10;
    EXPECT_EQ(buildPage(root(), q).layer, "deep");
    q.displayTick = 11;
    EXPECT_EQ(buildPage(root(), q).status, BuildStatus::InvalidRequest);
}
TEST_F(PageTest, AutomaticBandUsesCustomDeepGrid) {
    auto deep = record(0, "deep");
    deep.header.rowTickUnits = 400;
    write({record(), deep});
    auto p = buildPage(root(), request(0, 10, 2));
    EXPECT_EQ(p.status, BuildStatus::Complete);
    EXPECT_EQ(p.layer, "deep");
    EXPECT_EQ(p.band.tick, 20); // $5 and $10 are not multiples of the custom $4 native tick
    EXPECT_EQ(p.band.rows, 1);
}
TEST_F(PageTest, AutomaticTicksFollowSharedLadderAndSelectDeepAtFive) {
    write({record(), record(0, "deep"), record(0, "deep", hour)});
    for (const auto tf : {minute, hour}) {
        for (const auto tick : {5., 10., 20., 25., 50., 100., 200., 250.}) {
            auto q = request(0, tick * 2, 2, tf);
            auto p = buildPage(root(), q);
            EXPECT_EQ(p.status, BuildStatus::Complete);
            EXPECT_EQ(p.band.tick, tick);
            EXPECT_EQ(p.layer, "deep");
        }
    }
    auto q = request(19, 51, 2); // $20 and $25 need three aligned rows; $50 fits
    EXPECT_EQ(buildPage(root(), q).band.tick, 50);
    q = request(0, 21, 1);
    EXPECT_EQ(buildPage(root(), q).band.tick, 25);
}
TEST_F(PageTest, CustomNativeWithoutLargerLadderTickFailsBoundedly) {
    auto deep = record(0, "deep");
    deep.header.rowTickUnits = 300;
    write({record(), deep});
    EXPECT_EQ(buildPage(root(), request(0, 10, 2)).status, BuildStatus::IncompatibleGrid);
}
TEST_F(PageTest, MixedGenerationsKeepOwnGridAndOnlyIncompatibleColumnsAreUnknown) {
    auto old = record(0, "deep"), next = record(minute, "deep");
    old.header.rowTickUnits = 1000;
    old.header.configHash = Hmc2Store::configHash(old.header, .25, 4);
    next.header.configHash = Hmc2Store::configHash(next.header, .25, 4);
    old.entries = {{10, false, encodeSize(2), 0}};
    next.entries = {{20, false, encodeSize(6), 0}, {21, false, encodeSize(4), 0}};
    ASSERT_NE(old.header.configHash, next.header.configHash);
    write({old, next});
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), next.header, next.bucketStartMs, 1)));
    auto q = request(100, 150, 1);
    auto p = buildPage(root(), q);
    ASSERT_EQ(p.status, BuildStatus::Complete);
    ASSERT_EQ(p.columns.size(), 2);
    EXPECT_EQ(p.band.tick, 50);
    EXPECT_TRUE(valid(p.columns[0], 0));
    EXPECT_TRUE(valid(p.columns[1], 0));
    EXPECT_NEAR(quantity(p.columns[0], 0), decodeSize(encodeSize(2)), 1e-5);
    EXPECT_NEAR(quantity(p.columns[1], 0), decodeSize(encodeSize(6)) + decodeSize(encodeSize(4)), 1e-5);
    q = request(100, 125, 1);
    p = buildPage(root(), q);
    ASSERT_EQ(p.status, BuildStatus::Complete);
    ASSERT_EQ(p.columns.size(), 2);
    EXPECT_EQ(p.band.tick, 25);
    EXPECT_FALSE(valid(p.columns[0], 0));
    EXPECT_EQ(p.columns[0].cells[0], 0);
    EXPECT_EQ(p.columns[0].quantities[0], 0);
    EXPECT_EQ(p.columns[0].observedMs, minute);
    EXPECT_TRUE(valid(p.columns[1], 0));
    EXPECT_TRUE(p.exhausted);
    EXPECT_EQ(p.scannedStartMs, epoch);
    EXPECT_EQ(p.scannedEndMs, epoch + 2 * minute);
    q.tfMs = 5 * minute;
    p = buildPage(root(), q);
    ASSERT_EQ(p.status, BuildStatus::Complete);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_FALSE(valid(p.columns[0], 0));
    EXPECT_EQ(p.columns[0].observedMs, 2 * minute);
}
TEST_F(PageTest, MixedNativeRollupsWeightSidesBeforeChoosingDominantSide) {
    for (const auto tf : {minute, hour}) {
        QTemporaryDir mixed;
        auto old = record(0, "deep", tf), next = record(tf, "deep", tf);
        old.header.rowTickUnits = 1000;
        old.observedMs = static_cast<uint32_t>(tf / 2);
        old.entries = {{10, false, encodeSize(12), 0, old.observedMs}};
        next.entries = {{20, true, encodeSize(4), 0, next.observedMs},
                        {21, true, encodeSize(6), 0, next.observedMs}};
        if (tf == hour) old.coverage = {{0, 100, false, old.observedMs}, {0, 100, true, old.observedMs}};
        { Hmc2Store store(mixed.path().toStdString()); store.append(old); store.append(next); }
        auto q = request(100, 150, 1, tf == minute ? 5 * minute : 4 * hour);
        auto p = buildPage(mixed.path().toStdString(), q);
        ASSERT_EQ(p.status, BuildStatus::Complete);
        ASSERT_EQ(p.columns.size(), 1);
        EXPECT_EQ(p.columns[0].observedMs, tf * 3 / 2);
        EXPECT_TRUE(valid(p.columns[0], 0));
        EXPECT_TRUE(isAsk(p.columns[0].cells[0]));
        EXPECT_NEAR(quantity(p.columns[0], 0), (decodeSize(encodeSize(4)) + decodeSize(encodeSize(6))) * 2 / 3, 1e-5);
    }
}
TEST_F(PageTest, MixedRollupValidityRequiresCoverageFromBothNativeGrids) {
    auto old = record(0, "deep"), next = record(minute, "deep");
    old.header.rowTickUnits = 1000;
    old.bidRowHi = 14; // [100,150) covered, [150,200) unknown on old bids
    old.entries = {{10, false, encodeSize(2), 0}};
    next.entries = {{20, false, encodeSize(6), 0}};
    write({old, next});
    auto q = request(100, 200, 2, 5 * minute);
    auto p = buildPage(root(), q);
    ASSERT_EQ(p.status, BuildStatus::Complete);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_FALSE(valid(p.columns[0], 0));
    EXPECT_TRUE(valid(p.columns[0], 1));
    EXPECT_NEAR(quantity(p.columns[0], 1), (decodeSize(encodeSize(2)) + decodeSize(encodeSize(6))) / 2, 1e-5);
}
TEST_F(PageTest, SumsDecodedSizesDominantSideTieAndDescendingRows) {
    auto r = record();
    r.entries = {{10, false, encodeSize(2), encodeSize(100)}, {10, true, encodeSize(2), encodeSize(100)},
                 {11, false, encodeSize(3), encodeSize(100)}, {11, true, encodeSize(3), encodeSize(100)},
                 {12, true, encodeSize(8), encodeSize(100)}};
    write({r});
    auto q = request(10, 14, 2);
    auto page = buildPage(root(), q);
    ASSERT_EQ(page.columns.size(), 1);
    const auto &c = page.columns[0];
    EXPECT_TRUE(isAsk(c.cells[0]));
    EXPECT_FALSE(isAsk(c.cells[1]));
    const auto sum = decodeSize(encodeSize(2)) + decodeSize(encodeSize(3));
    EXPECT_EQ(c.cells[1], encodeSize(sum));
    EXPECT_NEAR(quantity(c, 1), sum, c.quantityScale);
    EXPECT_TRUE(valid(c, 0));
    EXPECT_TRUE(valid(c, 1));
}
TEST_F(PageTest, UnknownOutsideNearAndIncompleteConstituentRows) {
    auto r = record();
    r.bidRowLo = r.askRowLo = 10;
    r.bidRowHi = r.askRowHi = 11;
    write({r});
    auto page = buildPage(root(), request(8, 14, 3));
    ASSERT_EQ(page.columns.size(), 1);
    EXPECT_FALSE(valid(page.columns[0], 0));
    EXPECT_TRUE(valid(page.columns[0], 1));
    EXPECT_FALSE(valid(page.columns[0], 2));
    r.bucketStartMs += minute;
    r.askRowHi = 10; // one missing constituent side invalidates [10,12)
    write({r});
    page = buildPage(root(), request(8, 14, 3));
    EXPECT_FALSE(valid(page.columns.back(), 1));
}
TEST_F(PageTest, FiveAndFifteenMinuteMeansWeightObservationAndCoveredZeros) {
    std::vector<Hmc2Record> rows;
    for (int n = 0; n < 15; ++n) {
        auto r = record(n * minute);
        r.entries.clear();
        if (n == 0) {
            r.observedMs = 30000;
            r.entries = {{10, false, encodeSize(10), 0}};
        }
        rows.push_back(r);
    }
    write(rows);
    for (const auto tf : {5 * minute, 15 * minute}) {
        auto q = request(10, 11, 1, tf);
        q.endMs = epoch;
        auto result = buildPage(root(), q);
        ASSERT_EQ(result.columns.size(), 1);
        const auto &c = result.columns[0];
        EXPECT_EQ(c.observedMs, tf - 30000);
        EXPECT_TRUE(c.flags & kPartial);
        EXPECT_TRUE(valid(c, 0)); // partial observation is explicit, observed rows still valid
        EXPECT_NEAR(quantity(c, 0), decodeSize(encodeSize(10)) * 30000 / (tf - 30000), 1e-5);
    }
}
TEST_F(PageTest, BoundsChangesInvalidateOnlyAffectedRowsAndUseIndividualDenominators) {
    auto a = record(), b = record(minute);
    a.entries = {{10, false, encodeSize(2), 0}};
    b.entries = {{11, false, encodeSize(6), 0}};
    b.bidRowLo = b.askRowLo = 11;
    write({a, b});
    auto q = request(10, 12, 2, 5 * minute);
    auto p = buildPage(root(), q);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_TRUE(valid(p.columns[0], 0));
    EXPECT_FALSE(valid(p.columns[0], 1));
    EXPECT_NEAR(quantity(p.columns[0], 0), decodeSize(encodeSize(6)) / 2, 1e-4);
    EXPECT_NEAR(quantity(p.columns[0], 1), decodeSize(encodeSize(2)), 1e-4);
}
TEST_F(PageTest, MultiHourCoverageExactIncludingAbsentZeroRows) {
    auto a = record(0, "deep", hour), b = record(hour, "deep", hour);
    a.observedMs = b.observedMs = 120000;
    a.entries = {{10, false, encodeSize(8), 0, 30000}};
    a.coverage = {{10, 10, false, 30000}, {10, 10, true, 120000}};
    b.entries = {{10, false, encodeSize(2), 0, 120000}};
    b.coverage = {{10, 10, false, 120000}, {10, 10, true, 120000}};
    write({a, b});
    auto p = buildPage(root(), request(50, 55, 1, 4 * hour));
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_NEAR(quantity(p.columns[0], 0), (decodeSize(encodeSize(8)) * 30000 +
                decodeSize(encodeSize(2)) * 120000) / 150000, 1e-5);
    EXPECT_FALSE(valid(p.columns[0], 0));
    // No entry in the second hour still supplies a zero denominator contribution.
    b.entries.clear();
    write({b});
    p = buildPage(root(), request(50, 55, 1, 4 * hour));
    EXPECT_NEAR(quantity(p.columns[0], 0), decodeSize(encodeSize(8)) * 30000 / 150000, 1e-5);
}
TEST_F(PageTest, CurrentHourTailAndPersistedHourNeverDoubleCount) {
    auto h = record(0, "deep", hour);
    h.entries = {{10, false, encodeSize(2), 0, static_cast<uint32_t>(hour)}};
    auto stale = record(59 * minute, "deep"), tail = record(hour, "deep");
    stale.entries = {{10, false, encodeSize(1000), 0}};
    tail.entries = {{10, false, encodeSize(8), 0}};
    write({h, stale, tail});
    auto q = request(50, 55, 1, 4 * hour);
    auto p = buildPage(root(), q);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_EQ(p.columns[0].observedMs, hour + minute);
    EXPECT_NEAR(quantity(p.columns[0], 0), (decodeSize(encodeSize(2)) * hour +
                decodeSize(encodeSize(8)) * minute) / (hour + minute), 1e-5);
    h.bucketStartMs += hour;
    write({h});
    p = buildPage(root(), q);
    EXPECT_EQ(p.columns[0].observedMs, 2 * hour);
    EXPECT_NEAR(quantity(p.columns[0], 0), decodeSize(encodeSize(2)), 1e-5);
}
TEST_F(PageTest, BudgetPageIsCompletedSuffixNotExhaustedAndCanContinue) {
    std::vector<Hmc2Record> records;
    for (int i = 0; i < 5; ++i) {
        auto r = record(i * minute);
        r.entries = {{10, false, 0, encodeSize(1)}}; // force independent keyframes
        records.push_back(r);
    }
    write(records);
    Hmc2Reader reader(root());
    ReadControl warm;
    reader.availability("BTC-USD", "near", minute, warm);
    auto q = request();
    q.budgets.maxSourceRecords = 2;
    auto p = buildPage(reader, q);
    EXPECT_EQ(p.status, BuildStatus::Budget);
    ASSERT_EQ(p.columns.size(), 2);
    EXPECT_FALSE(p.exhausted);
    EXPECT_EQ(p.scannedStartMs, epoch + 3 * minute);
    EXPECT_EQ(p.scannedEndMs, epoch + 5 * minute);
    EXPECT_EQ(p.nextEnd, epoch + 2 * minute);
    q.endMs = p.nextEnd;
    q.budgets = {};
    p = buildPage(reader, q);
    ASSERT_EQ(p.columns.size(), 3);
    EXPECT_TRUE(p.exhausted);
    EXPECT_EQ(p.columns.back().bucketStartMs, epoch + 2 * minute);
}
TEST_F(PageTest, IncompleteRollupDoesNotClaimItsScannedInterval) {
    write({record(), record(minute), record(5 * minute)});
    Hmc2Reader reader(root());
    ReadControl warm;
    reader.availability("BTC-USD", "near", minute, warm);
    auto q = request(10, 12, 2, 5 * minute);
    q.budgets.maxSourceRecords = 2;
    auto p = buildPage(reader, q);
    EXPECT_EQ(p.status, BuildStatus::Budget);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_EQ(p.scannedStartMs, epoch + 5 * minute);
    EXPECT_EQ(p.nextEnd, epoch);
    EXPECT_FALSE(p.exhausted);
}
TEST_F(PageTest, CancellationStopsButZeroBudgetsAreClampedForProgress) {
    write({record()});
    auto q = request();
    std::stop_source stop;
    stop.request_stop();
    auto p = buildPage(root(), q, stop.get_token());
    EXPECT_EQ(p.status, BuildStatus::Cancelled);
    EXPECT_FALSE(p.exhausted);
    EXPECT_TRUE(p.columns.empty());
    Hmc2Reader reader(root());
    q.budgets.maxSourceRecords = 0;
    q.budgets.maxEntriesVisited = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        p = buildPage(reader, q);
        if (!p.columns.empty()) break;
        EXPECT_EQ(p.status, BuildStatus::Budget);
        EXPECT_FALSE(p.exhausted);
    }
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_EQ(p.scannedStartMs, epoch);
    EXPECT_EQ(p.scannedEndMs, epoch + minute);
    ReadControl clamped;
    clamped.limits = {0, 0, 0};
    EXPECT_TRUE(clamped.poll());
    EXPECT_EQ(clamped.limits.maxSourceRecords, 1);
    EXPECT_EQ(clamped.limits.maxEntriesVisited, 1);
    EXPECT_EQ(clamped.limits.maxWallMs, 10);
}
TEST_F(PageTest, AvailabilityCachedAndInvalidatedOnAppendAndGeneration) {
    write({record()});
    Hmc2Reader reader(root());
    ReadControl a;
    auto available = reader.availability("BTC-USD", "near", minute, a);
    EXPECT_EQ(available.oldestMs, epoch);
    EXPECT_EQ(available.latestMs, epoch);
    ReadControl cached;
    reader.availability("BTC-USD", "near", minute, cached);
    EXPECT_EQ(cached.sourceRecords, 0);
    auto newer = record(minute);
    newer.header.configHash++;
    write({newer});
    ReadControl updated;
    available = reader.availability("BTC-USD", "near", minute, updated);
    EXPECT_EQ(available.latestMs, epoch + minute);
    EXPECT_EQ(available.oldestMs, epoch);
}
TEST_F(PageTest, StreamingReconstructsDeltasAndOrdersLastValidGenerations) {
    std::vector<Hmc2Record> records;
    for (int i = 0; i < 32; ++i) {
        auto r = record(i * minute);
        r.entries[0].twapCode = encodeSize(2 + i);
        records.push_back(r);
    }
    write(records);
    auto replacement = record(17 * minute);
    replacement.header.configHash++;
    replacement.entries[0].twapCode = encodeSize(100);
    write({replacement, record(16 * minute)}); // nonchronological generation/file append
    Hmc2Reader reader(root());
    ReadControl control;
    std::vector<Hmc2Record> streamed;
    auto scan = reader.visit("BTC-USD", "near", minute, epoch + 13 * minute, epoch + 20 * minute,
                             [&](const auto &r) { streamed.push_back(r); }, control);
    auto reference = Hmc2Store::readRange(root(), "BTC-USD", "near", minute, epoch + 13 * minute, epoch + 20 * minute);
    ASSERT_EQ(streamed.size(), reference.size());
    for (size_t i = 0; i < streamed.size(); ++i) {
        EXPECT_EQ(streamed[i].bucketStartMs, reference[i].bucketStartMs);
        EXPECT_EQ(streamed[i].entries[0].twapCode, reference[i].entries[0].twapCode);
    }
    EXPECT_EQ(scan.scannedStartMs, epoch + 13 * minute);
    EXPECT_EQ(scan.scannedEndMs, epoch + 20 * minute);
    EXPECT_EQ(scan.status, ReadStatus::Complete);
}
TEST_F(PageTest, StreamingBudgetsAndCancellationReportOnlyVisitedPrefix) {
    write({record(), record(minute), record(2 * minute)});
    Hmc2Reader reader(root());
    ReadControl warm;
    reader.availability("BTC-USD", "near", minute, warm);
    ReadControl control;
    control.limits.maxSourceRecords = 1;
    int seen = 0;
    auto result = reader.visit("BTC-USD", "near", minute, epoch, epoch + 3 * minute,
                               [&](const auto &) { ++seen; }, control);
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(result.status, ReadStatus::Budget);
    EXPECT_EQ(result.scannedEndMs, epoch + minute);
    std::stop_source stop;
    ReadControl cancelled{{}, stop.get_token()};
    result = reader.visit("BTC-USD", "near", minute, epoch, epoch + 3 * minute,
                           [&](const auto &) { stop.request_stop(); }, cancelled);
    EXPECT_EQ(result.status, ReadStatus::Cancelled);
    EXPECT_EQ(result.scannedEndMs, epoch + minute);
}
TEST_F(PageTest, EmptyAndInvalidRequests) {
    EXPECT_TRUE(buildPage(root(), request()).exhausted);
    for (auto tf : {90000LL, 5400000LL}) {
        auto q = request(); q.tfMs = tf;
        EXPECT_EQ(buildPage(root(), q).status, BuildStatus::InvalidRequest);
    }
    auto q = request(); q.count = 1025;
    EXPECT_EQ(buildPage(root(), q).status, BuildStatus::InvalidRequest);
}
TEST_F(PageTest, ColdIndexDiscoveryHonorsRecordBudget) {
    write({record(), record(minute), record(2 * minute)});
    Hmc2Reader reader(root());
    ReadControl control;
    control.limits.maxSourceRecords = 1;
    int seen = 0;
    const auto result = reader.visit("BTC-USD", "near", minute, epoch, epoch + 3 * minute,
                                     [&](const auto &) { ++seen; }, control);
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(control.sourceRecords, 1);
    EXPECT_EQ(result.status, ReadStatus::Budget);
    EXPECT_EQ(result.scannedStartMs, result.scannedEndMs);
}
TEST_F(PageTest, DecimalTicksPreserveExactEdgesAndExtremeBandsReject) {
    auto r = record();
    r.header.rowTickUnits = 10; // 0.1
    r.entries = {{3, false, encodeSize(2), 0}};
    write({r});
    auto q = request(0.3, 0.5, 2);
    q.displayTick = 0.1;
    auto p = buildPage(root(), q);
    EXPECT_EQ(p.status, BuildStatus::Complete);
    EXPECT_DOUBLE_EQ(p.band.lo, 0.3);
    EXPECT_EQ(p.band.rows, 2);
    ASSERT_EQ(p.columns.size(), 1);
    EXPECT_NEAR(quantity(p.columns[0], 1), decodeSize(encodeSize(2)), 1e-6);
    q.displayTick = 1e300;
    EXPECT_EQ(buildPage(root(), q).status, BuildStatus::InvalidRequest);
}
TEST_F(PageTest, StreamAcrossDaysAndLargeAlignedDisplayRows) {
    write({record(86400000 - minute), record(86400000)});
    Hmc2Reader reader(root());
    ReadControl control;
    std::vector<int64_t> buckets;
    const auto r = reader.visit("BTC-USD", "near", minute, epoch + 86400000 - minute, epoch + 86400000 + minute,
                                [&](const auto &record) { buckets.push_back(record.bucketStartMs); }, control);
    ASSERT_EQ(buckets.size(), 2);
    EXPECT_LT(buckets[0], buckets[1]);
    EXPECT_EQ(r.status, ReadStatus::Complete);
    EXPECT_EQ(r.scannedEndMs, epoch + 86400000 + minute);
}
TEST_F(PageTest, PositiveEntryBudgetStopsBeforeNextReconstruction) {
    write({record(), record(minute), record(2 * minute)});
    Hmc2Reader reader(root());
    ReadControl warm;
    reader.availability("BTC-USD", "near", minute, warm);
    ReadControl control;
    control.limits.maxEntriesVisited = 3;
    size_t seen = 0;
    const auto scan = reader.visit("BTC-USD", "near", minute, epoch, epoch + 3 * minute,
                                   [&](const auto &) { ++seen; }, control);
    EXPECT_EQ(seen, 1);
    EXPECT_LE(control.entriesVisited, 3);
    EXPECT_EQ(scan.status, ReadStatus::Budget);
    EXPECT_EQ(scan.scannedEndMs, epoch + minute);
}
TEST_F(PageTest, IoErrorDoesNotClaimMissingHistory) {
    std::filesystem::create_directory(root() / "BTC-USD");
    {
        std::ofstream obstruction(root() / "BTC-USD" / "near-60000");
        obstruction << 'x';
    }
    const auto page = buildPage(root(), request());
    EXPECT_EQ(page.status, BuildStatus::IoError);
    EXPECT_FALSE(page.exhausted);
    EXPECT_EQ(page.scannedStartMs, page.scannedEndMs);
}
// Explicit benchmark only: numbers are reported, never asserted against a time limit.
TEST_F(PageTest, DISABLED_SyntheticDayTiming) {
    Hmc2Store store(root());
    for (const auto &layer : {std::string("deep"), std::string("near")}) {
        auto r = record(0, layer);
        const int entries = layer == "deep" ? 12000 : 1200;
        r.bidRowHi = r.askRowHi = entries - 1;
        r.entries.clear();
        for (int i = 0; i < entries; ++i)
            r.entries.push_back({i, bool(i % 2), encodeSize(1 + i % 100), encodeSize(100)});
        for (int n = 0; n < 1440; ++n) {
            r.bucketStartMs = epoch + n * minute;
            r.entries[n % entries].twapCode = encodeSize(2 + n % 80);
            store.append(r);
        }
        if (layer == "deep") {
            r.header.tfMs = hour;
            r.observedMs = hour;
            for (auto &e : r.entries) e.coveredMs = hour;
            r.coverage = {{0, entries - 1, false, hour}, {0, entries - 1, true, hour}};
            for (int n = 0; n < 24; ++n) {
                r.bucketStartMs = epoch + n * hour;
                store.append(r);
            }
        }
    }
    for (const auto &label : {std::string("deep-1m"), std::string("near-1m"), std::string("deep-4h")}) {
        Hmc2Reader reader(root());
        auto q = label == "near-1m" ? request(0, 1200, 2048) : request(0, 120000, 2048);
        if (label == "deep-4h") q.tfMs = 4 * hour;
        for (int pass = 0; pass < 2; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            auto p = buildPage(reader, q);
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "SERVE_TIMING " << label << " pass=" << pass << " columns=" << p.columns.size()
                      << " ms=" << ms << " records=" << p.sourceRecords << " entries=" << p.entriesVisited << '\n';
            EXPECT_EQ(p.status, BuildStatus::Complete);
            EXPECT_EQ(p.columns.size(), label == "deep-4h" ? 6 : 1024);
        }
    }
}
TEST_F(PageTest, DISABLED_FullFourHourPageTiming) {
    Hmc2Store store(root());
    auto r = record(0, "deep", hour);
    r.bidRowHi = r.askRowHi = 11999;
    r.coverage = {{0, 11999, false, hour}, {0, 11999, true, hour}};
    r.entries.clear();
    for (int i = 0; i < 12000; ++i)
        r.entries.push_back({i, bool(i % 2), encodeSize(1 + i % 100), encodeSize(100), hour});
    for (int n = 0; n < 4096; ++n) {
        r.bucketStartMs = epoch + n * hour;
        store.append(r);
    }
    Hmc2Reader reader(root());
    auto q = request(0, 120000, 2048, 4 * hour);
    const auto start = std::chrono::steady_clock::now();
    auto p = buildPage(reader, q);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "SERVE_TIMING deep-4h-full columns=" << p.columns.size() << " ms=" << ms
              << " records=" << p.sourceRecords << " entries=" << p.entriesVisited << '\n';
    EXPECT_EQ(p.status, BuildStatus::Complete);
    EXPECT_EQ(p.columns.size(), 1024);
    q.budgets = BuildRequest{}.budgets;
    const auto limitedStart = std::chrono::steady_clock::now();
    p = buildPage(reader, q);
    std::cout << "SERVE_TIMING deep-4h-default-budget columns=" << p.columns.size() << " ms="
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - limitedStart).count()
              << " status=" << static_cast<int>(p.status) << " exhausted=" << p.exhausted << '\n';
    EXPECT_TRUE(p.status == BuildStatus::Budget || p.status == BuildStatus::Complete);
    EXPECT_FALSE(p.columns.empty());
}
} // namespace
