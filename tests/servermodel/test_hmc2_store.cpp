#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>
#include <barrier>
#include <thread>
#include <future>
#include <bit>
#include <array>
#include <algorithm>
#include <iostream>
#include <random>
#include <zstd.h>
#include "servermodel/HmcolFormat.hpp"
#include "servermodel/PersistenceIo.hpp"
#include "servermodel/RecordingDir.hpp"

using namespace recording;

// Some cases capture stderr to check store warnings. Without a console (ctest,
// CI) Qt on Windows logs to OutputDebugString instead; force stderr before main.
static const bool kQtLogsToStderr = qputenv("QT_FORCE_STDERR_LOGGING", "1");
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
    template <class T> void scalar(std::vector<uint8_t> &b, T value) {
        auto raw = std::bit_cast<std::array<uint8_t, sizeof(T)>>(value);
        if constexpr (std::endian::native == std::endian::big)
            std::reverse(raw.begin(), raw.end());
        b.insert(b.end(), raw.begin(), raw.end());
    }
    std::vector<uint8_t> absolutePayload(const Hmc2Record &r) {
        std::vector<uint8_t> b;
        scalar(b, r.bucketStartMs);
        scalar(b, r.observedMs);
        scalar(b, r.flags);
        for (auto v : {r.bidRowLo, r.bidRowHi, r.askRowLo, r.askRowHi})
            scalar(b, v);
        for (auto v : {r.midOpen, r.midClose, r.midMin, r.midMax})
            scalar(b, v);
        scalar(b, static_cast<uint32_t>(r.entries.size()));
        int64_t previous = 0;
        for (const auto &e : r.entries) {
            putVarint(b, zigzag(e.row - previous));
            scalar(b, withSide(e.twapCode, e.isAsk));
            scalar(b, e.peakCode);
            previous = e.row;
        }
        return b;
    }
    void legacyFile(const std::filesystem::path &path, uint16_t schema, const std::vector<Hmc2Record> &records) {
        const auto &h = records.front().header;
        std::vector<uint8_t> b;
        scalar(b, kHmc2Magic);
        scalar(b, schema);
        scalar(b, uint32_t{0});
        scalar(b, uint32_t{0});
        for (const auto &str : {h.symbol, h.layer}) {
            scalar(b, static_cast<uint16_t>(str.size()));
            b.insert(b.end(), str.begin(), str.end());
        }
        scalar(b, h.tfMs);
        scalar(b, h.priceScale);
        scalar(b, h.rowTickUnits);
        scalar(b, h.sizeScale.floor);
        scalar(b, h.sizeScale.codesPerOctave);
        scalar(b, h.configHash);
        set32(b, 6, b.size());
        set32(b, 10, hmcol::crc32(b.data(), b.size()));
        for (const auto &r : records) {
            const auto frame = frameFromRaw(absolutePayload(r));
            b.insert(b.end(), frame.begin(), frame.end());
        }
        std::filesystem::create_directories(path.parent_path());
        save(path, b);
    }
    std::vector<std::vector<uint8_t>> frames(const std::filesystem::path &path) {
        const auto b = bytes(path);
        std::vector<std::vector<uint8_t>> out;
        for (size_t pos = u32(b, 6); pos < b.size();) {
            const size_t end = pos + 16 + u32(b, pos + 4);
            out.emplace_back(b.begin() + pos, b.begin() + end);
            pos = end;
        }
        return out;
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
// The server and the lab share this rule (recording.dir, fallback while its
// /Volumes/<name> volume is unmounted). "/Volumes/..." has no drive on Windows,
// so it must count as unmounted there too, never become C:\Volumes\...
TEST(RecordingDir, UnmountedVolumeUsesFallbackOrNothing) {
    const std::string absent = "/Volumes/SentinelDefinitelyAbsentVolume/recording";
    EXPECT_FALSE(volumeMounted(absent));
    auto choice = resolveRecordingDir(absent, "data/recording");
    EXPECT_EQ(choice.dir, std::filesystem::path("data/recording"));
    EXPECT_TRUE(choice.fallback);
    choice = resolveRecordingDir(absent, "");
    EXPECT_TRUE(choice.dir.empty());
    const auto plain = std::filesystem::temp_directory_path() / "recording";
    choice = resolveRecordingDir(plain.string(), "data/recording");
    EXPECT_EQ(choice.dir, plain);
    EXPECT_FALSE(choice.fallback);
    EXPECT_TRUE(volumeMounted("data/recording")); // relative: the working directory decides
}

#ifdef _WIN32
// "/Volumes/<name>" is a macOS mount point. On Windows it names C:\Volumes\<name>,
// which may exist (left behind by an older build); it must still count as not
// mounted, or the server records to the boot disk.
TEST(RecordingDir, VolumesPathIsNeverMountedOnWindowsEvenIfTheDirectoryExists) {
    namespace fs = std::filesystem;
    const fs::path volumes("/Volumes");
    const fs::path volume = volumes / "SentinelTestVolume-RecordingDir";
    const bool hadVolumes = fs::exists(volumes);
    std::error_code ec;
    fs::create_directories(volume, ec);
    if (ec) GTEST_SKIP() << "cannot create " << fs::absolute(volume).string() << ": " << ec.message();
    const bool mounted = volumeMounted(volume / "recording");
    fs::remove(volume, ec);
    if (!hadVolumes) fs::remove(volumes, ec);
    EXPECT_FALSE(mounted);
    const auto choice = resolveRecordingDir((volume / "recording").generic_string(), "data/recording");
    EXPECT_TRUE(choice.fallback);
}
#endif

// Every platform: a store opens under a fresh temp directory whose chain up to
// the drive root includes directories the user cannot fsync (C:\ on Windows),
// creates its missing directories, writes and reads back.
TEST_F(StoreTest, OpensAndWritesUnderFreshNestedDirectory) {
    const auto nested = root() / "fresh" / "nested" / "recording";
    ASSERT_FALSE(std::filesystem::exists(nested));
    const auto r = record(120000);
    {
        Hmc2Store store(nested);
        store.append(r);
    }
    EXPECT_TRUE(std::filesystem::is_directory(nested));
    const auto rows = Hmc2Store::readRange(nested, "BTC-USD", "deep", 60000, kEpoch, kEpoch + 172800000);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].bucketStartMs, r.bucketStartMs);
}

