#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>
#include <barrier>
#include <thread>
#include <future>
#include <zstd.h>
#include "servermodel/HmcolFormat.hpp"
#include "servermodel/PersistenceIo.hpp"

using namespace recording;
namespace {
constexpr int64_t kEpoch = kHmc2MinMs;
class StoreTest : public testing::Test {
  protected:
    QTemporaryDir dir;
    std::filesystem::path root() {
        return dir.path().toStdString();
    }
    Hmc2Record record(int64_t t = 0) {
        Hmc2Record r;
        r.header = {"BTC-USD", "deep", 60000, 100, 1000, {}, 42};
        r.bucketStartMs = kEpoch + t;
        r.observedMs = 60000;
        r.bidRowLo = r.askRowLo = 0;
        r.bidRowHi = r.askRowHi = 100;
        r.midOpen = r.midClose = r.midMin = r.midMax = 100;
        r.entries = {{9, false, encodeSize(2), encodeSize(4)}, {9, true, encodeSize(3), encodeSize(6)}};
        return r;
    }
    std::vector<Hmc2Record> read() {
        return Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, kEpoch, kEpoch + 172800000);
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
    void set32(std::vector<uint8_t> &b, size_t p, uint32_t value) {
        for (size_t i = 0; i < 4; ++i)
            b[p + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    std::vector<uint8_t> frameFor(const Hmc2Record &r) {
        QTemporaryDir other;
        const std::filesystem::path path = other.path().toStdString();
        {
            Hmc2Store store(path);
            store.append(r);
        }
        auto b = bytes(Hmc2Store::filePath(path, r.header, r.bucketStartMs));
        return {b.begin() + u32(b, 6), b.end()};
    }
    std::vector<uint8_t> rawPayload(const std::vector<uint8_t> &frame) {
        std::vector<uint8_t> raw(u32(frame, 8));
        const auto n = ZSTD_decompress(raw.data(), raw.size(), frame.data() + 16, frame.size() - 16);
        EXPECT_FALSE(ZSTD_isError(n));
        EXPECT_EQ(n, raw.size());
        return raw;
    }
    std::vector<uint8_t> frameFromRaw(const std::vector<uint8_t> &raw, bool multiFrame = false) {
        std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));
        const auto n = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
        EXPECT_FALSE(ZSTD_isError(n));
        compressed.resize(n);
        if (multiFrame) {
            const auto second = compressed;
            compressed.insert(compressed.end(), second.begin(), second.end());
        }
        std::vector<uint8_t> frame(16);
        set32(frame, 0, kHmc2RecordMagic);
        set32(frame, 4, static_cast<uint32_t>(compressed.size()));
        set32(frame, 8, static_cast<uint32_t>(raw.size() * (multiFrame ? 2 : 1)));
        set32(frame, 12, hmcol::crc32(compressed.data(), compressed.size()));
        frame.insert(frame.end(), compressed.begin(), compressed.end());
        return frame;
    }
};
TEST_F(StoreTest, RoundTripAndBucketUtcPath) {
    auto r = record(86400000 - 60000);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    const auto path = Hmc2Store::filePath(root(), r.header, r.bucketStartMs);
    EXPECT_EQ(path.filename(), "2000-01-01.hmc2");
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
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, kEpoch, 1)));
    r.header.rowTickUnits = 1000;
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, kEpoch, 2)));
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].header.rowTickUnits, 1000);
}
TEST_F(StoreTest, IncompleteTerminalRecordIsRepairedByWriterOnly) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    size_t firstSize;
    {
        Hmc2Store store(root());
        store.append(r);
        firstSize = std::filesystem::file_size(path);
        r.bucketStartMs = kEpoch + 60000;
        store.append(r);
    }
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 7);
    const auto tornSize = std::filesystem::file_size(path);
    ASSERT_EQ(read().size(), 1);
    EXPECT_EQ(std::filesystem::file_size(path), tornSize);
    {
        Hmc2Store store(root());
        r.bucketStartMs = kEpoch + 120000;
        store.append(r);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, kEpoch + 120000);
    EXPECT_EQ(std::filesystem::file_size(path), firstSize + frameFor(r).size());
}
TEST_F(StoreTest, IncompleteTerminalHeaderIsRepaired) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
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
        r.bucketStartMs = kEpoch + 60000;
        store.append(r);
    }
    EXPECT_EQ(read().size(), 2);
    EXPECT_EQ(std::filesystem::file_size(path), size + frameFor(r).size());
}
TEST_F(StoreTest, InteriorCrcMagicAndLengthCorruptionPreserveLaterRecords) {
    for (int type = 0; type < 3; ++type) {
        auto r = record();
        r.header.layer = "case" + std::to_string(type);
        const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
        {
            Hmc2Store store(root());
            for (int t = 0; t < 3; ++t) {
                r.bucketStartMs = kEpoch + t * 60000;
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
            r.bucketStartMs = kEpoch + 180000;
            store.append(r);
        }
        EXPECT_GT(std::filesystem::file_size(path), oldSize);
        auto rows = Hmc2Store::readRange(root(), "BTC-USD", r.header.layer, 60000, kEpoch, kEpoch + 240000);
        ASSERT_EQ(rows.size(), 3);
        EXPECT_EQ(rows[1].bucketStartMs, kEpoch + 120000);
        EXPECT_EQ(rows[2].bucketStartMs, kEpoch + 180000);
    }
}
TEST_F(StoreTest, CompleteTerminalCrcDamageIsNotTruncated) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    auto b = bytes(path);
    b.back() ^= 0xff;
    save(path, b);
    {
        Hmc2Store store(root());
        r.bucketStartMs = kEpoch + 60000;
        store.append(r);
    }
    EXPECT_GT(std::filesystem::file_size(path), b.size());
    auto rows = read();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, kEpoch + 60000);
}
TEST_F(StoreTest, LastRecordPerBucketAndChronologicalRange) {
    {
        Hmc2Store store(root());
        auto r = record(60000);
        store.append(r);
        r.bucketStartMs = kEpoch + 0;
        store.append(r);
        r.observedMs = 1234;
        store.append(r);
    }
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].bucketStartMs, kEpoch + 0);
    EXPECT_EQ(rows[0].observedMs, 1234);
    rows = Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, kEpoch + 60000, kEpoch + 120000);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, kEpoch + 60000);
}
TEST_F(StoreTest, RawLengthBoundAndHeaderCrc) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
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
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, kEpoch, 1)));
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
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
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
        r.bucketStartMs = kEpoch + 60000;
        store.append(r);
    }
    EXPECT_GT(std::filesystem::file_size(path), damagedSize);
    EXPECT_EQ(read().size(), 2);
    r.header.configHash = 43;
    {
        Hmc2Store store(root());
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, kEpoch, 1)));
}
TEST_F(StoreTest, FailedAppendRollsBackAndNextAppendReopensCachedWriter) {
    auto first = record();
    auto next = record(60000);
    const auto path = Hmc2Store::filePath(root(), first.header, kEpoch);
    Hmc2Store store(root());
    store.append(first);
    const auto before = bytes(path);
    store.afterFrameHeaderForTest([&] {
        EXPECT_EQ(std::filesystem::file_size(path), before.size() + 16);
        throw std::runtime_error("injected write failure after frame header");
    });
    EXPECT_THROW(store.append(next), std::runtime_error);
    EXPECT_EQ(bytes(path), before);
    EXPECT_NO_THROW(store.append(next));
    EXPECT_EQ(std::filesystem::file_size(path), before.size() + frameFor(next).size());
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, next.bucketStartMs);
}
TEST_F(StoreTest, FalseMagicAndPastEofCandidateAfterDamageNeverTruncate) {
    auto first = record();
    auto next = record(60000);
    const auto path = Hmc2Store::filePath(root(), first.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(first);
    }
    auto damaged = bytes(path);
    damaged.insert(damaged.end(), 37, 0xDD);
    auto falseFrame = frameFor(record(120000));
    falseFrame[12] ^= 0xFF; // Plausible framing and valid zstd, but bad CRC.
    damaged.insert(damaged.end(), falseFrame.begin(), falseFrame.end());
    std::vector<uint8_t> pastEof(19, 0xEE);
    set32(pastEof, 0, kHmc2RecordMagic);
    set32(pastEof, 4, 4096);
    set32(pastEof, 8, 84);
    set32(pastEof, 12, 0);
    damaged.insert(damaged.end(), pastEof.begin(), pastEof.end());
    save(path, damaged);
    ASSERT_EQ(read().size(), 1);
    EXPECT_EQ(bytes(path), damaged);
    {
        Hmc2Store store(root());
        store.append(next);
    }
    const auto after = bytes(path);
    ASSERT_EQ(after.size(), damaged.size() + frameFor(next).size());
    EXPECT_TRUE(std::equal(damaged.begin(), damaged.end(), after.begin()));
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, next.bucketStartMs);
}
TEST_F(StoreTest, TornTailImmediatelyAfterInteriorDamageIsRetained) {
    auto first = record();
    const auto path = Hmc2Store::filePath(root(), first.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(first);
    }
    auto damaged = bytes(path);
    auto bad = frameFor(record(60000));
    bad[12] ^= 1;
    auto torn = frameFor(record(120000));
    torn.resize(torn.size() - 7);
    damaged.insert(damaged.end(), bad.begin(), bad.end());
    damaged.insert(damaged.end(), torn.begin(), torn.end());
    save(path, damaged);
    auto next = record(180000);
    testing::internal::CaptureStderr();
    {
        Hmc2Store store(root());
        store.append(next);
    }
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("kept unverified tail after interior damage"), std::string::npos);
    const auto after = bytes(path);
    ASSERT_EQ(after.size(), damaged.size() + frameFor(next).size());
    EXPECT_TRUE(std::equal(damaged.begin(), damaged.end(), after.begin()));
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, next.bucketStartMs);
}
TEST_F(StoreTest, ResyncMagicStraddlesScanChunkBoundary) {
    auto first = record();
    auto next = record(60000);
    const auto path = Hmc2Store::filePath(root(), first.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(first);
    }
    auto b = bytes(path);
    // Scan starts one byte after damage. Its second 65536-byte chunk ends
    // three bytes into the next magic; only the overlap can recover it.
    b.insert(b.end(), 65534 + 65533, 0xD3);
    const auto frame = frameFor(next);
    b.insert(b.end(), frame.begin(), frame.end());
    save(path, b);
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].bucketStartMs, next.bucketStartMs);
    EXPECT_EQ(bytes(path), b);
}
TEST_F(StoreTest, EmptyAndHalfWrittenHeadersSurviveNewGenerationCreation) {
    for (size_t length : {size_t{0}, size_t{9}}) {
        auto r = record();
        r.header.layer = "header" + std::to_string(length);
        const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
        std::filesystem::create_directories(path.parent_path());
        std::vector<uint8_t> partial{'H', 'M', 'C', '2', 2, 0, 80, 0, 0};
        partial.resize(length);
        save(path, partial);
        {
            Hmc2Store store(root());
            store.append(r);
        }
        EXPECT_EQ(bytes(path), partial);
        EXPECT_TRUE(std::filesystem::exists(Hmc2Store::filePath(root(), r.header, kEpoch, 1)));
        auto rows = Hmc2Store::readRange(root(), r.header.symbol, r.header.layer, 60000, kEpoch, kEpoch + 60000);
        ASSERT_EQ(rows.size(), 1);
    }
}
TEST_F(StoreTest, CrcValidUndecodablePayloadsAndMultipleZstdFramesAreSkipped) {
    for (int kind = 0; kind < 4; ++kind) {
        auto r = record();
        r.header.layer = "decode" + std::to_string(kind);
        const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
        {
            Hmc2Store store(root());
            store.append(r);
        }
        auto b = bytes(path);
        auto middle = r;
        middle.bucketStartMs += 60000;
        auto raw = rawPayload(frameFor(middle));
        if (kind == 0)
            set32(raw, 8, 60001); // observedMs exceeds timeframe
        if (kind == 1)
            raw[0] ^= 1; // bucket not aligned
        if (kind == 2)
            raw[91] &= 0x7F; // second entry duplicates first row/side
        auto bad = frameFromRaw(raw, kind == 3);
        EXPECT_EQ(u32(bad, 12), hmcol::crc32(bad.data() + 16, bad.size() - 16));
        b.insert(b.end(), bad.begin(), bad.end());
        auto last = r;
        last.bucketStartMs += 120000;
        auto good = frameFor(last);
        b.insert(b.end(), good.begin(), good.end());
        save(path, b);
        auto rows = Hmc2Store::readRange(root(), r.header.symbol, r.header.layer, 60000, kEpoch, kEpoch + 180000);
        ASSERT_EQ(rows.size(), 2) << kind;
        EXPECT_EQ(rows.back().bucketStartMs, last.bucketStartMs);
        EXPECT_EQ(bytes(path), b);
    }
}
TEST_F(StoreTest, RangeSpansUtcDaysAndHourlyPath) {
    auto a = record(86400000 - 60000), b = record(86400000), hour = record(86400000);
    hour.header.tfMs = 3600000;
    hour.observedMs = 120000;
    {
        Hmc2Store store(root());
        store.append(b);
        store.append(a);
        store.append(hour);
    }
    auto rows =
        Hmc2Store::readRange(root(), a.header.symbol, a.header.layer, 60000, a.bucketStartMs, b.bucketStartMs + 60000);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].bucketStartMs, a.bucketStartMs);
    EXPECT_EQ(rows[1].bucketStartMs, b.bucketStartMs);
    rows = Hmc2Store::readRange(root(), hour.header.symbol, hour.header.layer, 3600000, hour.bucketStartMs,
                                hour.bucketStartMs + 3600000);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, 120000);
    EXPECT_EQ(Hmc2Store::filePath(root(), hour.header, hour.bucketStartMs).parent_path().filename(), "deep-3600000");
}
TEST_F(StoreTest, TimestampsAreBoundedBeforeCalendarConversionAndReadsClamp) {
    Hmc2Store store(root());
    auto r = record();
    store.append(r);
    r.bucketStartMs = kHmc2EndMs - 60000;
    store.append(r);
    for (int64_t t : {kHmc2MinMs - 60000, kHmc2EndMs, int64_t{1'800'000'000'000'000}, INT64_MIN, INT64_MAX}) {
        r.bucketStartMs = t;
        EXPECT_THROW(store.append(r), std::runtime_error) << t;
        EXPECT_THROW(Hmc2Store::filePath(root(), r.header, t), std::runtime_error) << t;
    }
    auto rows = Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, INT64_MIN, INT64_MAX);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.front().bucketStartMs, kHmc2MinMs);
    EXPECT_EQ(rows.back().bucketStartMs, kHmc2EndMs - 60000);
    EXPECT_TRUE(Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, INT64_MIN, kHmc2MinMs).empty());
    EXPECT_TRUE(Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000, kHmc2EndMs, INT64_MAX).empty());
}
TEST_F(StoreTest, ConcurrentReadersIgnorePartialAppendsWithoutRepairing) {
    auto first = record(), next = record(60000);
    const auto path = Hmc2Store::filePath(root(), first.header, kEpoch);
    Hmc2Store store(root());
    store.append(first);
    const auto initial = bytes(path);
    std::barrier rendezvous(2);
    store.afterFrameHeaderForTest([&] {
        rendezvous.arrive_and_wait(); // Reader observes the flushed partial frame.
        rendezvous.arrive_and_wait(); // Reader has finished checking byte preservation.
    });
    auto writer = std::async(std::launch::async, [&] { store.append(next); });
    rendezvous.arrive_and_wait();
    const auto partial = bytes(path);
    EXPECT_EQ(partial.size(), initial.size() + 16);
    for (int n = 0; n < 4; ++n) {
        auto rows = read();
        EXPECT_EQ(rows.size(), 1);
        EXPECT_EQ(bytes(path), partial);
    }
    rendezvous.arrive_and_wait();
    EXPECT_NO_THROW(writer.get());
    auto rows = read();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(std::filesystem::file_size(path), initial.size() + frameFor(next).size());
}
TEST_F(StoreTest, UnreadableDayDoesNotDiscardOtherDays) {
#ifndef _WIN32
    auto a = record(), b = record(86400000);
    {
        Hmc2Store store(root());
        store.append(a);
        store.append(b);
    }
    const auto path = Hmc2Store::filePath(root(), a.header, a.bucketStartMs);
    const auto permissions = std::filesystem::status(path).permissions();
    std::filesystem::permissions(path, std::filesystem::perms::none);
    std::ifstream denied(path);
    if (denied.is_open()) {
        std::filesystem::permissions(path, permissions);
        GTEST_SKIP() << "Process bypasses file permission restrictions";
    }
    auto rows = read();
    std::filesystem::permissions(path, permissions);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].bucketStartMs, b.bucketStartMs);
