#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>

using namespace recording;
namespace {
class StoreTest : public testing::Test {
  protected:
    QTemporaryDir dir;
    std::filesystem::path root() {
        return dir.path().toStdString();
    }
    Hmc2Record record(int64_t t = 0) {
        Hmc2Record r;
        r.header = {"BTC-USD", "deep", 60000, 100, 1000, {}, 42};
        r.bucketStartMs = t;
        r.observedMs = 60000;
        r.bidRowLo = r.askRowLo = 0;
        r.bidRowHi = r.askRowHi = 100;
        r.midOpen = r.midClose = r.midMin = r.midMax = 100;
        r.entries = {{9, false, encodeSize(2), encodeSize(4)}, {9, true, encodeSize(3), encodeSize(6)}};
        return r;
    }
    std::vector<Hmc2Record> read() {
        return Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, 0, 172800000);
    }
    std::vector<uint8_t> bytes(const std::filesystem::path &path) {
        std::ifstream f(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
    void save(const std::filesystem::path &path, const std::vector<uint8_t> &b) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(b.data()), b.size());
    }
    uint32_t u32(const std::vector<uint8_t> &b, size_t p) {
        return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24;
    }
};
TEST_F(StoreTest, RoundTripAndBucketUtcPath) {
    auto r = record(86400000 - 60000);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    const auto path = Hmc2Store::filePath(root(), r.header, r.bucketStartMs);
    EXPECT_EQ(path.filename(), "1970-01-01.hmc2");
    auto b = bytes(path);
    EXPECT_EQ(std::string(b.begin(), b.begin() + 4), "HMC2");
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    const auto &got = rows[0];
    EXPECT_EQ(got.header.configHash, 42);
    EXPECT_EQ(got.header.rowTickUnits, 1000);
    EXPECT_EQ(got.bucketStartMs, r.bucketStartMs);
    EXPECT_EQ(got.observedMs, 60000);
    ASSERT_EQ(got.entries.size(), 2);
    EXPECT_EQ(got.entries[1].row, 9);
    EXPECT_TRUE(got.entries[1].isAsk);
    EXPECT_EQ(got.entries[1].peakCode, r.entries[1].peakCode);
}
TEST_F(StoreTest, ExclusiveRootLockAndRelease) {
    {
        Hmc2Store store(root());
        EXPECT_THROW(Hmc2Store second(root()), std::runtime_error);
    }
    EXPECT_NO_THROW(Hmc2Store store(root()));
}
TEST_F(StoreTest, ConfigMismatchStartsNewGenerationAndDeduplicates) {
    auto r = record();
    {
        Hmc2Store store(root());
        store.append(r);
    }
    r.header.rowTickUnits = 2000;
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, 0, 1)));
    r.header.rowTickUnits = 1000;
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, 0, 2)));
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].header.rowTickUnits, 1000);
}
TEST_F(StoreTest, IncompleteTerminalRecordIsRepairedByWriterOnly) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, 0);
    size_t firstSize;
    {
        Hmc2Store store(root());
        store.append(r);
        firstSize = std::filesystem::file_size(path);
        r.bucketStartMs = 60000;
        store.append(r);
    }
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 7);
    const auto tornSize = std::filesystem::file_size(path);
    ASSERT_EQ(read().size(), 1);
    EXPECT_EQ(std::filesystem::file_size(path), tornSize);
    {
        Hmc2Store store(root());
        r.bucketStartMs = 120000;
        store.append(r);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, 120000);
    EXPECT_GT(std::filesystem::file_size(path), firstSize);
}
TEST_F(StoreTest, IncompleteTerminalHeaderIsRepaired) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, 0);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    const auto size = std::filesystem::file_size(path);
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f.write("HCR2xxx", 7);
    }
    {
        Hmc2Store store(root());
        r.bucketStartMs = 60000;
        store.append(r);
    }
    EXPECT_EQ(read().size(), 2);
    EXPECT_GT(std::filesystem::file_size(path), size);
}
TEST_F(StoreTest, InteriorCrcMagicAndLengthCorruptionPreserveLaterRecords) {
    for (int type = 0; type < 3; ++type) {
        auto r = record();
        r.header.layer = "case" + std::to_string(type);
        const auto path = Hmc2Store::filePath(root(), r.header, 0);
        {
            Hmc2Store store(root());
            for (int t = 0; t < 3; ++t) {
                r.bucketStartMs = t * 60000;
                store.append(r);
            }
        }
        auto b = bytes(path);
        const size_t start = u32(b, 6);
        const size_t second = start + 16 + u32(b, start + 4);
        if (type == 0)
            b[second + 16] ^= 0xff;
        if (type == 1)
            b[second] ^= 0xff;
        if (type == 2) {
            b[second + 4] = 0;
            b[second + 5] = 0;
            b[second + 6] = 1;
            b[second + 7] = 0;
        }
        save(path, b);
        const auto oldSize = b.size();
        {
            Hmc2Store store(root());
            r.bucketStartMs = 180000;
            store.append(r);
        }
        EXPECT_GT(std::filesystem::file_size(path), oldSize);
        auto rows = Hmc2Store::readRange(root(), "BTC-USD", r.header.layer, 60000, 0, 240000);
        ASSERT_EQ(rows.size(), 3);
        EXPECT_EQ(rows[1].bucketStartMs, 120000);
        EXPECT_EQ(rows[2].bucketStartMs, 180000);
    }
}
TEST_F(StoreTest, CompleteTerminalCrcDamageIsNotTruncated) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, 0);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    auto b = bytes(path);
    b.back() ^= 0xff;
    save(path, b);
    {
        Hmc2Store store(root());
        r.bucketStartMs = 60000;
        store.append(r);
    }
    EXPECT_GT(std::filesystem::file_size(path), b.size());
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, 60000);
}
TEST_F(StoreTest, LastRecordPerBucketAndChronologicalRange) {
    {
        Hmc2Store store(root());
        auto r = record(60000);
        store.append(r);
        r.bucketStartMs = 0;
        store.append(r);
        r.observedMs = 1234;
        store.append(r);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].bucketStartMs, 0);
    EXPECT_EQ(rows[0].observedMs, 1234);
    rows = Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, 60000, 120000);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, 60000);
}
TEST_F(StoreTest, RawLengthBoundAndHeaderCrc) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, 0);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    auto b = bytes(path);
    auto start = u32(b, 6);
    b[start + 8] = 1;
    b[start + 9] = 0;
    b[start + 10] = 0;
    b[start + 11] = 1;
    save(path, b);
    EXPECT_TRUE(read().empty());
    b[14] ^= 1;
    save(path, b);
    EXPECT_TRUE(read().empty());
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, 0, 1)));
}
TEST_F(StoreTest, InvalidPathsAndDiskErrorsSurface) {
    auto r = record();
    r.header.symbol = "../escape";
    Hmc2Store store(root());
    EXPECT_THROW(store.append(r), std::runtime_error);
    r = record();
    {
        std::ofstream blocked(root() / "BTC-USD");
        blocked << 'x';
    }
    EXPECT_THROW(store.append(r), std::exception);
}
TEST_F(StoreTest, SignedRowDeltasRoundTrip) {
    auto r = record();
    r.entries = {
        {INT64_MIN, false, 1, 1}, {INT64_MIN + 1, true, 2, 3}, {-2, false, 4, 5}, {-2, true, 6, 7}, {3, true, 8, 9}};
    {
        Hmc2Store store(root());
        store.append(r);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    ASSERT_EQ(rows[0].entries.size(), r.entries.size());
    for (size_t i = 0; i < r.entries.size(); ++i)
        EXPECT_EQ(rows[0].entries[i].row, r.entries[i].row);
}
TEST_F(StoreTest, GarbageTailIsRetainedAndHeaderHashMismatchGenerates) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, 0);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f.write("garbage", 7);
    }
    const auto damagedSize = std::filesystem::file_size(path);
    {
        Hmc2Store store(root());
        r.bucketStartMs = 60000;
        store.append(r);
    }
    EXPECT_GT(std::filesystem::file_size(path), damagedSize);
    EXPECT_EQ(read().size(), 2);
    r.header.configHash = 43;
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, 0, 1)));
}
} // namespace