// An append creates root/BTC-USD, then fsync of root fails. The retry finds
// BTC-USD existing; it must still fsync root before reporting success, or a
// power loss can drop the directory entry that holds written records.
TEST_F(StoreTest, FailedParentSyncIsRetriedBeforeTheNextAppendSucceeds) {
    Hmc2Store store(root());
    const auto rootDir = std::filesystem::absolute(root()).lexically_normal();
    std::vector<std::filesystem::path> synced;
    bool failRoot = true;
    store.beforeDirectorySyncForTest([&](const std::filesystem::path &dir) {
        synced.push_back(std::filesystem::absolute(dir).lexically_normal());
        if (failRoot && synced.back() == rootDir) {
            failRoot = false;
            throw std::runtime_error("injected directory sync failure");
        }
    });
    const auto r = record();
    EXPECT_THROW(store.append(r), std::runtime_error);
    ASSERT_FALSE(failRoot) << "the first append must have tried to sync the root";
    synced.clear();
    store.append(r);
    EXPECT_NE(std::find(synced.begin(), synced.end(), rootDir), synced.end())
        << "the retry reported success without re-syncing the root";
    EXPECT_EQ(read().size(), 1u);
}

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
        ASSERT_EQ(rows.size(), 2);
        EXPECT_EQ(rows[1].bucketStartMs, kEpoch + 180000);
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
            raw[94] = 2; // invalid side byte
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
    for (auto &e : hour.entries) e.coveredMs = hour.observedMs;
    hour.coverage = {{0, 100, false, hour.observedMs}, {0, 100, true, hour.observedMs}};
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
    EXPECT_EQ(std::filesystem::file_size(path), initial.size() + 16 + u32(partial, initial.size() + 4));
    EXPECT_EQ(absolutePayload(rows.back()), absolutePayload(next));
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
TEST_F(StoreTest, LegacySchemasReadAndForceANewGeneration) {
    for (uint16_t schema : {1, 2}) {
        auto a = record(), b = record(60000);
        a.header.layer = b.header.layer = "legacy" + std::to_string(schema);
        b.entries[0].twapCode -= 5;
        const auto path = Hmc2Store::filePath(root(), a.header, kEpoch);
        legacyFile(path, schema, {a, b});
        const auto original = bytes(path);
        auto rows = Hmc2Store::readRange(root(), "BTC-USD", a.header.layer, 60000, kEpoch, kEpoch + 120000);
        ASSERT_EQ(rows.size(), 2);
        EXPECT_EQ(absolutePayload(rows[0]), absolutePayload(a));
        EXPECT_EQ(absolutePayload(rows[1]), absolutePayload(b));
        {
            Hmc2Store store(root());
            b.bucketStartMs += 60000;
            store.append(b);
        }
        EXPECT_EQ(bytes(path), original);
        const auto next = Hmc2Store::filePath(root(), b.header, kEpoch, 1);
        EXPECT_EQ(bytes(next)[4], 3);
        EXPECT_EQ(rawPayload(frames(next)[0])[84], 0);
    }
}
TEST_F(StoreTest, KeyframesOnBoundaryGapReopenAndRollback) {
    auto r = record(13 * 60000);
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    {
        Hmc2Store store(root());
        for (int minute : {13, 14, 15, 16, 18, 19}) {
            r.bucketStartMs = kEpoch + minute * 60000;
            store.append(r);
        }
    }
    {
        Hmc2Store store(root());
        r.bucketStartMs += 60000; // reopen at 20
        store.append(r);
        r.bucketStartMs += 60000;
        store.afterFrameHeaderForTest([] { throw std::runtime_error("rollback"); });
        EXPECT_THROW(store.append(r), std::runtime_error);
        store.append(r); // rollback forces keyframe at 21
        r.bucketStartMs += 60000;
        store.append(r);
    }
    const auto all = frames(path);
    const std::vector<uint8_t> kinds{0, 1, 0, 1, 0, 1, 0, 0, 1};
    ASSERT_EQ(all.size(), kinds.size());
    for (size_t j = 0; j < all.size(); ++j)
        EXPECT_EQ(rawPayload(all[j])[84], kinds[j]) << j;
    EXPECT_EQ(read().size(), all.size());
}
TEST_F(StoreTest, DeltaRemovalsNewKeysPeakOnlyAndBoundsChanges) {
    std::vector<Hmc2Record> expected;
    auto r = record();
    {
        Hmc2Store store(root());
        auto append = [&] {
            store.append(r);
            expected.push_back(r);
            r.bucketStartMs += 60000;
        };
        append();
        r.entries[0].peakCode += 1; // peak-only changes are not omitted
        r.entries[1].twapCode -= 7;
        append();
        r.entries.erase(r.entries.begin()); // remove bid while keeping ask at same row
        r.entries.push_back({10, false, 1, 2});
        r.bidRowLo = 10;
        r.bidRowHi = 20;
        r.midMin = 99;
        r.observedMs = 57000;
        r.flags = kPartial | kLateEvents;
        append();
        r.entries.clear();
        r.askRowLo = 1;
        r.askRowHi = 0;
        append();
        r.entries = {{-2, true, kMaxCode, 1}, {20, false, 1, kMaxCode}};
        append();
        append(); // zero changed keys
    }
    const auto got = read();
    ASSERT_EQ(got.size(), expected.size());
    for (size_t j = 0; j < got.size(); ++j)
        EXPECT_EQ(absolutePayload(got[j]), absolutePayload(expected[j])) << j;
}
TEST_F(StoreTest, SyntheticStreamEqualsAbsoluteAndDeltasAreSmaller) {
    std::mt19937 rng(42);
    auto r = record();
    r.entries.clear();
    for (int j = 0; j < 12000; ++j) {
        const auto twap = static_cast<uint16_t>(1000 + rng() % 28000);
        r.entries.push_back({j - 6000, j % 2 != 0, twap, static_cast<uint16_t>(twap + rng() % 300)});
    }
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    std::vector<Hmc2Record> expected;
    {
        Hmc2Store store(root());
        for (int minute = 0; minute < 46; ++minute) {
            r.bucketStartMs = kEpoch + minute * 60000;
            for (int j = 0; j < 120; ++j) {
                auto &entry = r.entries[rng() % r.entries.size()];
                entry.twapCode += (j % 2 ? 1 : -1);
                entry.peakCode += (j % 2 ? 2 : -2);
            }
            r.bidRowLo = -6000 + minute;
            r.askRowHi = 6000 - minute;
            r.midClose += 0.125;
            r.flags = minute % 4;
            store.append(r);
            expected.push_back(r);
        }
    }
    QTemporaryDir legacyRoot;
    const auto legacyPath = Hmc2Store::filePath(legacyRoot.path().toStdString(), r.header, kEpoch);
    legacyFile(legacyPath, 2, expected);
    const auto absolute = Hmc2Store::readRange(legacyRoot.path().toStdString(), "BTC-USD", "deep", 60000,
                                             kEpoch, kEpoch + 46 * 60000);
    const auto delta = read();
    ASSERT_EQ(absolute.size(), expected.size());
    ASSERT_EQ(delta.size(), expected.size());
    for (size_t j = 0; j < delta.size(); ++j) {
        EXPECT_EQ(absolutePayload(delta[j]), absolutePayload(absolute[j])) << j;
        EXPECT_EQ(absolutePayload(delta[j]), absolutePayload(expected[j])) << j;
    }
    for (int start : {1, 7, 14, 15, 16, 29, 44}) {
        auto part = Hmc2Store::readRange(root(), "BTC-USD", "deep", 60000,
                                       kEpoch + start * 60000 + 1, kEpoch + (start + 2) * 60000);
        ASSERT_EQ(part.size(), 1) << start;
        EXPECT_EQ(absolutePayload(part.front()), absolutePayload(expected[start + 1]));
    }
    size_t keys = 0, deltas = 0, keyBytes = 0, deltaBytes = 0;
    for (const auto &frame : frames(path)) {
        if (rawPayload(frame)[84] == 0) {
            ++keys;
            keyBytes += frame.size();
        } else {
            ++deltas;
            deltaBytes += frame.size();
        }
    }
    ASSERT_EQ(keys, 4);
    ASSERT_EQ(deltas, 42);
    const double keyMean = double(keyBytes) / keys, deltaMean = double(deltaBytes) / deltas;
    std::cout << "Synthetic 12000-key book, 1% updates/minute: keyframe=" << keyMean
              << " bytes/record, delta=" << deltaMean << " bytes/record (framing + zstd L3)\n";
    EXPECT_LT(deltaMean, keyMean / 10);
}
TEST_F(StoreTest, CorruptOrWrongBaseDeltaSkipsChainUntilKeyframe) {
    for (int kind = 0; kind < 5; ++kind) {
        auto r = record();
        r.header.layer = "chain" + std::to_string(kind);
        const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
        {
            Hmc2Store store(root());
            for (int minute = 0; minute < 18; ++minute) {
                r.bucketStartMs = kEpoch + minute * 60000;
                ++r.entries[0].twapCode;
                store.append(r);
            }
        }
        auto b = bytes(path);
        auto all = frames(path);
        auto raw = rawPayload(all[2]);
        if (kind == 0)
            all[2][12] ^= 1; // CRC corruption
        if (kind == 1) {
            raw[85] ^= 1; // referenced bucket is not the previous bucket
            all[2] = frameFromRaw(raw);
        }
        if (kind == 2)
            all[2].clear(); // physically missing predecessor
        if (kind == 3) {
            raw.resize(95);
            raw.push_back(0x80); // truncated code varint
            all[2] = frameFromRaw(raw);
        }
        if (kind == 4) {
            raw[93] = 0xff; // malformed row varint consumes entry payload
            all[2] = frameFromRaw(raw);
        }
        b.resize(u32(b, 6));
        for (const auto &frame : all)
            b.insert(b.end(), frame.begin(), frame.end());
        save(path, b);
        const auto got = Hmc2Store::readRange(root(), "BTC-USD", r.header.layer, 60000, kEpoch, kEpoch + 18 * 60000);
        ASSERT_EQ(got.size(), 5) << kind;
        EXPECT_EQ(got[1].bucketStartMs, kEpoch + 60000);
        EXPECT_EQ(got[2].bucketStartMs, kEpoch + 15 * 60000);
        EXPECT_EQ(absolutePayload(got.back()), absolutePayload(r));
        EXPECT_EQ(bytes(path), b); // reader never repairs
    }
}
TEST_F(StoreTest, ZeroTwapPeakIsPreservedByKeyframeThenRemovedByDelta) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    std::vector<Hmc2Record> expected;
    {
        Hmc2Store store(root());
        store.append(r);
        expected.push_back(r);
        r.bucketStartMs += 60000;
        r.entries[0].twapCode = 0; // observed instantaneous peak, no time integral
        store.append(r);
        expected.push_back(r);
        r.bucketStartMs += 60000;
        r.entries.erase(r.entries.begin());
        store.append(r);
        expected.push_back(r);
    }
    const auto all = frames(path);
    EXPECT_EQ(rawPayload(all[1])[84], 0);
    EXPECT_EQ(rawPayload(all[2])[84], 1);
    const auto got = read();
    ASSERT_EQ(got.size(), expected.size());
    for (size_t j = 0; j < got.size(); ++j)
        EXPECT_EQ(absolutePayload(got[j]), absolutePayload(expected[j]));
}
TEST_F(StoreTest, EvictionForgetsBaseAndSeriesNeverShareIt) {
    auto r = record();
    r.header.layer = "a";
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    Hmc2Store store(root());
    store.append(r);
    for (int j = 0; j < 64; ++j) {
        auto other = record();
        other.header.layer = "z" + std::to_string(j);
        store.append(other);
    }
    r.bucketStartMs += 60000;
    store.append(r);
    const auto all = frames(path);
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(rawPayload(all[1])[84], 0);
}
TEST_F(StoreTest, SparseExtremeRowJumpFallsBackToKeyframe) {
    auto r = record();
    r.entries = {{INT64_MIN, false, 1, 2}, {-2, false, 1, 2}, {3, true, 1, 2}};
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    Hmc2Store store(root());
    store.append(r);
    r.bucketStartMs += 60000;
    ++r.entries.front().peakCode;
    ++r.entries.back().peakCode;
    store.append(r);
    EXPECT_EQ(rawPayload(frames(path)[1])[84], 0);
    const auto got = read();
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(absolutePayload(got.back()), absolutePayload(r));
}
TEST_F(StoreTest, GenerationCannotSupplyAnotherFilesMissingBase) {
    auto r = record();
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    {
        Hmc2Store store(root());
        store.append(r);
        r.bucketStartMs += 60000;
        ++r.entries[0].twapCode;
        store.append(r);
    }
    auto b = bytes(path);
    const auto all = frames(path);
    b.resize(u32(b, 6));
    b.insert(b.end(), all[1].begin(), all[1].end());
    const auto generationPath = Hmc2Store::filePath(root(), r.header, kEpoch, 1);
    save(generationPath, b); // orphan in higher generation must not override valid record
    auto changed = rawPayload(all[1]);
    changed.back() ^= 2; // valid different peak offset if incorrectly given the other file's base
    b.resize(u32(b, 6));
    const auto frame = frameFromRaw(changed);
    b.insert(b.end(), frame.begin(), frame.end());
    save(generationPath, b);
    const auto got = read();
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(absolutePayload(got.back()), absolutePayload(r));
}
} // namespace

