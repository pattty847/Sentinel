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
    ASSERT_EQ(entries.row.size(), 3u);
    EXPECT_EQ(entries.baseRow, 10);
    EXPECT_EQ(entries.offsets, (std::vector<uint32_t>{0, 2, 2, 3, 3}));
    EXPECT_EQ(entries.column, (std::vector<uint32_t>{0, 0, 2}));
    EXPECT_EQ(entries.row, (std::vector<uint32_t>{0, 1, 2}));
    EXPECT_EQ(entries.side, (std::vector<uint32_t>{0, 1, 0}));
    EXPECT_FLOAT_EQ(entries.size[0], float(decodeSize(encodeSize(3))));
    EXPECT_EQ(entries.coverage[1].bidLo > entries.coverage[1].bidHi, true);
    EXPECT_EQ(entries.coverage[0].askHi, 2);
    EXPECT_EQ(entries.observedMs[0], minute);
    EXPECT_EQ(entries.observedMs[1], 0u);

    BuildRequest q;
    q.symbol = "BTC-USD"; q.endMs = kHmc2MinMs; q.count = 1;
    q.priceLo = 10; q.priceHi = 13; q.rows = 3; q.displayTick = 1;
    const auto page = buildPage(root, q);
    ASSERT_EQ(page.columns.size(), 1u);
    EXPECT_NEAR(entries.size[0], page.columns[0].quantities[2] * page.columns[0].quantityScale, 0.01);
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
    EXPECT_EQ(entries.size.size(), 1u);
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
    ASSERT_EQ(entries.size.size(), 1u);
    EXPECT_EQ(entries.column[0], 0u);
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
    ASSERT_EQ(entries.nativeTick, 10);
    ASSERT_EQ(entries.row.size(), 2u);
    EXPECT_EQ(entries.row[0], 0u);
    EXPECT_EQ(entries.row[1], 0u);
    EXPECT_NEAR(entries.size[1], decodeSize(encodeSize(2)) + decodeSize(encodeSize(3)), 0.001);
    EXPECT_EQ(entries.coverage[1].bidLo, 0);
    EXPECT_EQ(entries.coverage[1].bidHi, 0);
}
} // namespace
