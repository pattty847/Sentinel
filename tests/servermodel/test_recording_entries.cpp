#include "servermodel/RecordingEntries.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <fstream>
#include <stdexcept>
#include <string>

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
    const auto composed = loadComposedMinuteEntries(root, "BTC-USD", "deep",
        kHmc2MinMs, kHmc2MinMs + 2 * minute, 2);
    EXPECT_EQ(composed.sourceMinutes, 2u);
    EXPECT_TRUE(composed.preNormalized);
    EXPECT_EQ(composed.nativeFactor[0], 2u);
    const auto rawCell = binRecordingCell(entries, 0, 2, 0, 1);
    const auto composedCell = binRecordingCell(composed, 0, 1, 0, 1);
    EXPECT_EQ(rawCell.valid, composedCell.valid);
    EXPECT_LE(std::abs(int(encodeSize(rawCell.bid)) -
                       int(encodeSize(composedCell.bid))), 1);
}

TEST(RecordingEntries, UncomposableHourTailReportsUnavailableInsteadOfReturningMinutes) {
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
        r.bidRowLo = r.askRowLo = r.bidRowHi = r.askRowHi = 4;
        r.entries = {{4, false, encodeSize(2), 0}};
        writer.append(r);
    }
    try {
        (void)loadHourEntriesWithMinuteTail(root, "BTC-USD", "deep",
                                            kHmc2MinMs, kHmc2MinMs + 60 * minute);
        FAIL() << "mixed native grids must not fall back to raw minute columns";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("timeframe unavailable"), std::string::npos);
    }
    EXPECT_THROW((void)loadComposedHourEntries(root, "BTC-USD", "deep",
                  kHmc2MinMs, kHmc2MinMs + 60 * minute, 60), std::runtime_error);
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
    const auto cell = binRecordingCell(data, 0, 2, 0, 0);
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

TEST(RecordingEntries, FiveAndSixteenMinuteColumnsEqualWeightedMinuteSum) {
    const auto data = syntheticRecordingEntries(1'000'000);
    for (uint32_t tf : {5u, 16u}) {
        const uint32_t alignment = uint32_t((tf - (data.startMs / minute) % tf) % tf);
        for (uint32_t first = alignment; first + tf <= data.columns(); first += tf) {
            for (uint32_t row : {0u, 100u, 500u, 999u}) {
                const auto composed = binRecordingCell(data, first, first + tf, row, row);
                double expectedBid = 0, expectedAsk = 0, observed = 0;
                for (uint32_t c = first; c < first + tf; ++c) {
                    observed += data.observedMs[c];
                    for (uint32_t i = data.offsets[c]; i < data.offsets[c + 1]; ++i) {
                        if ((data.rowSide[i] & 0x7fffffffu) != row) continue;
                        const double amount = decodeSize(uint16_t(data.code[i]), data.columnScale[c]) *
                                              data.observedMs[c];
                        (data.rowSide[i] & 0x80000000u ? expectedAsk : expectedBid) += amount;
                    }
                }
                EXPECT_TRUE(composed.valid);
                EXPECT_NEAR(composed.bid, expectedBid / observed, std::max(0.002, expectedBid / observed * 1e-6));
                EXPECT_NEAR(composed.ask, expectedAsk / observed, std::max(0.002, expectedAsk / observed * 1e-6));
            }
        }
    }
}

TEST(RecordingEntries, ExplicitFiveAndSixteenMinuteColumnsKeepPartialCoverage) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 500, {}, 42};
    r.observedMs = minute;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'050;
    r.bidRowLo = r.askRowLo = 20'000;
    r.askRowHi = 20'003;
    {
        Hmc2Store writer(root);
        for (int c = 0; c < 80; ++c) {
            if (c % 11 == 4) continue;
            r.bucketStartMs = kHmc2MinMs + int64_t(c) * minute;
            r.bidRowHi = c % 3 == 0 ? 20'001 : 20'003;
            r.entries = {{20'001, false, encodeSize(3 + c % 4), 0},
                         {20'002, true, encodeSize(8 + c % 5), 0}};
            writer.append(r);
        }
    }
    const auto raw = loadRecordingEntries(root, "BTC-USD", "deep",
                                          kHmc2MinMs, kHmc2MinMs + 80 * minute);
    for (uint32_t tf : {5u, 16u}) {
        const auto composed = loadComposedMinuteEntries(root, "BTC-USD", "deep",
            kHmc2MinMs, kHmc2MinMs + 80 * minute, tf);
        ASSERT_EQ(composed.sourceMinutes, tf);
        ASSERT_EQ(composed.columns(), 80u / tf);
        for (uint32_t c = 0; c < composed.columns(); ++c) {
            for (uint32_t row = 0; row < 4; ++row) {
                const auto original = binRecordingCell(raw, c * tf, (c + 1) * tf, row, row);
                const auto compact = binRecordingCell(composed, c, c + 1, row, row);
                EXPECT_EQ(original.valid, compact.valid);
                EXPECT_LE(std::abs(int(encodeSize(original.bid)) - int(encodeSize(compact.bid))), 1);
                EXPECT_LE(std::abs(int(encodeSize(original.ask)) - int(encodeSize(compact.ask))), 1);
            }
        }
    }
}

TEST(RecordingEntries, MissingMinuteDoesNotDiluteTimeframeColumn) {
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
        for (int c = 0; c < 16; ++c) {
            if (c == 3) continue;
            r.bucketStartMs = kHmc2MinMs + c * minute;
            writer.append(r);
        }
    }
    const auto entries = loadRecordingEntries(root, "BTC-USD", "near", kHmc2MinMs, kHmc2MinMs + 16 * minute);
    const auto cell = binRecordingCell(entries, 0, 16, 0, 1);
    EXPECT_TRUE(cell.valid);
    EXPECT_NEAR(cell.bid, decodeSize(encodeSize(4)), 0.001);
    EXPECT_NEAR(cell.ask, decodeSize(encodeSize(5)), 0.001);
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