TEST_F(StoreTest, LegacyHoursExposeApproximateCoverageForSchemasOneTwoAndThree) {
    for (uint16_t schema : {1, 2, 3}) {
        auto h = record();
        h.header.tfMs = 3600000;
        h.header.symbol = "LEGACY-" + std::to_string(schema);
        const auto path = Hmc2Store::filePath(root(), h.header, h.bucketStartMs);
        legacyFile(path, schema == 3 ? 2 : schema, {h});
        if (schema == 3) {
            auto b = bytes(path);
            const auto headerLength = u32(b, 6);
            b.resize(headerLength);
            b[4] = 3;
            set32(b, 10, 0);
            set32(b, 10, hmcol::crc32(b.data(), b.size()));
            auto minute = h;
            minute.header.tfMs = 60000;
            auto frame = frameFor(minute); // schema 3 absolute payload
            b.insert(b.end(), frame.begin(), frame.end());
            save(path, b);
        }
        auto out = Hmc2Store::readRange(root(), h.header.symbol, "deep", 3600000, kEpoch, kEpoch + 3600000);
        ASSERT_EQ(out.size(), 1) << schema;
        EXPECT_TRUE(out[0].flags & kApproximateCoverage);
        EXPECT_EQ(out[0].entries[0].coveredMs, h.observedMs);
        Hmc2Reader reader(root());
        ReadControl control;
        size_t count = 0;
        reader.visit(h.header.symbol, "deep", 3600000, kEpoch, kEpoch + 3600000, [&](const auto &r) {
            ++count;
            EXPECT_TRUE(r.flags & kApproximateCoverage);
            EXPECT_EQ(r.entries[0].coveredMs, h.observedMs);
        }, control);
        EXPECT_EQ(count, 1);
    }
}
TEST_F(StoreTest, HourSchemaRejectsMissingOrInconsistentCoverage) {
    Hmc2Store store(root());
    auto r = record();
    r.header.tfMs = 3600000;
    EXPECT_THROW(store.append(r), std::runtime_error);
    for (auto &e : r.entries) e.coveredMs = 10000;
    r.coverage = {{0, 100, false, 10000}, {0, 100, true, 20000}};
    EXPECT_THROW(store.append(r), std::runtime_error);
    r.entries[1].coveredMs = 20000;
    EXPECT_NO_THROW(store.append(r));
    auto out = Hmc2Store::readRange(root(), "BTC-USD", "deep", 3600000, kEpoch, kEpoch + 3600000);
    ASSERT_EQ(out.size(), 1);
    EXPECT_FALSE(out[0].flags & kApproximateCoverage);
    EXPECT_EQ(out[0].coverage.size(), 2);
    EXPECT_EQ(out[0].entries[1].coveredMs, 20000);
    EXPECT_EQ(bytes(Hmc2Store::filePath(root(), r.header, r.bucketStartMs))[4], 4);
}
TEST_F(StoreTest, StreamingSkipsCorruptDeltaChainAndFallsBackToEarlierGeneration) {
    auto r = record();
    {
        Hmc2Store store(root());
        for (int i = 0; i < 20; ++i) {
            r.bucketStartMs = kEpoch + i * 60000;
            store.append(r);
        }
    }
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    auto b = bytes(path);
    auto offset = u32(b, 6);
    offset += 16 + u32(b, offset + 4); // corrupt minute 1; dependent minutes 2..14 must drop
    b[offset + 16] ^= 1;
    save(path, b);
    auto reference = read();
    Hmc2Reader reader(root());
    ReadControl control;
    std::vector<int64_t> times;
    reader.visit("BTC-USD", "deep", 60000, kEpoch, kEpoch + 20 * 60000,
                 [&](const auto &r) { times.push_back(r.bucketStartMs); }, control);
    ASSERT_EQ(times.size(), reference.size());
    for (size_t n = 0; n < times.size(); ++n)
        EXPECT_EQ(times[n], reference[n].bucketStartMs);
    // A malformed complete replacement cannot supersede the valid original.
    auto newer = record();
    newer.header.configHash++;
    {
        Hmc2Store store(root());
        store.append(newer);
    }
    auto replacement = Hmc2Store::filePath(root(), newer.header, kEpoch, 1);
    b = bytes(replacement);
    b.back() ^= 1;
    save(replacement, b);
    ReadControl fresh;
    times.clear();
    reader.visit("BTC-USD", "deep", 60000, kEpoch, kEpoch + 60000,
                 [&](const auto &r) { times.push_back(r.bucketStartMs); }, fresh);
    ASSERT_EQ(times.size(), 1);
    EXPECT_EQ(times[0], kEpoch);
}

