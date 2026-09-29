#include "servermodel/RecordingEntries.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <fstream>

using namespace recording;
namespace {
constexpr int64_t minute = 60'000;
TEST(RecordingEntries, SparseOrderCoverageAndQuantitiesMatchMinutePage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "near", minute, 100, 100, {}, 42};
    r.bucketStartMs = kHmc2MinMs;
    r.observedMs = minute;
    r.bidRowLo = 10; r.bidRowHi = 12; r.askRowLo = 10; r.askRowHi = 12;
    r.midOpen = r.midClose = r.midMin = r.midMax = 11;
    r.entries = {{10, false, encodeSize(3), 0}, {11, true, encodeSize(7), 0}};
    {
        Hmc2Store writer(root);
        writer.append(r);
        r.bucketStartMs += 2 * minute;
        r.entries = {{12, false, encodeSize(5), 0}};
        writer.append(r);
    }
    const auto entries = loadRecordingEntries(root, "BTC-USD", "near", kHmc2MinMs, kHmc2MinMs + 4 * minute);
    ASSERT_EQ(entries.columns(), 4u);
    ASSERT_EQ(entries.rowSide.size(), 3u);
    EXPECT_EQ(entries.baseRow, 10);
    EXPECT_EQ(entries.offsets, (std::vector<uint32_t>{0, 2, 2, 3, 3}));
    EXPECT_EQ(entries.rowSide, (std::vector<uint32_t>{0, 0x80000001u, 2}));
    EXPECT_EQ(entries.code[0], encodeSize(3));
    EXPECT_EQ(entries.coverage[1].bidLo > entries.coverage[1].bidHi, true);
    EXPECT_EQ(entries.coverage[0].askHi, 2);
    EXPECT_EQ(entries.observedMs[0], minute);
    EXPECT_EQ(entries.observedMs[1], 0u);

    BuildRequest q;
    q.symbol = "BTC-USD"; q.endMs = kHmc2MinMs; q.count = 1;
    q.priceLo = 10; q.priceHi = 13; q.rows = 3; q.displayTick = 1;
    const auto page = buildPage(root, q);
    ASSERT_EQ(page.columns.size(), 1u);
    EXPECT_NEAR(decodeSize(uint16_t(entries.code[0])), page.columns[0].quantities[2] * page.columns[0].quantityScale, 0.01);
}

TEST(RecordingEntries, ReaderIgnoresIncompleteAppendTail) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 500, {}, 42};
    r.bucketStartMs = kHmc2MinMs;
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = 1; r.bidRowHi = r.askRowHi = 2;
    r.midOpen = r.midClose = r.midMin = r.midMax = 10;
    r.entries = {{1, false, encodeSize(2), 0}};
    {
        Hmc2Store writer(root);
        writer.append(r);
    }
    std::ofstream tail(Hmc2Store::filePath(root, r.header, r.bucketStartMs), std::ios::binary | std::ios::app);
    tail.write("HCR2\x14", 5);
    tail.close();
    const auto entries = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + 2 * minute);
    EXPECT_EQ(entries.code.size(), 1u);
}

TEST(RecordingEntries, ReaderSkipsCrcFailingTailRecord) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "near", minute, 100, 100, {}, 42};
    r.bucketStartMs = kHmc2MinMs;
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = r.bidRowHi = r.askRowHi = 10;
    r.midOpen = r.midClose = r.midMin = r.midMax = 10;
    r.entries = {{10, false, encodeSize(2), 0}};
    {
        Hmc2Store writer(root);
        writer.append(r);
        r.bucketStartMs += minute;
        writer.append(r);
    }
    const auto path = Hmc2Store::filePath(root, r.header, r.bucketStartMs);
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes(std::istreambuf_iterator<char>{input}, {});
    size_t last = std::string::npos;
    for (size_t i = 0; i + 16 <= bytes.size(); ++i)
        if (bytes[i] == 'H' && bytes[i + 1] == 'C' && bytes[i + 2] == 'R' && bytes[i + 3] == '2') last = i;
    ASSERT_NE(last, std::string::npos);
    bytes[last + 12] ^= 0x5a;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    const auto entries = loadRecordingEntries(root, "BTC-USD", "near", kHmc2MinMs, kHmc2MinMs + 3 * minute);
    ASSERT_EQ(entries.code.size(), 1u);
    EXPECT_EQ(entries.offsets[1], 1u);
    EXPECT_EQ(entries.observedMs[1], 0u);
}

