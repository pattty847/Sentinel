#include "LevelTestSupport.hpp"
#include "LevelLogReplayOutput.hpp"
#include <algorithm>
#include "levelstore/LevelLog.hpp"
#include "Crc32.hpp"
#include <gtest/gtest.h>
#include <array>
#include <fstream>
#include <numeric>
#include <map>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace levelstore_test;
namespace {
constexpr int64_t hour = 3'600'000, day = 86'400'000;
DayBinding binding(int64_t floor = 0) { return {"BTC-USD", floor, floor + day, 2}; }
BookImage image(int64_t floor = 0, int n = 0) {
    BookImage i{floor, true, {}};
    for (int j = 0; j < n; ++j)
        i.levels.push_back({true, j + 1, (j + 1) * 125});
    return i;
}
MinuteBlock block(int64_t minute, bool empty = false) {
    MinuteBlock b;
    b.bucketStartMs = minute;
    if (!empty) {
        b.observedAtStart = true;
        b.messages = {{minute, true, false, ClearCause::None},
                      {minute + 1, false, false, ClearCause::Invalid},
                      {minute + 2, false, false, ClearCause::Invalid},
                      {minute + 3, true, true, ClearCause::None}};
        b.changes = {{0, true, 100, 1250}, {3, false, 101, 3500}};
    }
    return b;
}
std::vector<uint8_t> canonical(const LevelLogRecord &r) {
    ByteWriter w;
    if (const auto *i = std::get_if<BookImage>(&r.value)) encodeImage(w, *i);
    else encodeBlock(w, std::get<MinuteBlock>(r.value));
    return w.bytes;
}
std::vector<LevelLogRecord> records(const fs::path &root) {
    std::vector<LevelLogRecord> v;
    LevelLogReader(root, "BTC-USD").read([&](const auto &r) { v.push_back(r); });
    return v;
}
std::vector<uint8_t> bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void put(const fs::path &p, const std::vector<uint8_t> &v) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size()));
    ASSERT_TRUE(f.good());
}
struct Inject : LevelLogFiles {
    std::vector<std::string> calls;
    size_t failAt = SIZE_MAX;
    bool shortWrite = false;
    std::function<void(const std::string &)> after;
    bool point(const std::string &op) {
        calls.push_back(op);
        if (calls.size() - 1 != failAt) return false;
        if (shortWrite && op == "write") return true;
        throw std::runtime_error("injected " + op);
    }
    void done(const std::string &op) { if (after) after(op); }
    void createDirectory(const fs::path &p) override {
        point("create"); LevelLogFiles::createDirectory(p); done("create");
    }
    std::FILE *openAppend(const fs::path &p) override {
        point("open"); auto *f = LevelLogFiles::openAppend(p); done("open"); return f;
    }
    size_t write(std::FILE *f, std::span<const uint8_t> v) override {
        if (point("write")) {
            const auto n = LevelLogFiles::write(f, v.first(v.size() / 2));
            done("short");
            return n;
        }
        const auto n = LevelLogFiles::write(f, v); done("write"); return n;
    }
    void close(std::FILE *f) override {
        // fclose consumes the handle even when it reports a failure.
        LevelLogFiles::close(f); point("close"); done("close");
    }
    void sync(const fs::path &p, bool dir) override {
        const auto op = dir ? "dir-sync" : "file-sync";
        point(op); LevelLogFiles::sync(p, dir); done(op);
    }
    void remove(const fs::path &p) override {
        point("remove"); LevelLogFiles::remove(p); done("remove");
    }
    void truncate(const fs::path &p, uint64_t n) override {
        point("truncate"); LevelLogFiles::truncate(p, n); done("truncate");
    }
};
void seed(const fs::path &root) {
    LevelLog w(root, "BTC-USD");
    w.onDayStart(binding(), image());
    w.onBlock(binding(), block(0));
    w.sync();
}
void unchangedPoison(LevelLog &w, Inject &io) {
    ASSERT_TRUE(w.failed());
    const auto n = io.calls.size();
    EXPECT_THROW(w.open(), std::runtime_error);
    EXPECT_THROW(w.durableTail(), std::runtime_error);
    EXPECT_THROW(w.onDayStart(binding(day), image(day)), std::runtime_error);
    EXPECT_THROW(w.onBlock(binding(), block(120'000)), std::runtime_error);
    EXPECT_THROW(w.sync(), std::runtime_error);
    EXPECT_THROW(w.truncateAfter(0), std::runtime_error);
    EXPECT_EQ(n, io.calls.size());
}
void assertPrefix(const fs::path &root, const std::vector<LevelLogRecord> &expected, size_t required) {
    LevelLog reopen(root, "BTC-USD");
    ASSERT_NO_THROW(reopen.open());
    const auto v = records(root);
    ASSERT_GE(v.size(), required);
    ASSERT_LE(v.size(), expected.size());
    for (size_t i = 0; i < v.size(); ++i) {
        EXPECT_EQ(v[i].binding, expected[i].binding);
        EXPECT_EQ(canonical(v[i]), canonical(expected[i]));
    }
}
} // namespace

TEST(LevelLog, RoundTripOpenHourAndLargeBook) {
    TempDir t;
    LevelLog w(t.path, "BTC-USD");
    std::vector<LevelLogRecord> expected{{binding(), image(0, 100'000)}};
    w.onDayStart(binding(), std::get<BookImage>(expected[0].value));
    for (const auto m : std::array<int64_t, 4>{0, 60'000, hour, day - 60'000}) {
        auto b = block(m, m == 60'000);
        w.onBlock(binding(), b);
        expected.push_back({binding(), b});
        w.sync();
        ASSERT_EQ(records(t.path).size(), expected.size());
    }
    w.onDayStart(binding(day), image(day));
    expected.push_back({binding(day), image(day)});
    w.sync();
    assertPrefix(t.path, expected, expected.size());
    const auto first = LevelLogReader(t.path, "BTC-USD").imageForHour(23 * hour);
    ASSERT_TRUE(first);
    EXPECT_EQ(canonical(*first), canonical(expected[0]));
}

TEST(LevelLog, EveryAppendAndSyncFailurePoisonsAndPreservesSyncedPrefix) {
    std::vector<std::string> points;
    {
        TempDir t; seed(t.path);
        auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io);
        w.open(); io->calls.clear();
        w.onBlock(binding(), block(hour)); w.sync(); points = io->calls;
        EXPECT_EQ(points, (std::vector<std::string>{"open", "write", "write", "write", "close", "file-sync", "dir-sync"}));
    }
    for (const int64_t nextMinute : {int64_t(60'000), hour}) {
        const std::vector<LevelLogRecord> expected{{binding(), image()}, {binding(), block(0)}, {binding(), block(nextMinute)}};
        for (size_t at = 0; at < points.size(); ++at) {
            for (bool shortWrite : {false, true}) {
                if (shortWrite && points[at] != "write") continue;
                SCOPED_TRACE("point=" + std::to_string(at) + " op=" + points[at] + " short=" + std::to_string(shortWrite) + " minute=" + std::to_string(nextMinute));
                TempDir t; seed(t.path);
                auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io);
                w.open(); io->calls.clear(); io->failAt = at; io->shortWrite = shortWrite;
                EXPECT_THROW({ w.onBlock(binding(), block(nextMinute)); w.sync(); }, std::runtime_error);
                unchangedPoison(w, *io);
                assertPrefix(t.path, expected, 2);
            }
        }
    }
    RecordProperty("operation_points", points.size());
}

TEST(LevelLog, EveryCreateAndOpenRecoveryFailurePoisons) {
    for (bool existing : {false, true}) {
        std::vector<std::string> points;
        auto setup = [&](const fs::path &root) {
            if (!existing) return;
            seed(root);
            const auto p = LevelLogReader::hourPath(root, "BTC-USD", 0);
            std::ofstream f(p, std::ios::binary | std::ios::app); f << "LC";
        };
        {
            TempDir t; const auto root = t.path / "new" / "root"; setup(root);
            auto io = std::make_shared<Inject>(); LevelLog w(root, "BTC-USD", io);
            w.open(); points = io->calls;
        }
        for (size_t at = 0; at < points.size(); ++at) {
            SCOPED_TRACE("existing=" + std::to_string(existing) + " point=" + std::to_string(at) + " op=" + points[at]);
            TempDir t; const auto root = t.path / "new" / "root"; setup(root);
            auto io = std::make_shared<Inject>(); io->failAt = at;
            LevelLog w(root, "BTC-USD", io);
            EXPECT_THROW(w.open(), std::runtime_error);
            unchangedPoison(w, *io);
            assertPrefix(root, {{binding(), image()}, {binding(), block(0)}}, existing ? 2 : 0);
        }
    }
}

TEST(LevelLog, CorruptionNeverTruncatesLaterData) {
    TempDir t; seed(t.path);
    { LevelLog w(t.path, "BTC-USD"); w.onBlock(binding(), block(60'000)); w.sync(); }
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    const auto original = bytes(p);
    ByteReader firstHeader(std::span<const uint8_t>(original).subspan(8, 4));
    const auto middle = 12 + firstHeader.fixed32() + 4;
    for (int mutation : {0, 1, 2, 3}) {
        auto damaged = original;
        if (mutation == 0) damaged[middle + 15] ^= 0x40; // middle frame payload, with a valid frame after it
        if (mutation == 1) { // believable oversized length concealing later valid frames
            damaged[8] = 0; damaged[9] = 0; damaged[10] = 1; damaged[11] = 0;
        }
        if (mutation == 2) damaged.back() ^= 0x01; // a complete bad terminal CRC is not a tear
        if (mutation == 3) damaged[0] = 'X';
        put(p, damaged);
        LevelLog w(t.path, "BTC-USD");
        try { w.open(); FAIL() << "corruption accepted"; }
        catch (const std::runtime_error &e) {
            EXPECT_NE(std::string(e.what()).find(p.string()), std::string::npos);
            EXPECT_NE(std::string(e.what()).find("offset="), std::string::npos);
        }
        EXPECT_TRUE(w.failed());
        EXPECT_EQ(bytes(p), damaged);
        EXPECT_THROW(records(t.path), std::runtime_error);
    }
}

TEST(LevelLog, EveryTornTerminalByteRecoversAndRemainsAppendable) {
    TempDir source; seed(source.path);
    const auto p = LevelLogReader::hourPath(source.path, "BTC-USD", 0);
    const auto prefix = bytes(p);
    { LevelLog w(source.path, "BTC-USD"); w.onBlock(binding(), block(60'000)); w.sync(); }
    const auto complete = bytes(p);
    for (size_t length = prefix.size() + 1; length < complete.size(); ++length) {
        SCOPED_TRACE(length);
        TempDir t; fs::create_directories(t.path / "BTC-USD");
        const auto target = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
        put(target, {complete.begin(), complete.begin() + length});
        EXPECT_EQ(records(t.path).size(), 2u); // read-only reader withholds terminal partial
        EXPECT_EQ(fs::file_size(target), length);
        LevelLog w(t.path, "BTC-USD");
        ASSERT_NO_THROW(w.open());
        EXPECT_EQ(bytes(target), prefix);
        w.onBlock(binding(), block(60'000)); w.sync();
        EXPECT_EQ(bytes(target), complete);
    }
    RecordProperty("torn_offsets", complete.size() - prefix.size() - 1);
}

TEST(LevelLog, TornEarlierHourWithLaterDataFailsClosed) {
    TempDir t; seed(t.path);
    { LevelLog w(t.path, "BTC-USD"); w.onBlock(binding(), block(hour)); w.sync(); }
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    auto damaged = bytes(p); damaged.push_back('L'); put(p, damaged);
    LevelLog w(t.path, "BTC-USD");
    EXPECT_THROW(w.open(), std::runtime_error);
    EXPECT_EQ(bytes(p), damaged);
}

TEST(LevelLog, CutWithinHourAcrossHoursAndAtDayStartThenAppend) {
    for (int64_t cut : std::array<int64_t, 4>{60'000, hour, day, 0}) {
        TempDir t; seed(t.path);
        LevelLog w(t.path, "BTC-USD");
        w.onBlock(binding(), block(60'000));
        w.onBlock(binding(), block(hour));
        w.onDayStart(binding(day), image(day));
        w.onBlock(binding(day), block(day)); w.sync();
        w.truncateAfter(cut);
        for (const auto &r : records(t.path)) EXPECT_LT(r.minute(), cut);
        const auto b = binding(cut == day ? day : 0);
        if (cut == 0 || cut == day) w.onDayStart(b, image(cut));
        w.onBlock(b, block(cut)); w.sync();
        const auto v = records(t.path);
        ASSERT_FALSE(v.empty());
        EXPECT_EQ(v.back().minute(), cut);
        LevelLog reopen(t.path, "BTC-USD"); EXPECT_NO_THROW(reopen.open());
    }
}

TEST(LevelLog, EveryCutFailurePoisonsAndLeavesPrefix) {
    const std::vector<LevelLogRecord> expected{{binding(), image()}, {binding(), block(0)},
        {binding(), block(60'000)}, {binding(), block(hour)}, {binding(day), image(day)}, {binding(day), block(day)}};
    auto setup = [&](const fs::path &p) {
        seed(p); LevelLog w(p, "BTC-USD");
        w.onBlock(binding(), block(60'000));
        w.onBlock(binding(), block(hour)); w.onDayStart(binding(day), image(day));
        w.onBlock(binding(day), block(day)); w.sync();
    };
    std::vector<std::string> points;
    {
        TempDir t; setup(t.path);
        auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io); w.open();
        io->calls.clear(); w.truncateAfter(60'000); points = io->calls;
        EXPECT_EQ(points, (std::vector<std::string>{"remove", "dir-sync", "remove", "dir-sync", "truncate", "file-sync", "file-sync", "dir-sync"}));
    }
    for (size_t at = 0; at < points.size(); ++at) {
        SCOPED_TRACE("point=" + std::to_string(at) + " op=" + points[at]);
        TempDir t; setup(t.path);
        auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io); w.open();
        io->calls.clear(); io->failAt = at;
        EXPECT_THROW(w.truncateAfter(60'000), std::runtime_error);
        unchangedPoison(w, *io);
        assertPrefix(t.path, expected, 2);
        LevelLog finish(t.path, "BTC-USD"); finish.truncateAfter(60'000);
        EXPECT_EQ(records(t.path).size(), 2u);
    }
}

#ifndef _WIN32
TEST(LevelLog, SeededHardKillsAtOperationsAndMidFrame) {
    std::mt19937 rng(0xE1B2A);
    constexpr int kills = 32;
    for (int attempt = 0; attempt < kills; ++attempt) {
        SCOPED_TRACE(attempt);
        TempDir t; seed(t.path);
        const int64_t nextMinute = attempt % 2 == 0 ? 60'000 : hour;
        const bool midFrame = attempt < 16;
        const int point = midFrame ? int(rng() % 3) : (attempt - 16) % 8;
        int ready[2]; ASSERT_EQ(pipe(ready), 0);
        const auto pid = fork(); ASSERT_GE(pid, 0);
        if (pid == 0) {
            close(ready[0]);
            auto stop = [&] {
                const char c = 'K'; (void)::write(ready[1], &c, 1);
                for (;;) pause();
            };
            struct KillFiles : LevelLogFiles {
                int target = 0, seen = 0;
                bool middle = false, active = false;
                std::function<void()> stop;
                void hit() { if (active && seen++ == target) stop(); }
                std::FILE *openAppend(const fs::path &p) override {
                    auto *f = LevelLogFiles::openAppend(p); if (!middle) hit(); return f;
                }
                size_t write(std::FILE *f, std::span<const uint8_t> v) override {
                    if (active && middle && seen++ == target) {
                        LevelLogFiles::write(f, v.first(v.size() / 2)); stop();
                    }
                    auto n = LevelLogFiles::write(f, v); if (!middle) hit(); return n;
                }
                void close(std::FILE *f) override { LevelLogFiles::close(f); if (!middle) hit(); }
                void sync(const fs::path &p, bool dir) override { LevelLogFiles::sync(p, dir); if (!middle) hit(); }
            };
            auto io = std::make_shared<KillFiles>(); io->middle = midFrame; io->target = point; io->stop = stop;
            try {
                LevelLog w(t.path, "BTC-USD", io); w.open(); io->active = true;
                w.onBlock(binding(), block(nextMinute)); w.sync();
                stop(); // after successful durability fence
            } catch (...) { _exit(3); }
            _exit(4);
        }
        close(ready[1]);
        char c = 0;
        ASSERT_EQ(::read(ready[0], &c, 1), 1);
        close(ready[0]);
        ASSERT_EQ(kill(pid, SIGKILL), 0);
        int status = 0; ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFSIGNALED(status)); EXPECT_EQ(WTERMSIG(status), SIGKILL);
        const size_t required = !midFrame && point >= 6 ? 3 : 2;
        assertPrefix(t.path, {{binding(), image()}, {binding(), block(0)}, {binding(), block(nextMinute)}}, required);
    }
    RecordProperty("kill_points", kills);
}
#endif

TEST(LevelLog, EveryImageWriteFailurePoisonsWithoutPartialRecord) {
    for (size_t at = 0; at < 7; ++at) {
        for (bool shortWrite : {false, true}) {
            if (shortWrite && (at < 1 || at > 3)) continue;
            SCOPED_TRACE("point=" + std::to_string(at) + " short=" + std::to_string(shortWrite));
            TempDir t;
            auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io); w.open();
            io->calls.clear(); io->failAt = at; io->shortWrite = shortWrite;
            EXPECT_THROW({ w.onDayStart(binding(), image(0, 1000)); w.sync(); }, std::runtime_error);
            unchangedPoison(w, *io);
            assertPrefix(t.path, {{binding(), image(0, 1000)}}, 0);
        }
    }
}

TEST(LevelLog, ValidChecksumCannotHideInvalidCanonicalPayload) {
    TempDir t; seed(t.path);
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    auto b = bytes(p);
    ByteReader h(std::span<const uint8_t>(b).subspan(8, 4));
    const auto n = h.fixed32();
    ByteReader payload(std::span<const uint8_t>(b).subspan(12, n));
    (void)payload.str(256); (void)payload.svarint(); (void)payload.svarint(); (void)payload.svarint();
    (void)payload.fixed32();
    (void)payload.fixed32(); // image decimal scales
    (void)payload.svarint(); // image timestamp
    b[12 + n - payload.remaining()] = 2; // invalid image observed flag
    const auto crc = sentinel::crc32(b.data(), 12 + n);
    for (size_t i = 0; i < 4; ++i) b[12 + n + i] = static_cast<uint8_t>(crc >> (8 * i));
    put(p, b);
    LevelLog w(t.path, "BTC-USD"); EXPECT_THROW(w.open(), std::runtime_error);
    EXPECT_EQ(bytes(p), b);
}

TEST(LevelLog, InvalidOrderingAndForeignBindingsPoisonBeforeWriting) {
    for (int mutation : {0, 1, 2, 3}) {
        TempDir t; seed(t.path);
        const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
        const auto original = bytes(p);
        LevelLog w(t.path, "BTC-USD");
        auto b = binding();
        if (mutation == 1) b.symbol = "ETH-USD";
        if (mutation == 2) b.priceDigits = 3;
        if (mutation == 3) {
            EXPECT_THROW(w.onDayStart(b, image()), std::runtime_error);
        } else {
            EXPECT_THROW(w.onBlock(b, block(mutation == 0 ? 0 : 60'000)), std::runtime_error);
        }
        EXPECT_TRUE(w.failed()); EXPECT_EQ(bytes(p), original);
    }
}

namespace {
using FileTree = std::map<std::string, std::vector<uint8_t>>;
FileTree tree(const fs::path &root) {
    FileTree result;
    if (!fs::exists(root / "BTC-USD")) return result;
    for (const auto &e : fs::directory_iterator(root / "BTC-USD"))
        if (e.path().extension() == ".lcl") result.emplace(e.path().filename().string(), bytes(e.path()));
    return result;
}
void powerLossTail(const fs::path &root, int shape) {
    seed(root);
    const auto p = LevelLogReader::hourPath(root, "BTC-USD", 0);
    auto prefix = bytes(p);
    if (shape == 1) {
        { LevelLog w(root, "BTC-USD"); w.onBlock(binding(), block(60'000)); }
        auto damaged = bytes(p);
        std::fill(damaged.begin() + prefix.size() + 12, damaged.end() - 4, uint8_t(0));
        put(p, damaged);
    } else if (shape == 2) {
        prefix.resize(prefix.size() + 4096, 0);
        put(p, prefix);
    } else {
        { LevelLog w(root, "BTC-USD"); w.onBlock(binding(), block(hour)); w.sync(); }
        prefix.insert(prefix.end(), {'L', 'C', 'L', 'G', 1, 2, 0, 0, 9, 0, 0, 0, 1});
        put(p, prefix);
    }
}
void checkAnchorRecovery(int shape) {
    TempDir t, expected;
    powerLossTail(t.path, shape);
    seed(expected.path);
    // Strict process-crash recovery remains fail-closed for these shapes.
    LevelLog strict(t.path, "BTC-USD");
    EXPECT_THROW(strict.open(), std::runtime_error);
    EXPECT_THROW(strict.truncateAfter(60'000), std::runtime_error);
    LevelLog restart(t.path, "BTC-USD");
    ASSERT_NO_THROW(restart.truncateAfter(60'000)); // no strict open first
    EXPECT_EQ(tree(t.path), tree(expected.path));
    restart.onBlock(binding(), block(60'000)); restart.sync();
    { LevelLog ref(expected.path, "BTC-USD"); ref.onBlock(binding(), block(60'000)); ref.sync(); }
    EXPECT_EQ(tree(t.path), tree(expected.path));
}
} // namespace

TEST(LevelLog, AnchorCutDiscardsD1ZeroedCompletePayload) { checkAnchorRecovery(1); }
TEST(LevelLog, AnchorCutDiscardsD2ZeroExtent) { checkAnchorRecovery(2); }
TEST(LevelLog, AnchorCutDiscardsD4TornEarlierHourAndLaterData) { checkAnchorRecovery(4); }

TEST(LevelLog, AnchorCutRejectsDamageBeforeBoundaryWithoutChangingAnyFile) {
    for (int shape : {1, 2, 4}) {
        SCOPED_TRACE(shape);
        TempDir t; powerLossTail(t.path, shape);
        const auto before = tree(t.path);
        LevelLog w(t.path, "BTC-USD");
        try { w.truncateAfter(120'000); FAIL() << "damaged retained minute accepted"; }
        catch (const std::runtime_error &e) {
            EXPECT_NE(std::string(e.what()).find("path="), std::string::npos);
            EXPECT_NE(std::string(e.what()).find("offset="), std::string::npos);
        }
        EXPECT_TRUE(w.failed()); EXPECT_EQ(tree(t.path), before);
    }
}

TEST(LevelLog, CutRemovesEmptyHourFilesAndSyncsDirectory) {
    for (int64_t cut : std::array<int64_t, 3>{60'000, hour, 0}) {
        TempDir t; seed(t.path);
        auto io = std::make_shared<Inject>(); LevelLog w(t.path, "BTC-USD", io);
        w.onBlock(binding(), block(hour)); w.sync(); io->calls.clear();
        w.truncateAfter(cut);
        EXPECT_FALSE(fs::exists(LevelLogReader::hourPath(t.path, "BTC-USD", hour)));
        for (const auto &[name, v] : tree(t.path)) EXPECT_FALSE(v.empty()) << name;
        if (cut == 0) EXPECT_TRUE(tree(t.path).empty());
        const auto removed = std::find(io->calls.begin(), io->calls.end(), "remove");
        ASSERT_NE(removed, io->calls.end());
        ASSERT_NE(removed + 1, io->calls.end()); EXPECT_EQ(*(removed + 1), "dir-sync");
        EXPECT_EQ(io->calls.back(), "dir-sync");
    }
}

TEST(LevelLog, CutAndReplayMatchesUninterruptedFileTree) {
    std::vector<LevelLogRecord> all{{binding(), image(0, 1000)}};
    for (int64_t m = 0; m < 126; ++m) all.push_back({binding(), block(m * 60'000, m % 7 == 0)});
    all.push_back({binding(day), image(day, 1000)});
    all.push_back({binding(day), block(day)});
    all.push_back({binding(day), block(day + 60'000)});
    auto append = [](LevelLog &w, const LevelLogRecord &r) {
        if (const auto *i = std::get_if<BookImage>(&r.value)) w.onDayStart(r.binding, *i);
        else w.onBlock(r.binding, std::get<MinuteBlock>(r.value));
    };
    TempDir reference;
    { LevelLog w(reference.path, "BTC-USD"); for (const auto &r : all) append(w, r); w.sync(); }
    for (int64_t cut : std::array<int64_t, 7>{0, 45 * 60'000, hour, 119 * 60'000, 2 * hour, day, day + 60'000}) {
        SCOPED_TRACE(cut);
        TempDir t, prefix;
        { LevelLog w(t.path, "BTC-USD"); for (const auto &r : all) append(w, r); w.sync(); }
        const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", day);
        auto torn = bytes(p); torn.insert(torn.end(), {'L', 'C'}); put(p, torn);
        LevelLog restart(t.path, "BTC-USD"); restart.truncateAfter(cut);
        { LevelLog w(prefix.path, "BTC-USD"); for (const auto &r : all) if (r.minute() < cut) append(w, r); w.sync(); }
        EXPECT_EQ(tree(t.path), tree(prefix.path)); // includes file names, before replay reaches future hours
        for (const auto &r : all) if (r.minute() >= cut) append(restart, r);
        restart.sync();
        EXPECT_EQ(tree(t.path), tree(reference.path));
    }
}

TEST(LevelLog, DurableTailAdvancesOnlyAtSuccessfulFences) {
    TempDir t; LevelLog w(t.path, "BTC-USD");
    EXPECT_THROW(w.durableTail(), std::runtime_error);
    w.open(); EXPECT_FALSE(w.durableTail());
    w.onDayStart(binding(), image()); EXPECT_FALSE(w.durableTail());
    w.sync(); EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(), 0, true}));
    w.onBlock(binding(), block(0));
    EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(), 0, true}));
    w.sync(); EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(), 0, false}));
    w.onDayStart(binding(day), image(day));
    EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(), 0, false}));
    w.sync(); EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(day), day, true}));
    w.truncateAfter(day); EXPECT_EQ(w.durableTail(), (LevelLogTail{binding(), 0, false}));
    w.truncateAfter(0); EXPECT_FALSE(w.durableTail());
    w.onDayStart(binding(), image()); // not fenced by this instance
    LevelLog reopen(t.path, "BTC-USD"); reopen.open(); // explicitly adopts and syncs inherited complete records
    EXPECT_EQ(reopen.durableTail(), (LevelLogTail{binding(), 0, true}));
}

TEST(LevelLog, AnchorCutImagePolicyMatchesRestoredDayStartedState) {
    for (int64_t floor : {int64_t(0), day, int64_t(30 * 60'000)}) {
        for (const auto policy : {CutImageAtBoundary::Remove, CutImageAtBoundary::Keep}) {
            TempDir t; auto b = binding(floor);
            { LevelLog w(t.path, "BTC-USD"); w.onDayStart(b, image(floor)); w.onBlock(b, block(floor)); w.sync(); }
            // The image-only anchor must not validate a damaged first minute it will replay.
            const auto path = LevelLogReader::hourPath(t.path, "BTC-USD", floor);
            auto damaged = bytes(path); damaged.back() ^= 1; put(path, damaged);
            LevelLog restart(t.path, "BTC-USD"); restart.truncateAfter(floor, policy);
            if (policy == CutImageAtBoundary::Keep) {
                EXPECT_EQ(restart.durableTail(), (LevelLogTail{b, floor, true}));
                ASSERT_EQ(records(t.path).size(), 1u);
            } else {
                EXPECT_FALSE(restart.durableTail()); EXPECT_TRUE(tree(t.path).empty());
                restart.onDayStart(b, image(floor));
            }
            restart.onBlock(b, block(floor)); restart.sync();
            ASSERT_EQ(records(t.path).size(), 2u);
        }
    }
    TempDir t; seed(t.path);
    const auto before = tree(t.path);
    LevelLog w(t.path, "BTC-USD");
    EXPECT_THROW(w.truncateAfter(60'000, CutImageAtBoundary::Keep), std::runtime_error);
    EXPECT_EQ(tree(t.path), before); // a required image may not silently disappear
}

TEST(LevelLog, LogAndRecorderAcceptUnalignedCeiling) {
    for (int64_t ceiling : {1LL, 30'000LL, 60'001LL}) {
        TempDir t; auto b = binding(); b.ceilingMs = ceiling;
        LevelLog w(t.path, "BTC-USD"); LevelRecorder c(b, &w);
        c.onMinute(b.symbol, 0, 0, 0);
        ASSERT_NO_THROW(c.throwIfFailed()); w.sync();
        const auto v = records(t.path); ASSERT_EQ(v.size(), 2u);
        EXPECT_EQ(v[0].binding, b); EXPECT_EQ(v[1].binding, b);
    }
}

TEST(LevelLog, AppleDoubleSidecarsAreIgnoredByReadOpenAndCut) {
    TempDir t; seed(t.path);
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    const auto sidecar = p.parent_path() / ("._" + p.filename().string());
    put(sidecar, {0, 0, 0, 1});
    EXPECT_EQ(records(t.path).size(), 2u);
    LevelLog w(t.path, "BTC-USD"); ASSERT_NO_THROW(w.open());
    w.truncateAfter(0); EXPECT_TRUE(records(t.path).empty());
    EXPECT_EQ(bytes(sidecar), (std::vector<uint8_t>{0, 0, 0, 1}));
}

TEST(LevelLog, ResyncFindsValidatedFrameAcrossChunkBoundary) {
    TempDir t; seed(t.path);
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    const auto prefix = bytes(p);
    { LevelLog w(t.path, "BTC-USD"); w.onBlock(binding(), block(60'000)); w.sync(); }
    const auto complete = bytes(p);
    const std::vector<uint8_t> valid(complete.begin() + prefix.size(), complete.end());
    auto damaged = prefix;
    damaged.insert(damaged.end(), valid.begin(), valid.begin() + 12);
    const uint32_t pastEof = 300'000;
    for (size_t i = 0; i < 4; ++i) damaged[prefix.size() + 8 + i] = uint8_t(pastEof >> (8 * i));
    damaged.resize(prefix.size() + 65'534, 0); // candidate magic straddles the first resync chunk's end
    damaged.insert(damaged.end(), valid.begin(), valid.end());
    put(p, damaged);
    LevelLog w(t.path, "BTC-USD"); EXPECT_THROW(w.open(), std::runtime_error);
    EXPECT_EQ(bytes(p), damaged);
}

TEST(LevelLog, LargeTornImageWithoutResyncCandidateRepairs) {
    TempDir t;
    { LevelLog w(t.path, "BTC-USD"); w.onDayStart(binding(), image(0, 100'000)); w.sync(); }
    const auto p = LevelLogReader::hourPath(t.path, "BTC-USD", 0);
    auto torn = bytes(p); ASSERT_GT(torn.size(), 65'536u); torn.pop_back(); put(p, torn);
    LevelLog w(t.path, "BTC-USD"); ASSERT_NO_THROW(w.open());
    EXPECT_FALSE(w.durableTail()); EXPECT_TRUE(records(t.path).empty());
    w.onDayStart(binding(), image()); w.sync(); EXPECT_EQ(records(t.path).size(), 1u);
}

TEST(LevelLog, ReplayMeasuresEveryDayImage) {
    TempDir t;
    LogReplayOutput output(t.path, "BTC-USD");
    output.onDayStart(binding(), image());
    output.onDayStart(binding(day), image(day));
    ASSERT_EQ(output.imageCosts.size(), 2u);
    EXPECT_EQ(output.imageCosts[0]["atMs"], 0);
    EXPECT_EQ(output.imageCosts[1]["atMs"], day);
    for (const auto &cost : output.imageCosts) {
        EXPECT_EQ(cost["symbol"], "BTC-USD");
        EXPECT_GE(cost["appendSyncMs"].get<double>(), 0);
    }
}