#else
    GTEST_SKIP() << "POSIX permission fixture";
#endif
}
TEST_F(StoreTest, DirectoryEnumerationErrorReturnsPartialHistoryWithoutThrowing) {
    std::filesystem::create_directory(root() / "BTC-USD");
    {
        std::ofstream blocked(root() / "BTC-USD" / "deep-60000");
        blocked << 'x';
    }
    EXPECT_NO_THROW(EXPECT_TRUE(read().empty()));
}
TEST_F(StoreTest, ExclusiveCreateNeverOverwritesAnExistingGeneration) {
    const auto path = root() / "existing.hmc2";
    const std::vector<uint8_t> original{'h', 'i', 's', 't', 'o', 'r', 'y'};
    save(path, original);
    int error = 0;
    const std::vector<uint8_t> replacement{'x'};
    EXPECT_FALSE(sentinel::persistence::writeNewFileExclusive(path, replacement, error));
    EXPECT_NE(error, 0);
    EXPECT_EQ(bytes(path), original);
}
TEST_F(StoreTest, GenerationCollisionCannotOverwriteTheBaseFile) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(r);
    }
    const auto original = bytes(path);
    // Force a wrapped generation selection back to the occupied base filename.
    // Exclusive creation must protect it even if selection produces a collision.
    std::filesystem::copy_file(path, Hmc2Store::filePath(root(), r.header, kEpoch, UINT32_MAX));
    r.header.configHash += 1;
    Hmc2Store store(root());
    EXPECT_THROW(store.append(r), std::runtime_error);
    EXPECT_EQ(bytes(path), original);
}
} // namespace