TEST_F(StoreTest, DeltaEntriesAllReceiveCurrentObservationCoverage) {
    Hmc2Store store(root());
    auto r = record();
    store.append(r);
    r.bucketStartMs += 60000;
    r.observedMs = 12345;
    store.append(r); // unchanged entries inherited from the full-minute base
    auto records = read();
    ASSERT_EQ(records.size(), 2);
    for (const auto &e : records.back().entries) EXPECT_EQ(e.coveredMs, 12345);
    Hmc2Reader reader(root());
    ReadControl control;
    reader.visit("BTC-USD", "deep", 60000, kEpoch + 60000, kEpoch + 120000,
                 [&](const auto &r) { for (const auto &e : r.entries) EXPECT_EQ(e.coveredMs, r.observedMs); }, control);
}
TEST_F(StoreTest, ShrinkBetweenCandidateCollectionAndLoadIsNotAnEmptyInterval) {
    auto a = record(), b = record(60000);
    Hmc2Store store(root()); store.append(a); store.append(b);
    const auto path = Hmc2Store::filePath(root(), a.header, kEpoch);
    const auto original = bytes(path);
    const size_t firstEnd = u32(original, 6) + 16 + u32(original, u32(original, 6) + 4);
    Hmc2Reader reader(root());
    reader.beforeCandidateForTest([&] { std::filesystem::resize_file(path, firstEnd); });
    ReadControl control;
    int seen = 0;
    auto scan = reader.visit("BTC-USD", "deep", 60000, kEpoch + 60000, kEpoch + 120000,
                             [&](const auto &) { ++seen; }, control);
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(scan.status, ReadStatus::IoError);
    EXPECT_EQ(scan.scannedStartMs, scan.scannedEndMs);
    save(path, original);
    Hmc2Reader availabilityReader(root());
    availabilityReader.beforeCandidateForTest([&] { std::filesystem::resize_file(path, u32(original, 6)); });
    ReadControl availableControl;
    auto available = availabilityReader.availability("BTC-USD", "deep", 60000, availableControl);
    EXPECT_EQ(availableControl.status, ReadStatus::IoError);
    EXPECT_FALSE(available.oldestMs);
}
TEST_F(StoreTest, SelectedFrameOpenAndPayloadReadFailuresAreIoErrors) {
    auto r = record(60000);
    Hmc2Store store(root()); store.append(r);
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    const auto original = bytes(path);
    for (bool missing : {false, true}) {
        save(path, original);
        Hmc2Reader reader(root());
        reader.beforeReadForTest([&] {
            if (missing) std::filesystem::remove(path);
            else std::filesystem::resize_file(path, u32(original, 6) + 17);
        });
        ReadControl control;
        int seen = 0;
        auto scan = reader.visit("BTC-USD", "deep", 60000, kEpoch, kEpoch + 120000,
                                 [&](const auto &) { ++seen; }, control);
        EXPECT_EQ(seen, 0);
        EXPECT_EQ(scan.status, ReadStatus::IoError);
        EXPECT_EQ(scan.scannedStartMs, scan.scannedEndMs);
    }
}
TEST_F(StoreTest, SameSizeRewriteAtSameMtimeCannotChangeTheSelectedBucket) {
    auto r = record();
    Hmc2Store store(root()); store.append(r);
    const auto path = Hmc2Store::filePath(root(), r.header, kEpoch);
    const auto original = bytes(path);
    const auto modified = std::filesystem::last_write_time(path);
    auto payload = rawPayload(frames(path).front());
    std::vector<uint8_t> replacement;
    for (int n = 1; n < 256; ++n) {
        const auto time = kEpoch + n * 60000;
        for (size_t j = 0; j < 8; ++j) payload[j] = static_cast<uint8_t>(time >> (8 * j));
        auto frame = frameFromRaw(payload);
        if (frame.size() + u32(original, 6) == original.size()) {
            replacement.assign(original.begin(), original.begin() + u32(original, 6));
            replacement.insert(replacement.end(), frame.begin(), frame.end());
            break;
        }
    }
    ASSERT_EQ(replacement.size(), original.size());
    Hmc2Reader reader(root());
    reader.beforeReadForTest([&] { save(path, replacement); std::filesystem::last_write_time(path, modified); });
    ReadControl control;
    int seen = 0;
    auto scan = reader.visit("BTC-USD", "deep", 60000, kEpoch, kEpoch + 60000,
                             [&](const auto &) { ++seen; }, control);
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(scan.status, ReadStatus::IoError);
    EXPECT_EQ(scan.scannedStartMs, scan.scannedEndMs);
}
TEST_F(StoreTest, IncrementalAppendChecksTailAndResumesPartialDiscovery) {
    Hmc2Store store(root());
    for (int n = 0; n < 20; ++n) store.append(record(n * 60000));
    Hmc2Reader reader(root());
    ReadControl initial;
    reader.availability("BTC-USD", "deep", 60000, initial);
    const auto before = reader.diagnosticsForTest();
    store.append(record(20 * 60000));
    ReadControl appended;
    auto available = reader.availability("BTC-USD", "deep", 60000, appended);
    EXPECT_EQ(available.latestMs, kEpoch + 20 * 60000);
    EXPECT_EQ(reader.diagnosticsForTest().indexedFrames, before.indexedFrames + 1);
    EXPECT_EQ(reader.diagnosticsForTest().directoryListings, before.directoryListings);
    const auto path = Hmc2Store::filePath(root(), record().header, kEpoch);
    auto raw = bytes(path); raw.back() ^= 1; save(path, raw);
    store.append(record(21 * 60000));
    ReadControl changed;
    reader.availability("BTC-USD", "deep", 60000, changed);
    EXPECT_GT(reader.diagnosticsForTest().indexedFrames, before.indexedFrames + 2);
    Hmc2Reader tiny(root()); // not "small": a macro in the Windows SDK (rpcndr.h)
    size_t seen = 0;
    for (int attempt = 0; attempt < 30 && !seen; ++attempt) {
        ReadControl c;
        c.limits.maxSourceRecords = 1;
        c.limits.maxEntriesVisited = 1;
        tiny.visit("BTC-USD", "deep", 60000, kEpoch + 14 * 60000, kEpoch + 15 * 60000,
                    [&](const auto &) { ++seen; }, c);
    }
    EXPECT_EQ(seen, 1); // neither day discovery nor a delta chain can livelock
}
TEST_F(StoreTest, IndexEvictionIsLruAndListingIsReused) {
    Hmc2Store store(root());
    for (int n = 0; n < 65; ++n) store.append(record(n * 86400000LL));
    Hmc2Reader reader(root());
    auto visit = [&](int day) {
        ReadControl c;
        reader.visit("BTC-USD", "deep", 60000, kEpoch + day * 86400000LL, kEpoch + day * 86400000LL + 60000,
                     [](const auto &) {}, c);
    };
    for (int n = 0; n < 64; ++n) visit(n);
    visit(0); // smallest path is hot, day 1 is now LRU
    visit(64);
    const auto before = reader.diagnosticsForTest();
    visit(0);
    EXPECT_EQ(reader.diagnosticsForTest().indexedFrames, before.indexedFrames);
    visit(1);
    EXPECT_EQ(reader.diagnosticsForTest().indexedFrames, before.indexedFrames + 1);
    EXPECT_EQ(reader.diagnosticsForTest().directoryListings, 1);
}
TEST_F(StoreTest, AvailabilitySkipsDeniedAndCorruptEdgeFiles) {
#ifndef _WIN32
    Hmc2Store store(root());
    for (int n = 0; n < 3; ++n) store.append(record(n * 86400000LL));
    const auto first = Hmc2Store::filePath(root(), record().header, kEpoch);
    const auto last = Hmc2Store::filePath(root(), record().header, kEpoch + 2 * 86400000LL);
    std::filesystem::permissions(first, std::filesystem::perms::none);
    std::ifstream denied(first);
    if (denied.is_open()) {
        std::filesystem::permissions(first, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "Process bypasses permissions";
    }
    save(last, {'b', 'a', 'd'});
    Hmc2Reader reader(root());
    ReadControl c;
    auto available = reader.availability("BTC-USD", "deep", 60000, c);
    EXPECT_EQ(c.status, ReadStatus::Complete);
    EXPECT_EQ(available.oldestMs, kEpoch + 86400000LL);
    EXPECT_EQ(available.latestMs, kEpoch + 86400000LL);
    std::filesystem::permissions(first, std::filesystem::perms::owner_all);
    ReadControl recovered;
    available = reader.availability("BTC-USD", "deep", 60000, recovered);
    EXPECT_EQ(available.oldestMs, kEpoch); // skipped discovery is not cached permanently
#else
    GTEST_SKIP() << "POSIX permission fixture";
#endif
}

#include "Hmc2LegacyFixture.hpp"
TEST_F(StoreTest, SchemaThreeHourThenSchemaFourAppendKeepsBothGenerations) {
    auto old = record(); old.header.tfMs = 3600000;
    const auto path = Hmc2Store::filePath(root(), old.header, kEpoch);
    recording_fixture::schema3Hour(path, old);
    const auto original = bytes(path);
    auto newer = old; newer.bucketStartMs += 3600000;
    for (auto &e : newer.entries) e.coveredMs = newer.observedMs;
    newer.coverage = {{0, 100, false, newer.observedMs}, {0, 100, true, newer.observedMs}};
    { Hmc2Store store(root()); store.append(newer); }
    EXPECT_EQ(bytes(path), original);
    EXPECT_EQ(bytes(Hmc2Store::filePath(root(), old.header, kEpoch, 1))[4], 4);
    auto records = Hmc2Store::readRange(root(), "BTC-USD", "deep", 3600000, kEpoch, kEpoch + 7200000);
    ASSERT_EQ(records.size(), 2);
    EXPECT_TRUE(records[0].flags & kApproximateCoverage);
    EXPECT_FALSE(records[1].flags & kApproximateCoverage);
    Hmc2Reader reader(root()); ReadControl c; size_t count = 0;
    reader.visit("BTC-USD", "deep", 3600000, kEpoch, kEpoch + 7200000, [&](const auto &r) {
        EXPECT_EQ(r.bucketStartMs, records[count++].bucketStartMs);
    }, c);
    EXPECT_EQ(count, 2);
}

TEST_F(StoreTest, ReaderAssertsWorkerOwnershipInDebugBuilds) {
#ifndef NDEBUG
    Hmc2Reader reader(root());
    EXPECT_DEATH({ std::thread other([&] { reader.diagnosticsForTest(); }); other.join(); }, "owner");
#else
    GTEST_SKIP() << "Ownership assertion is debug-only";
#endif
}