TEST(RecordingEntries, MixedFiveAndTenDollarGridsUseCommonRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 1000, {}, 42};
    r.bucketStartMs = kHmc2MinMs;
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = r.bidRowHi = r.askRowHi = 2;
    r.midOpen = r.midClose = r.midMin = r.midMax = 25;
    r.entries = {{2, false, encodeSize(1), 0}};
    {
        Hmc2Store writer(root);
        writer.append(r);
        r.header.rowTickUnits = 500;
        r.header.configHash = 43;
        r.bucketStartMs += minute;
        r.bidRowLo = r.askRowLo = 4;
        r.bidRowHi = r.askRowHi = 5;
        r.entries = {{4, false, encodeSize(2), 0}, {5, false, encodeSize(3), 0}};
        writer.append(r);
    }
    const auto entries = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + 2 * minute);
    ASSERT_EQ(entries.nativeTick, 5);
    ASSERT_EQ(entries.rowSide.size(), 3u);
    EXPECT_EQ(entries.rowSide[0], 0u);
    EXPECT_EQ(entries.rowSide[1], 0u);
    EXPECT_EQ(entries.rowSide[2], 1u);
    EXPECT_EQ(entries.nativeFactor, (std::vector<uint32_t>{2, 1}));
    EXPECT_EQ(entries.coverage[1].bidLo, 0);
    EXPECT_EQ(entries.coverage[1].bidHi, 1);
    const auto older = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + minute);
    const auto recent = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs + minute,
                                             kHmc2MinMs + 2 * minute);
    const auto joined = joinRecordingEntries(older, recent);
    EXPECT_EQ(joined.nativeTick, entries.nativeTick);
    EXPECT_EQ(joined.baseRow, entries.baseRow);
    EXPECT_EQ(joined.rowSide, entries.rowSide);
    EXPECT_EQ(joined.code, entries.code);
    EXPECT_EQ(joined.offsets, entries.offsets);
    EXPECT_EQ(joined.nativeFactor, entries.nativeFactor);
    EXPECT_EQ(joined.coverage[1].bidLo, entries.coverage[1].bidLo);
    EXPECT_EQ(joined.coverage[1].bidHi, entries.coverage[1].bidHi);
}

TEST(RecordingEntries, KeepsOriginalCodesAndPerMinuteScales) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 500, {}, 42};
    r.bucketStartMs = kHmc2MinMs;
    r.observedMs = minute;
    r.bidRowLo = r.bidRowHi = r.askRowLo = r.askRowHi = 20'000;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'000;
    r.entries = {{20'000, false, encodeSize(0.00041, r.header.sizeScale), 0}};
    const uint16_t firstCode = r.entries[0].twapCode;
    {
        Hmc2Store writer(root);
        writer.append(r);
        r.bucketStartMs += minute;
        r.header.sizeScale.floor = 1e-8;
        r.header.configHash = 43;
        r.entries[0].twapCode = encodeSize(0.00041, r.header.sizeScale);
        writer.append(r);
    }
    const auto data = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + 2 * minute);
    ASSERT_EQ(data.baseRow, 20'000);
    ASSERT_EQ(data.code.size(), 2u);
    EXPECT_EQ(data.code[0], firstCode);
    EXPECT_EQ(data.code[1], r.entries[0].twapCode);
    EXPECT_DOUBLE_EQ(data.columnScale[0].floor, 1e-6);
    EXPECT_DOUBLE_EQ(data.columnScale[1].floor, 1e-8);
    const auto cell = binRecordingCell(data, 0, 2, 0, 0, true);
    EXPECT_TRUE(cell.valid);
    const double expected = (decodeSize(firstCode, data.columnScale[0]) +
                             decodeSize(r.entries[0].twapCode, data.columnScale[1])) * 0.5;
    EXPECT_NEAR(cell.bid, expected, expected * 1e-6);
    const auto older = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + minute);
    const auto recent = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs + minute,
                                             kHmc2MinMs + 2 * minute);
    const auto joined = joinRecordingEntries(older, recent);
    EXPECT_EQ(joined.code, data.code);
    EXPECT_EQ(joined.rowSide, data.rowSide);
    EXPECT_DOUBLE_EQ(joined.columnScale[0].floor, 1e-6);
    EXPECT_DOUBLE_EQ(joined.columnScale[1].floor, 1e-8);
}

