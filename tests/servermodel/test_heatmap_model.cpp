#include "heatmap/BinCell.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "heatmap/RecordingLoader.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QDateTime>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <map>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace recording;
constexpr int64_t minute = heatmap::kMinuteMs, hour = heatmap::kHourMs, day = heatmap::kDayMs;
constexpr int64_t epoch = kHmc2MinMs;

Hmc2Record makeMinute(int64_t index, const std::string& layer = "deep") {
    Hmc2Record r;
    const int64_t tick = layer == "near" ? 1 : index < 360 ? 10 : 5;
    r.header = {"BTC-USD", layer, minute, 100, tick * 100, {}, uint64_t(index < 360 ? 41 : 42)};
    if (index >= 540) { r.header.sizeScale.floor = 1e-8; r.header.configHash = 43; }
    r.bucketStartMs = epoch + index * minute;
    r.observedMs = index % 11 ? minute : 17'123;
    r.flags = (r.observedMs < minute ? kPartial : 0) | (index % 31 == 0 ? kResynced : 0);
    r.bidRowLo = 100'000 / tick;
    r.askRowLo = (100'000 + (index % 7 == 0 ? 10 : 0)) / tick;
    r.bidRowHi = (100'040 - (index % 5 == 0 ? 10 : 0)) / tick - 1;
    r.askRowHi = 100'040 / tick - 1;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'020;
    for (int64_t row = 100'000 / tick; row < 100'030 / tick; ++row) {
        for (bool ask : {false, true}) {
            if (row < (ask ? r.askRowLo : r.bidRowLo) || row > (ask ? r.askRowHi : r.bidRowHi)) continue;
            const double size = 0.000013 * (1 + (row * 17 + index * 13 + int(ask) * 19) % 127);
            const auto code = encodeSize(size, r.header.sizeScale);
            r.entries.push_back({row, ask, code, code, r.observedMs});
        }
    }
    return r;
}

// Independent dense fixture rollup, deliberately quantized as persisted HMC2
// hours are. No production composer/coverage sweep is used to make the oracle.
Hmc2Record makeHour(const std::vector<Hmc2Record>& minutes) {
    auto out = minutes.front();
    out.header.tfMs = hour;
    out.bucketStartMs = out.bucketStartMs / hour * hour;
    out.entries.clear(); out.coverage.clear(); out.observedMs = 0; out.flags = 0;
    out.bidRowLo = out.askRowLo = INT64_MAX;
    out.bidRowHi = out.askRowHi = 0;
    for (const auto& r : minutes) {
        out.observedMs += r.observedMs;
        out.flags |= r.flags;
        out.bidRowLo = std::min(out.bidRowLo, r.bidRowLo);
        out.bidRowHi = std::max(out.bidRowHi, r.bidRowHi);
        out.askRowLo = std::min(out.askRowLo, r.askRowLo);
        out.askRowHi = std::max(out.askRowHi, r.askRowHi);
    }
    for (bool ask : {false, true}) {
        for (int64_t row = ask ? out.askRowLo : out.bidRowLo; row <= (ask ? out.askRowHi : out.bidRowHi); ++row) {
            uint32_t covered = 0;
            long double numerator = 0;
            for (const auto& r : minutes) {
                if (row < (ask ? r.askRowLo : r.bidRowLo) || row > (ask ? r.askRowHi : r.bidRowHi)) continue;
                covered += r.observedMs;
                for (const auto& e : r.entries)
                    if (e.row == row && e.isAsk == ask)
                        numerator += static_cast<long double>(decodeSize(e.twapCode, r.header.sizeScale)) * r.observedMs;
            }
            if (!covered) continue;
            if (!out.coverage.empty() && out.coverage.back().isAsk == ask &&
                out.coverage.back().hi + 1 == row && out.coverage.back().coveredMs == covered)
                out.coverage.back().hi = row;
            else out.coverage.push_back({row, row, ask, covered});
            if (numerator) {
                const auto code = encodeSize(static_cast<double>(numerator / covered), out.header.sizeScale);
                out.entries.push_back({row, ask, code, code, covered});
            }
        }
    }
    std::sort(out.entries.begin(), out.entries.end(), [](const auto& a, const auto& b) {
        return std::pair(a.row, a.isAsk) < std::pair(b.row, b.isAsk);
    });
    return out;
}

// Independent dense native-row CPU integration of raw minutes. No production
// accumulator, coverage lookup, binning or composition helpers are used here.
heatmap::BinCell sumMinutes(const heatmap::SparseColumns& minutes, int64_t start, int64_t tf,
                            double price, double displayTick) {
    struct Grid {
        uint64_t observed = 0;
        std::map<int64_t, std::array<uint64_t, 2>> coverage;
        std::map<int64_t, std::array<long double, 2>> numerator;
    };
    std::map<double, Grid> grids;
    uint64_t total = 0;
    for (const auto& column : minutes.columns) {
        if (column.bucketStartMs < start || column.bucketStartMs >= start + tf) continue;
        total += column.observedMs;
        for (const auto& n : column.native) {
            const auto tick = n.grid.rowTickUnits / n.grid.priceScale;
            auto& grid = grids[tick];
            grid.observed += n.observedMs;
            const auto lo = int64_t(std::llround(price / tick));
            const auto end = int64_t(std::llround((price + displayTick) / tick));
            for (int64_t row = lo; row < end; ++row) for (bool ask : {false, true}) {
                for (const auto& run : n.coverage[ask])
                    if (row >= run.lo && row <= run.hi) grid.coverage[row][ask] += n.observedMs;
            }
            for (const auto& e : n.entries) {
                const auto row = n.baseRow + e.row();
                if (row >= lo && row < end) grid.numerator[row][e.isAsk()] +=
                    static_cast<long double>(decodeSize(e.code, n.sizeScale)) * n.observedMs;
            }
        }
    }
    heatmap::BinCell out;
    if (!total) return out;
    out.valid = true;
    std::array<long double, 2> sums{};
    for (auto& [tick, grid] : grids) {
        std::array<long double, 2> native{};
        const auto lo = int64_t(std::llround(price / tick)), end = int64_t(std::llround((price + displayTick) / tick));
        for (auto row = lo; row < end; ++row) for (bool ask : {false, true}) {
            const auto duration = grid.coverage[row][ask];
            out.valid &= duration == grid.observed;
            if (duration) native[ask] += grid.numerator[row][ask] / duration;
        }
        for (bool ask : {false, true}) sums[ask] += native[ask] * (static_cast<long double>(grid.observed) / total);
    }
    out.bid = double(sums[0]); out.ask = double(sums[1]);
    out.dominantAsk = sums[1] > sums[0];
    out.code = withSide(encodeSize(out.dominantAsk ? out.ask : out.bid), out.dominantAsk);
    return out;
}

void comparePage(Hmc2Reader& reader, const BuildRequest& q) {
    const auto page = buildPage(reader, q);
    ASSERT_EQ(page.status, BuildStatus::Complete) << page.message;
    ASSERT_FALSE(page.columns.empty());
    const auto start = page.columns.front().bucketStartMs;
    const auto end = q.endMs / q.tfMs * q.tfMs + q.tfMs;
    auto levels = heatmap::loadRecordingLevels(reader, q.symbol, page.layer, start, end, q.tfMs);
    const auto composed = heatmap::compose(levels, q.tfMs);
    ASSERT_EQ(composed.tfMs, q.tfMs);
    ASSERT_EQ(composed.columns.size(), page.columns.size());
    for (size_t c = 0; c < composed.columns.size(); ++c) {
        const auto& actual = composed.columns[c];
        const auto& expected = page.columns[c];
        ASSERT_EQ(actual.bucketStartMs, expected.bucketStartMs);
        EXPECT_EQ(actual.bucketStartMs % q.tfMs, 0);
        EXPECT_EQ(actual.observedMs, expected.observedMs);
        EXPECT_EQ(actual.flags, expected.flags);
        const auto cells = heatmap::binColumn(actual, page.band.lo, page.band.lo + page.band.tick * page.band.rows,
                                               page.band.tick, page.sizeScale);
        ASSERT_EQ(cells.size(), expected.cells.size());
        for (size_t row = 0; row < cells.size(); ++row) {
            SCOPED_TRACE(testing::Message() << "tf=" << q.tfMs << " layer=" << page.layer << " bucket="
                         << actual.bucketStartMs << " row=" << row << " tick=" << page.band.tick);
            EXPECT_EQ(cells[row].code, expected.cells[row]); // exactly zero code steps, including side
            EXPECT_EQ(cells[row].dominantAsk, isAsk(expected.cells[row]));
            EXPECT_EQ(cells[row].valid, bool((expected.validity[row / 8] >> (row % 8)) & 1));
            const auto oracle = heatmap::binCell(actual, page.band.lo + (cells.size() - 1 - row) * page.band.tick,
                                                  page.band.tick, page.sizeScale);
            EXPECT_EQ(cells[row].code, oracle.code);
            EXPECT_EQ(cells[row].valid, oracle.valid);
            EXPECT_EQ(cells[row].dominantAsk, oracle.dominantAsk);
            EXPECT_NEAR(cells[row].bid, oracle.bid, 1e-12);
            EXPECT_NEAR(cells[row].ask, oracle.ask, 1e-12);
            EXPECT_NEAR(std::max(cells[row].bid, cells[row].ask), expected.quantities[row] * expected.quantityScale,
                        expected.quantityScale * 0.501 + 1e-12);
        }
    }
}

class HeatmapModel : public testing::Test {
protected:
    QTemporaryDir dir;
    std::filesystem::path root() const { return dir.path().toStdString(); }
    void writeFixture(int64_t count = 26 * 60 + 17) {
        Hmc2Store writer(root());
        std::map<int64_t, std::vector<Hmc2Record>> hours;
        for (int64_t i = 0; i < count; ++i) {
            if (i % 17 == 3 || (i >= 122 && i < 133)) continue;
            writer.append(makeMinute(i, "near"));
            auto r = makeMinute(i);
            writer.append(r);
            hours[i / 60].push_back(std::move(r));
        }
        for (const auto& [h, records] : hours)
            if (h < 26 && h != 4 && h != 10) writer.append(makeHour(records));
    }
};

TEST_F(HeatmapModel, ExactPageParityAcrossTimeframesLayersGapsScalesAndNativeGrids) {
    writeFixture();
    Hmc2Reader reader(root());
    for (const int64_t tf : {minute, 5 * minute, 16 * minute, hour, 4 * hour, day}) {
        for (const bool preferNear : {false, true}) {
            BuildRequest q;
            q.symbol = "BTC-USD"; q.tfMs = tf; q.budgets = {};
            q.displayTick = tf < hour && preferNear ? 2. : 10.;
            q.priceLo = 99'990; q.priceHi = 100'050; q.rows = 60;
            // Two suffixes cover the mid-range grid change and the open-hour tail.
            for (const auto end : {epoch + 10 * hour, epoch + 27 * hour}) {
                q.endMs = end / tf * tf;
                q.count = tf < hour ? 700 : 48;
                comparePage(reader, q);
            }
        }
    }
    // Latest $5 grid is representable, historical $10 constituents make only
    // their output columns unknown when displaying $5 bins.
    BuildRequest q;
    q.symbol = "BTC-USD"; q.tfMs = 16 * minute; q.endMs = epoch + 7 * hour;
    q.count = 30; q.priceLo = 100'000; q.priceHi = 100'040; q.displayTick = 5; q.rows = 8; q.budgets = {};
    comparePage(reader, q);
}

TEST_F(HeatmapModel, SixteenAndNinetyMinutesUseExactEpochBucketsAndMinuteDurations) {
    writeFixture(12 * 60);
    Hmc2Reader reader(root());
    for (const int64_t tf : {16 * minute, 90 * minute}) {
        const auto start = (epoch + 5 * hour) / tf * tf;
        const auto end = start + 4 * tf;
        auto levels = heatmap::loadRecordingLevels(reader, "BTC-USD", "deep", start, end, tf);
        ASSERT_EQ(levels.size(), 1u);
        EXPECT_EQ(levels.front().tfMs, minute);
        const auto result = heatmap::compose(levels, tf);
        ASSERT_EQ(result.columns.size(), 4u);
        for (size_t c = 0; c < 4; ++c) {
            const auto& column = result.columns[c];
            EXPECT_EQ(column.bucketStartMs, start + int64_t(c) * tf);
            uint64_t observed = 0;
            for (const auto& source : levels.front().columns)
                if (source.bucketStartMs >= column.bucketStartMs && source.bucketStartMs < column.bucketStartMs + tf)
                    observed += source.observedMs;
            EXPECT_EQ(column.observedMs, observed);
            for (const double price : {99'990., 100'000., 100'010., 100'020., 100'030., 100'040.}) {
                const auto expected = sumMinutes(levels.front(), column.bucketStartMs, tf, price, 10);
                const auto actual = heatmap::binCell(column, price, 10);
                EXPECT_NEAR(actual.bid, expected.bid, 1e-15);
                EXPECT_NEAR(actual.ask, expected.ask, 1e-15);
                EXPECT_EQ(actual.code, expected.code);
                EXPECT_EQ(actual.valid, expected.valid);
                EXPECT_EQ(actual.dominantAsk, expected.dominantAsk);
            }
        }
    }
}

TEST_F(HeatmapModel, SparseAdapterPreservesIdentityScaleCoverageAndPackedBaseRows) {
    auto r = makeMinute(541);
    r.flags |= kProvisional;
    const auto column = heatmap::fromRecording(r);
    ASSERT_EQ(column.native.size(), 1u);
    const auto& n = column.native.front();
    EXPECT_EQ(column.flags, r.flags);
    EXPECT_EQ(n.grid.configHash, r.header.configHash);
    EXPECT_EQ(n.grid.rowTickUnits, r.header.rowTickUnits);
    EXPECT_EQ(n.sizeScale.floor, r.header.sizeScale.floor);
    EXPECT_GT(n.baseRow, 0);
    ASSERT_EQ(n.entries.size(), r.entries.size());
    for (size_t i = 0; i < n.entries.size(); ++i) {
        EXPECT_EQ(n.baseRow + n.entries[i].row(), r.entries[i].row);
        EXPECT_EQ(n.entries[i].isAsk(), r.entries[i].isAsk);
        EXPECT_EQ(n.entries[i].code, r.entries[i].twapCode);
        EXPECT_TRUE(n.entryCoveredMs.empty());
        EXPECT_EQ(heatmap::entryCoveredMs(n, i), r.observedMs);
    }
    EXPECT_EQ(n.coverage[0][0].lo, r.bidRowLo);
    EXPECT_EQ(n.coverage[1][0].hi, r.askRowHi);
    const auto h = makeHour({makeMinute(0), makeMinute(1)});
    const auto hourColumn = heatmap::fromRecording(h);
    const auto& hn = hourColumn.native[0];
    ASSERT_EQ(hn.entryCoveredMs.size(), hn.entries.size());
    for (size_t i = 0; i < hn.entries.size(); ++i) EXPECT_EQ(hn.entryCoveredMs[i], h.entries[i].coveredMs);
}

TEST_F(HeatmapModel, OpenHourAcrossGridChangeComposesWithoutMinuteFallback) {
    {
        Hmc2Store writer(root());
        writer.append(makeMinute(0));
        auto finer = makeMinute(360);
        finer.bucketStartMs = epoch + minute;
        writer.append(finer);
    }
    Hmc2Reader reader(root());
    auto levels = heatmap::loadRecordingLevels(reader, "BTC-USD", "deep", epoch, epoch + hour, hour);
    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(levels[0].tfMs, minute);
    const auto composed = heatmap::compose(levels, hour);
    ASSERT_EQ(composed.columns.size(), 1u);
    EXPECT_EQ(composed.tfMs, hour);
    EXPECT_EQ(composed.columns[0].native.size(), 2u);
    BuildRequest q;
    q.symbol = "BTC-USD"; q.tfMs = hour; q.endMs = epoch; q.count = 1;
    q.priceLo = 99'990; q.priceHi = 100'050; q.displayTick = 10; q.rows = 6; q.budgets = {};
    comparePage(reader, q);
    levels[0].columns.back().flags |= kProvisional;
    EXPECT_TRUE(heatmap::compose(levels, hour).columns[0].flags & kProvisional);
}

TEST_F(HeatmapModel, CoarseScannedGapsSupersedeMinutesAndCompositionRemainsAssociative) {
    writeFixture(120);
    Hmc2Reader reader(root());
    auto minutes = heatmap::loadRecording(reader, "BTC-USD", "deep", minute, epoch, epoch + 2 * hour);
    auto hours = heatmap::loadRecording(reader, "BTC-USD", "deep", hour, epoch, epoch + hour);
    hours.columns.clear(); // a proven missing sealed hour must not be backfilled
    const std::array levels{minutes, hours};
    const auto composed = heatmap::compose(levels, hour);
    ASSERT_EQ(composed.columns.size(), 1u);
    EXPECT_EQ(composed.columns[0].bucketStartMs, epoch + hour);
    const auto five = heatmap::compose(minutes, 5 * minute);
    const auto viaFive = heatmap::compose(five, 15 * minute);
    const auto direct = heatmap::compose(minutes, 15 * minute);
    ASSERT_EQ(viaFive.columns.size(), direct.columns.size());
    for (size_t c = 0; c < direct.columns.size(); ++c) {
        ASSERT_FALSE(viaFive.columns[c].native[0].numerators.empty());
        for (const double price : {99'990., 100'000., 100'010., 100'020., 100'030., 100'040.}) {
            const auto a = heatmap::binCell(viaFive.columns[c], price, 10);
            const auto b = heatmap::binCell(direct.columns[c], price, 10);
            EXPECT_EQ(a.code, b.code);
            EXPECT_EQ(a.valid, b.valid);
        }
    }
    EXPECT_THROW(heatmap::compose(hours, 90 * minute), std::invalid_argument);
    EXPECT_THROW(heatmap::compose(std::array{minutes, minutes}, 5 * minute), std::invalid_argument);
}

TEST(HeatmapResolution, PriceLadderLayerAndOnePixelTimeLimit) {
    EXPECT_EQ(heatmap::idealTick(100, 200, 100, 2), 2);
    EXPECT_EQ(heatmap::idealTick(100, 225, 100, 2), 5);
    EXPECT_EQ(heatmap::idealTick(100, 225, 100, 2, 0.5), 2.5);
    EXPECT_EQ(heatmap::idealTick(100, 225, 100, 2, 5), 5);
    EXPECT_EQ(heatmap::idealTick(100, 200, 0, 2), 0);
    EXPECT_EQ(heatmap::idealTick(100, 200, 100, INFINITY), 0);
    EXPECT_EQ(heatmap::layerFor(2, 5, 16 * minute), "near");
    EXPECT_EQ(heatmap::layerFor(5, 5, minute), "deep");
    EXPECT_EQ(heatmap::layerFor(1, 5, hour), "deep");
    EXPECT_EQ(heatmap::layerFor(1, 5, 90 * minute), "deep");
    for (const auto tf : heatmap::kAutoTimeframes) {
        EXPECT_EQ(heatmap::autoTimeframe(0, 1000 * tf, 1000), tf);
        EXPECT_NE(heatmap::autoTimeframe(1, 1 + 1000 * tf, 1000), tf);
    }
    EXPECT_FALSE(heatmap::autoTimeframe(0, 1001 * day, 1000));
    EXPECT_FALSE(heatmap::autoTimeframe(0, minute, 0));
    EXPECT_EQ(heatmap::autoTimeframe(1, 1000 * minute, 1000), minute);
    EXPECT_EQ(heatmap::autoTimeframe(-minute, 0, 1), minute);
    EXPECT_EQ(heatmap::autoTimeframe(-1, 1, 1), std::nullopt);
}

TEST(HeatmapModelScan, UnloadedChunksAndPartialEdgesNeverBecomeRecorderGaps) {
    using heatmap::BucketState;
    // Adjacent chunks can jointly prove a 16m bucket; an unloaded middle chunk
    // must remain loading even though it lies inside the bounding extent.
    heatmap::SparseColumns a{"BTC-USD", "deep", minute, epoch + minute, epoch + 20 * minute,
        {heatmap::fromRecording(makeMinute(2)), heatmap::fromRecording(makeMinute(17))},
        {{epoch + minute, epoch + 20 * minute}}};
    heatmap::SparseColumns b{"BTC-USD", "deep", minute, epoch + 20 * minute, epoch + 33 * minute,
        {heatmap::fromRecording(makeMinute(32))}, {{epoch + 20 * minute, epoch + 33 * minute}}};
    heatmap::SparseColumns c{"BTC-USD", "deep", minute, epoch + 48 * minute, epoch + 64 * minute,
        {}, {{epoch + 48 * minute, epoch + 64 * minute}}};
    const auto result = heatmap::compose(std::array{c, a, b}, 16 * minute);
    EXPECT_EQ(result.startMs, epoch);
    EXPECT_EQ(result.endMs, epoch + 64 * minute);
    EXPECT_EQ(result.scannedRanges, (std::vector<heatmap::SparseColumns::TimeRange>{
        {epoch + 16 * minute, epoch + 32 * minute}, {epoch + 48 * minute, epoch + 64 * minute}}));
    EXPECT_EQ(heatmap::bucketState(result, epoch), BucketState::NotLoaded);
    EXPECT_EQ(heatmap::bucketState(result, epoch + 16 * minute), BucketState::Present);
    EXPECT_EQ(heatmap::bucketState(result, epoch + 32 * minute), BucketState::NotLoaded);
    EXPECT_EQ(heatmap::bucketState(result, epoch + 48 * minute), BucketState::Gap);
    EXPECT_EQ(heatmap::bucketState(result, epoch + 64 * minute), BucketState::NotLoaded);
    ASSERT_EQ(result.columns.size(), 1u);
    EXPECT_EQ(heatmap::bucketState(heatmap::compose(result, 32 * minute), epoch), BucketState::NotLoaded);
    auto noProof = a;
    noProof.scannedRanges.clear();
    EXPECT_THROW(heatmap::validate(noProof), std::invalid_argument);
    noProof.columns.clear();
    const auto unloaded = heatmap::compose(noProof, 16 * minute);
    EXPECT_TRUE(unloaded.scannedRanges.empty());
    EXPECT_TRUE(unloaded.columns.empty());
    // A coarse level's bounding extent alone must not supersede loaded minutes.
    heatmap::SparseColumns hours{"BTC-USD", "deep", hour, epoch, epoch + 2 * hour, {},
        {{epoch, epoch + hour}}};
    heatmap::SparseColumns tail{"BTC-USD", "deep", minute, epoch + hour, epoch + 2 * hour,
        {heatmap::fromRecording(makeMinute(61))}, {{epoch + hour, epoch + 2 * hour}}};
    const auto mixed = heatmap::compose(std::array{hours, tail}, hour);
    EXPECT_EQ(heatmap::bucketState(mixed, epoch), BucketState::Gap);
    EXPECT_EQ(heatmap::bucketState(mixed, epoch + hour), BucketState::Present);
}

TEST(HeatmapModelRows, SparseMergeAndWholeColumnBinningMatchCellOracle) {
    auto r = makeMinute(0, "near");
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = 100'000;
    r.bidRowHi = r.askRowHi = 700'000;
    r.entries = {{100'000, false, 10000, 10000, minute}, {700'000, true, 11000, 11000, minute}};
    auto second = r;
    second.bucketStartMs += minute;
    second.entries[0].twapCode = 12000;
    second.askRowLo += 10;
    heatmap::SparseColumns minutes{"BTC-USD", "near", minute, epoch, epoch + 2 * minute,
        {heatmap::fromRecording(r), heatmap::fromRecording(second)}, {{epoch, epoch + 2 * minute}}};
    const auto composed = heatmap::compose(minutes, 2 * minute);
    ASSERT_EQ(composed.columns.size(), 1u);
    const auto& column = composed.columns[0];
    EXPECT_EQ(column.native[0].entries.size(), 2u);
    EXPECT_TRUE(column.native[0].composed);
    const auto cells = heatmap::binColumn(column, 0, 800'000, 100'000);
    ASSERT_EQ(cells.size(), 8u);
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto oracle = heatmap::binCell(column, double(7 - i) * 100'000, 100'000);
        EXPECT_EQ(cells[i].code, oracle.code);
        EXPECT_EQ(cells[i].valid, oracle.valid);
        EXPECT_EQ(cells[i].dominantAsk, oracle.dominantAsk);
        EXPECT_DOUBLE_EQ(cells[i].bid, oracle.bid);
        EXPECT_DOUBLE_EQ(cells[i].ask, oracle.ask);
    }
    static_assert(sizeof(heatmap::SparseEntry) == 8);
    EXPECT_TRUE(minutes.columns[0].native[0].entryCoveredMs.empty());
    auto broken = minutes;
    std::swap(broken.columns[0].native[0].entries[0], broken.columns[0].native[0].entries[1]);
    EXPECT_THROW(heatmap::validate(broken), std::invalid_argument);
}

TEST(HeatmapModelTiming, SyntheticMonthOfDeepHoursReportsComposeMilliseconds) {
    // A single assert-free timing sample, not a latency gate or benchmark loop.
    // 720 input columns fit on one screen at >=1px/column; fixture construction
    // is excluded. Include full validation, decode tables and output allocation.
    constexpr int64_t rows = 4096;
    heatmap::SparseColumns hours{"BTC-USD", "deep", hour, epoch, epoch + 30 * day, {},
        {{epoch, epoch + 30 * day}}};
    size_t entries = 0;
    for (int64_t h = 0; h < 720; ++h) {
        if (h % 19 == 3) continue;
        heatmap::NativeColumn n;
        n.grid = {42, 500, 100}; n.baseRow = 20'000;
        n.observedMs = h % 13 ? hour : hour / 3;
        n.coverage[0] = {{n.baseRow, n.baseRow + rows - 1, n.observedMs}};
        n.coverage[1] = n.coverage[0];
        for (uint32_t row = 0; row < rows; ++row) for (bool ask : {false, true})
            n.entries.push_back({row | (uint32_t(ask) << 31), uint16_t(10000 + (row * 7 + h * 11 + ask) % 2000)});
        entries += n.entries.size();
        hours.columns.push_back({epoch + h * hour, n.observedMs, 0, {std::move(n)}});
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = heatmap::compose(hours, 4 * hour);
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "heatmap month compose: input_hours=720 native_rows=" << rows << " entries=" << entries
              << " output_columns=" << result.columns.size() << " tf_ms=" << result.tfMs
              << " elapsed_ms=" << elapsed << " target_ms=50 (informational)\n";
}

TEST(HeatmapModelPrecision, ComposedNumeratorsAvoidAnExtraLogCodeStep) {
    auto a = makeMinute(0, "near");
    a.observedMs = 60'000;
    a.bidRowLo = a.askRowLo = 100'000;
    a.bidRowHi = a.askRowHi = 100'001;
    a.entries = {{100'000, false, 10000, 10000, 60'000}, {100'001, false, 10001, 10001, 60'000}};
    auto b = a;
    b.bucketStartMs += minute;
    b.observedMs = 17'123;
    b.entries[0].twapCode = 10216;
    const heatmap::SparseColumns minutes{"BTC-USD", "near", minute, epoch, epoch + 2 * minute,
        {heatmap::fromRecording(a), heatmap::fromRecording(b)}, {{epoch, epoch + 2 * minute}}};
    const auto composed = heatmap::compose(minutes, 2 * minute);
    ASSERT_EQ(composed.columns.size(), 1u);
    EXPECT_EQ(heatmap::binCell(composed.columns.front(), 100'000, 2).code, 10846);
    auto quantized = composed.columns.front();
    quantized.native.front().numerators.clear();
    auto malformed = composed;
    malformed.columns.front() = quantized;
    EXPECT_THROW(heatmap::validate(malformed), std::invalid_argument);
    EXPECT_THROW(heatmap::compose(malformed, 4 * minute), std::invalid_argument);
    EXPECT_THROW(heatmap::binCell(quantized, 100'000, 2), std::invalid_argument);
    // Deliberately reinterpret rounded means as raw data to demonstrate the loss.
    quantized.native.front().composed = false;
    EXPECT_EQ(heatmap::binCell(quantized, 100'000, 2).code, 10845);
}

TEST(HeatmapModelCoverage, ZeroRowsAreValidGapsAreUnknownAndTiesSelectBid) {
    auto r = makeMinute(0, "near");
    r.entries.clear();
    r.bidRowLo = r.askRowLo = 100'000;
    r.bidRowHi = r.askRowHi = 100'002;
    auto column = heatmap::fromRecording(r);
    EXPECT_TRUE(heatmap::binCell(column, 100'000, 2).valid);
    EXPECT_FALSE(heatmap::binCell(column, 100'002, 2).valid);
    EXPECT_EQ(heatmap::binCell(column, 100'000, 2).code, 0);
    r.entries = {{100'000, false, 10000, 10000, r.observedMs},
                 {100'000, true, 10000, 10000, r.observedMs}};
    column = heatmap::fromRecording(r);
    const auto tie = heatmap::binCell(column, 100'000, 2);
    EXPECT_TRUE(tie.valid);
    EXPECT_FALSE(tie.dominantAsk);
    EXPECT_EQ(tie.code, 10000);
    EXPECT_FALSE(heatmap::binCell({}, 100'000, 2).valid);
    EXPECT_THROW(heatmap::binColumn(column, 100, 200, 0), std::invalid_argument);
}

TEST(HeatmapModelReal, ClosedPastBucketsMatchPagesExactly) {
    if (qgetenv("SENTINEL_HEATMAP_REAL_PARITY") != "1") GTEST_SKIP() << "Set SENTINEL_HEATMAP_REAL_PARITY=1 to opt in";
    const auto overrideRoot = qgetenv("SENTINEL_HEATMAP_REAL_ROOT");
    const std::filesystem::path root = overrideRoot.isEmpty() ? "/Volumes/T7/sentinel-data/recording" : overrideRoot.toStdString();
    ASSERT_TRUE(std::filesystem::exists(root / "BTC-USD")) << root;
    Hmc2Reader reader(root);
    // Strictly before today's UTC boundary: no open minute/hour/day enters parity.
    const auto end = QDateTime::currentMSecsSinceEpoch() / day * day;
    for (const auto tf : {minute, 5 * minute, 16 * minute, hour, 4 * hour, day}) {
        for (const bool near : {false, true}) {
            BuildRequest q;
            q.symbol = "BTC-USD"; q.tfMs = tf; q.endMs = end - tf; q.count = 2; q.budgets = {};
            q.displayTick = tf < hour && near ? 2. : 20.;
            // Derive an occupied range from a closed record rather than assuming
            // the live market's current price. Read just one hour for minute levels.
            ReadControl control;
            bool found = false;
            reader.visit(q.symbol, near && tf < hour ? "near" : "deep", tf < hour ? minute : hour,
                end - (tf < hour ? hour : 2 * day), end, [&](const auto& r) {
                    if (r.entries.empty()) return;
                    q.priceLo = std::floor((r.entries.front().row * r.header.rowTickUnits / r.header.priceScale) /
                                           *q.displayTick) * *q.displayTick;
                    q.priceHi = q.priceLo + 8 * *q.displayTick;
                    found = true;
                }, control);
            ASSERT_TRUE(found) << "No closed source records for tf=" << tf;
            comparePage(reader, q);
        }
    }
}
} // namespace