TEST(RecordingEntries, JoinPreservesEmptyOlderColumns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 500, {}, 42};
    r.bucketStartMs = kHmc2MinMs + minute;
    r.observedMs = minute;
    r.bidRowLo = r.bidRowHi = r.askRowLo = r.askRowHi = 20'000;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'000;
    r.entries = {{20'000, false, encodeSize(3), 0}};
    { Hmc2Store writer(root); writer.append(r); }
    const auto older = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs, kHmc2MinMs + minute);
    const auto recent = loadRecordingEntries(root, "BTC-USD", "deep", kHmc2MinMs + minute,
                                             kHmc2MinMs + 2 * minute);
    ASSERT_EQ(older.nativeTick, 0);
    const auto joined = joinRecordingEntries(older, recent);
    EXPECT_EQ(joined.columns(), 2u);
    EXPECT_EQ(joined.offsets, (std::vector<uint32_t>{0, 0, 1}));
    EXPECT_EQ(joined.observedMs, (std::vector<uint32_t>{0, uint32_t(minute)}));
    EXPECT_EQ(joined.baseRow, 20'000);
    EXPECT_EQ(joined.code[0], encodeSize(3));
}

TEST(RecordingEntries, EightMinuteLodMatchesRawWithUnalignedTimeAndPriceBins) {
    const auto entries = syntheticRecordingEntries(1'000'000);
    ASSERT_EQ(entries.lod.coverage.size(), 125u);
    for (const auto [first, end] : {std::pair{0u, 1000u}, {3u, 28u}, {128u, 144u}, {797u, 999u}}) {
        for (const auto [lo, hi] : {std::pair{0u, 999u}, {330u, 340u}, {660u, 680u}, {500u, 500u}}) {
            const auto raw = binRecordingCell(entries, first, end, lo, hi, false);
            const auto lod = binRecordingCell(entries, first, end, lo, hi, true);
            EXPECT_EQ(raw.valid, lod.valid);
            EXPECT_NEAR(raw.bid, lod.bid, std::max(0.02f, raw.bid * 1e-6f));
            EXPECT_NEAR(raw.ask, lod.ask, std::max(0.02f, raw.ask * 1e-6f));
            const auto spatial = binRecordingCell(entries, first, end, lo, hi, true, true);
            EXPECT_EQ(raw.valid, spatial.valid);
            EXPECT_NEAR(raw.bid, spatial.bid, std::max(0.02f, raw.bid * 1e-6f));
            EXPECT_NEAR(raw.ask, spatial.ask, std::max(0.02f, raw.ask * 1e-6f));
        }
    }
}

TEST(RecordingEntries, TimeLodExcludesMissingMinuteFromObservedDuration) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "near", minute, 100, 100, {}, 42};
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = 10;
    r.bidRowHi = r.askRowHi = 11;
    r.midOpen = r.midClose = r.midMin = r.midMax = 10;
    r.entries = {{10, false, encodeSize(4), 0}, {11, true, encodeSize(5), 0}};
    {
        Hmc2Store writer(root);
        for (int c = 0; c < 8; ++c) {
            if (c == 3) continue;
            r.bucketStartMs = kHmc2MinMs + c * minute;
            writer.append(r);
        }
    }
    const auto entries = loadRecordingEntries(root, "BTC-USD", "near", kHmc2MinMs, kHmc2MinMs + 8 * minute);
    const auto raw = binRecordingCell(entries, 0, 8, 0, 1, false);
    const auto lod = binRecordingCell(entries, 0, 8, 0, 1, true);
    EXPECT_TRUE(raw.valid);
    EXPECT_TRUE(lod.valid);
    EXPECT_NEAR(raw.bid, lod.bid, 0.0001);
    EXPECT_NEAR(raw.ask, lod.ask, 0.0001);
}

TEST(RecordingEntries, AlignedDensePriceSumsMatchRawMinutes) {
    const auto entries = syntheticRecordingEntries(1'000'000);
    for (uint32_t c : {0u, 123u, 999u}) {
        for (uint32_t row : {0u, 320u, 640u, 960u}) {
            const auto raw = binRecordingCell(entries, c, c + 1, row, row + 39, false);
            const auto meta = entries.dense40.meta[c];
            const auto index = meta[0] + int32_t(row / 40) - meta[1];
            ASSERT_GE(index, meta[0]);
            ASSERT_LT(index, entries.dense40.meta[c + 1][0]);
            EXPECT_NEAR(raw.bid, entries.dense40.sums[index][0], std::max(0.02f, raw.bid * 1e-6f));
            EXPECT_NEAR(raw.ask, entries.dense40.sums[index][1], std::max(0.02f, raw.ask * 1e-6f));
        }
    }
    const auto rawEight = binRecordingCell(entries, 0, 8, 300, 399, false);
    const auto coarseMeta = entries.timeDense100.meta[0];
    const auto coarseIndex = coarseMeta[0] + 3 - coarseMeta[1];
    ASSERT_GE(coarseIndex, coarseMeta[0]);
    EXPECT_NEAR(rawEight.bid, entries.timeDense100.sums[coarseIndex][0] / (8 * minute),
                std::max(0.02f, rawEight.bid * 1e-6f));
}

TEST(RecordingEntries, ParallelReaderChunksKeepMinuteOffsets) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "near", minute, 100, 100, {}, 42};
    r.observedMs = minute;
    r.bidRowLo = r.askRowLo = r.bidRowHi = r.askRowHi = 10;
    r.midOpen = r.midClose = r.midMin = r.midMax = 10;
    r.entries = {{10, false, encodeSize(2), 0}};
    {
        Hmc2Store writer(root);
        for (int c = 0; c < 720; ++c) {
            if (c == 359 || c == 541) continue;
            r.bucketStartMs = kHmc2MinMs + int64_t(c) * minute;
            writer.append(r);
        }
    }
    const auto entries = loadRecordingEntries(root, "BTC-USD", "near", kHmc2MinMs, kHmc2MinMs + 720 * minute);
    ASSERT_EQ(entries.columns(), 720u);
    ASSERT_EQ(entries.rowSide.size(), 718u);
    EXPECT_EQ(entries.offsets[359], entries.offsets[360]);
    EXPECT_EQ(entries.offsets[360] + 1, entries.offsets[361]);
    EXPECT_EQ(entries.offsets[541], entries.offsets[542]);
    EXPECT_EQ(entries.observedMs[359], 0u);
    EXPECT_EQ(entries.observedMs[360], minute);
    EXPECT_EQ(entries.offsets.back(), 718u);
}
} // namespace
