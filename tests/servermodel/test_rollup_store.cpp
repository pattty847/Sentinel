// Stored 1h/1d rollup levels (RollupStore): the oracle against composed
// minutes, the builder's file rules, and restart/rollover/deletion/corruption.
#include "heatmap/BinCell.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "heatmap/HeatmapTiles.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/ChunkService.hpp"
#include "servermodel/RollupStore.hpp"
#include "roller/Roller.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>

namespace recording {
struct RollupStoreTestAccess {
    static bool logDue(RollupStore& store, const std::string& symbol, int64_t day, uint64_t& repeats) {
        return store.fallbackLogDue(symbol, "near", day, repeats);
    }
    static size_t logCount(const RollupStore& store) { return store.fallbackLogs_.size(); }
};
} // namespace recording

namespace {
using namespace recording;
using namespace heatmap;
namespace fs = std::filesystem;
constexpr int64_t minute = kMinuteMs, hour = kHourMs, day = kDayMs;
// A UTC day start 13 days into the first 1d chunk (so day 3 starts the next one).
constexpr int64_t epoch = kHmc2MinMs + 13 * day, day0 = epoch;

uint32_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return uint32_t(x);
}
// A minute with both sides, zero rows inside coverage, uneven coverage edges,
// partial observation and (deep, from day 1 noon) a second native grid.
Hmc2Record record(const std::string& layer, int64_t bucket) {
    const uint64_t m = uint64_t((bucket - epoch) / minute);
    const bool coarse = layer == "deep" && bucket >= day0 + day + 12 * hour;
    const int tick = coarse ? 2 : 1;
    Hmc2Record r;
    r.header = {"BTC-USD", layer, minute, 100., 100 * tick, {}, uint64_t(layer == "near" ? 7 : 11 + tick)};
    r.bucketStartMs = bucket;
    r.observedMs = mix(m) % 5 ? 60000 : 20000 + mix(m * 3) % 40000;
    r.flags = r.observedMs < 60000 ? kPartial : 0;
    const int64_t base = (layer == "near" ? 1000 : 900) + int64_t(mix(m * 5) % 40) - 20;
    r.bidRowLo = (base - 30 - int64_t(mix(m * 7) % 6)) / tick;
    r.bidRowHi = base / tick;
    r.askRowLo = base / tick + 1;
    r.askRowHi = (base + 30 + int64_t(mix(m * 11) % 6)) / tick;
    for (int64_t row = r.bidRowLo; row <= r.askRowHi; ++row) {
        const uint32_t h = mix(m * 131 + uint64_t(row));
        if (h % 3 == 0) continue; // a covered zero row
        const double size = 0.01 + double(h % 100000) / 997.0;
        r.entries.push_back({row, row > r.bidRowHi, encodeSize(size), 0, r.observedMs});
    }
    return r;
}
// Day 0 starts at 13:13 (first minute mid-day); days 1-3 have gaps (missing
// minutes and whole hours); day 4 is the open day.
bool recorded(int64_t bucket) {
    const int64_t t = bucket - day0;
    if (t < 13 * hour + 13 * minute) return false;
    const int64_t hourOfDay = (t % day) / hour;
    if (t / day == 1 && (hourOfDay == 3 || hourOfDay == 4)) return false;
    if (t / day == 2 && hourOfDay == 20) return false;
    return mix(uint64_t(t / minute) * 17) % 11 != 0;
}
constexpr int64_t kRecordedEnd = day0 + 4 * day + 2 * hour;

class RollupTest : public testing::Test {
protected:
    // The recorded minutes are written once and copied into each test's tree.
    static inline std::unique_ptr<QTemporaryDir> fixture;
    static void SetUpTestSuite() {
        fixture = std::make_unique<QTemporaryDir>();
        Hmc2Store store(fs::path(fixture->path().toStdString()) / "hmc2");
        // Sparse in hours to keep the fixture small: a few hours per day.
        for (int64_t t = day0; t < kRecordedEnd; t += minute) {
            const int64_t hourOfDay = ((t - day0) % day) / hour;
            if (hourOfDay % 5 != 0 && hourOfDay != 13 && hourOfDay != 14 && hourOfDay != 23) continue;
            if (!recorded(t)) continue;
            store.append(record("near", t));
            store.append(record("deep", t));
        }
        // The same deep day 1 with one more recorded hour (07:00): a closed
        // day whose minute files a later backfill rewrites with other cells.
        Hmc2Store changed(fs::path(fixture->path().toStdString()) / "changed");
        for (int64_t t = day0 + day; t < day0 + 2 * day; t += minute) {
            const int64_t hourOfDay = ((t - day0) % day) / hour;
            if (hourOfDay % 5 != 0 && hourOfDay != 13 && hourOfDay != 14 && hourOfDay != 23 && hourOfDay != 7) continue;
            if (hourOfDay != 7 && !recorded(t)) continue;
            changed.append(record("deep", t));
        }
    }
    static fs::path changedTree() { return fs::path(fixture->path().toStdString()) / "changed"; }
    static void TearDownTestSuite() { fixture.reset(); }
    QTemporaryDir temp;
    fs::path hmc2() const { return fs::path(temp.path().toStdString()) / "hmc2"; }
    fs::path rollupRoot() const { return fs::path(temp.path().toStdString()) / "hmc2-rollups"; }
    int64_t through = kRecordedEnd;
    void SetUp() override {
        const fs::path source = fs::path(fixture->path().toStdString()) / "hmc2";
        fs::create_directories(hmc2());
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            const auto target = hmc2() / fs::relative(entry.path(), source);
            if (entry.is_directory()) fs::create_directories(target);
            else if (entry.path().extension() == ".hmc2") fs::copy_file(entry.path(), target);
        }
    }
    RollupStore::Options options() {
        RollupStore::Options o;
        o.hmc2Root = hmc2();
        o.root = rollupRoot();
        o.symbols = {"BTC-USD"};
        o.minuteThrough = [this](const std::string&, const std::string&) { return through; };
        return o;
    }
    std::vector<uint8_t> bytes(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

std::shared_ptr<const StoredChunk> stored(SparseColumns columns, uint64_t generation) {
    auto chunk = std::make_shared<StoredChunk>();
    chunk->key = {columns.symbol, std::string(chunkSourceForHmc2Layer(columns.layer)->id), columns.tfMs,
                  columns.startMs};
    chunk->columns = std::make_shared<SparseColumns>(std::move(columns));
    chunk->generation = generation;
    return chunk;
}
std::vector<std::shared_ptr<const StoredChunk>> minuteChunks(Hmc2Reader& reader, const std::string& layer,
                                                            int64_t from, int64_t to) {
    std::vector<std::shared_ptr<const StoredChunk>> out;
    for (int64_t s = from; s < to; s += hour)
        out.push_back(stored(buildChunk(reader, {"BTC-USD", "hmc2." + layer, minute, s}), out.size() + 1));
    return out;
}
std::vector<std::shared_ptr<const StoredChunk>> served(RollupStore& store, Hmc2Reader& reader,
                                                      const std::vector<ChunkKey>& keys) {
    std::vector<std::shared_ptr<const StoredChunk>> out;
    for (const auto& key : keys) {
        const auto reply = store.serve(key, reader);
        EXPECT_TRUE(reply.wire) << reply.message;
        if (reply.wire) out.push_back(stored(decodeChunk(*reply.wire).columns, out.size() + 100));
    }
    return out;
}
// Captures Qt log lines for the lifetime of the object.
class Logs {
public:
    Logs() {
        std::lock_guard lock(mutex());
        lines() = {};
        previous_ = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
            std::lock_guard lock(mutex());
            lines().push_back(message);
        });
    }
    ~Logs() { qInstallMessageHandler(previous_); }
    size_t count(const QString& text) const {
        std::lock_guard lock(mutex());
        return size_t(std::count_if(lines().begin(), lines().end(), [&](const QString& m) { return m.contains(text); }));
    }
    QString first(const QString& text) const {
        std::lock_guard lock(mutex());
        for (const auto& m : lines()) if (m.contains(text)) return m;
        return {};
    }
private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::vector<QString>& lines() { static std::vector<QString> l; return l; }
    QtMessageHandler previous_ = nullptr;
};
uint32_t word(const BinCell& c) { return tiles::cellWord(c.valid ? tiles::kCellValid : tiles::kCellVeil, c.code, c.dominantAsk); }
struct Diff { uint64_t compared = 0, cells = 0, states = 0; };
// Every bucket's state, and every cell of every present bucket over both
// columns' useful extent plus one sentinel bin each side, at tick `multiple`
// times the native LCM.
void compareCells(const SparseColumns& expected, const SparseColumns& actual, int multiple, Diff& diff) {
    int64_t lcm = 0;
    for (const auto& c : expected.columns) for (const auto& n : c.native) lcm = lcm ? std::lcm(lcm, n.grid.rowTickUnits) : n.grid.rowTickUnits;
    ASSERT_GT(lcm, 0);
    const int64_t tickUnits = lcm * multiple;
    const double tick = double(tickUnits) / 100.0;
    for (int64_t t = expected.startMs; t < expected.endMs; t += expected.tfMs) {
        if (bucketState(expected, t) != bucketState(actual, t)) { ++diff.states; continue; }
        if (bucketState(expected, t) != BucketState::Present) continue;
        const auto find = [&](const SparseColumns& s) {
            return *std::lower_bound(s.columns.begin(), s.columns.end(), t,
                                     [](const SparseColumn& c, int64_t v) { return c.bucketStartMs < v; });
        };
        const auto a = find(expected), b = find(actual);
        auto extent = tiles::usefulUnits(expected, 100, t, t + expected.tfMs);
        const auto other = tiles::usefulUnits(actual, 100, t, t + expected.tfMs);
        extent.lo = std::min(extent.lo, other.lo);
        extent.end = std::max(extent.end, other.end);
        const double lo = std::floor(double(extent.lo) / double(tickUnits)) * tick - tick;
        const double hi = std::ceil(double(extent.end) / double(tickUnits)) * tick + tick;
        const auto x = binColumn(a, lo, hi, tick), y = binColumn(b, lo, hi, tick);
        EXPECT_EQ(x.size(), y.size());
        for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
            ++diff.compared;
            if (word(x[i]) != word(y[i])) ++diff.cells;
        }
    }
}
} // namespace

// The acceptance oracle: stored 1h and 1d chunks bin to the same cells as the
// same days' composed minutes, for both layers, at native, 5x and 50x ticks;
// and 4h composed from stored 1h equals 4h composed from minutes.
TEST_F(RollupTest, StoredLevelsBinLikeComposedMinutes) {
    RollupStore store(options());
    const auto pass = store.runPass();
    EXPECT_EQ(pass.built, 8u); // 4 closed days x 2 layers
    EXPECT_EQ(pass.failed, 0u);
    Hmc2Reader reader(hmc2());
    const int64_t from = day0, to = day0 + 4 * day; // closed days
    for (const std::string layer : {"near", "deep"}) {
        const auto minutes = minuteChunks(reader, layer, from, to);
        std::vector<ChunkKey> hours, days;
        for (int64_t d = from; d < to; d += day) hours.push_back({"BTC-USD", "hmc2." + layer, hour, d});
        const int64_t span = chunkSpanMs(day);
        for (int64_t s = chunkStartOf(day, from); s < to; s += span) days.push_back({"BTC-USD", "hmc2." + layer, day, s});
        ASSERT_EQ(days.size(), 2u) << "the fixture crosses a 1d chunk boundary";
        const auto hourChunks = served(store, reader, hours), dayChunks = served(store, reader, days);
        for (const int64_t tf : {hour, 4 * hour, day}) {
            const auto expected = tiles::composeChunks(minutes, "BTC-USD", layer, tf, from, to);
            const auto actual = tiles::composeChunks(tf == day ? dayChunks : hourChunks, "BTC-USD", layer, tf, from, to);
            ASSERT_FALSE(expected.columns.empty());
            for (const int multiple : {1, 5, 50}) {
                Diff diff;
                compareCells(expected, actual, multiple, diff);
                EXPECT_EQ(diff.states, 0u) << layer << " tf=" << tf << " x" << multiple;
                EXPECT_EQ(diff.cells, 0u) << layer << " tf=" << tf << " x" << multiple;
                EXPECT_GT(diff.compared, 0u);
            }
        }
        // Stored columns carry exact numerators: composing them is not a second
        // quantization of rounded codes.
        bool composed = false;
        for (const auto& c : hourChunks.front()->columns->columns) for (const auto& n : c.native) composed |= n.composed;
        EXPECT_TRUE(composed);
    }
}

TEST_F(RollupTest, OpenDayIsNeverBuiltAndLevelsEndAtTheLastClosedDay) {
    through = day0 + 3 * day + 5 * hour; // day 3 is open
    RollupStore store(options());
    EXPECT_EQ(store.runPass().built, 6u);
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "deep", hour, day0 + 3 * day)));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "near", day, day0 + 3 * day)));
    const auto levels = store.levels("BTC-USD", "deep");
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[0].levelMs, hour);
    EXPECT_EQ(levels[0].oldestMs, day0 + 13 * hour); // the first recorded hour
    EXPECT_EQ(levels[0].throughMs, day0 + 3 * day);
    EXPECT_EQ(levels[1].levelMs, day);
    EXPECT_EQ(levels[1].oldestMs, day0);
    EXPECT_EQ(levels[1].throughMs, day0 + 3 * day);
    Hmc2Reader reader(hmc2());
    const auto open = store.serve({"BTC-USD", "hmc2.deep", hour, day0 + 3 * day}, reader);
    EXPECT_FALSE(open.wire);
    EXPECT_EQ(open.error, ChunkError::Unavailable);
}

TEST_F(RollupTest, RebuildIsIdempotentAndByteIdentical) {
    RollupStore store(options());
    store.runPass();
    const auto path = store.filePath("BTC-USD", "deep", hour, day0 + day);
    const auto piece = store.filePath("BTC-USD", "near", day, day0 + 2 * day);
    const auto before = bytes(path), pieceBefore = bytes(piece);
    ASSERT_FALSE(before.empty());
    const auto stamp = fs::last_write_time(path);
    const auto generation = store.generation();
    EXPECT_EQ(store.runPass().built, 0u) << "current days are not rebuilt";
    EXPECT_EQ(store.generation(), generation) << "a re-check never shrinks the advertised run, even briefly";
    EXPECT_EQ(fs::last_write_time(path), stamp);
    fs::remove_all(rollupRoot());
    EXPECT_EQ(store.runPass().built, 8u);
    EXPECT_EQ(bytes(path), before);
    EXPECT_EQ(bytes(piece), pieceBefore);
}

TEST_F(RollupTest, KilledBuildLeavesNoServedPartialFile) {
    auto o = options();
    const auto victim = RollupStore(options()).filePath("BTC-USD", "deep", hour, day0 + 2 * day);
    o.beforeRename = [&](const fs::path& tempPath, const fs::path& final) {
        if (final == victim) {
            // A kill here leaves only the temp file (copied aside: the throw
            // below removes the original, as an orderly failure does).
            fs::copy_file(tempPath, fs::path(victim.string() + ".tmp-999-0"));
            throw std::runtime_error("killed");
        }
    };
    {
        RollupStore store(o);
        const auto pass = store.runPass();
        EXPECT_EQ(pass.failed, 1u);
        EXPECT_FALSE(fs::exists(victim));
        // Days after the failed one are not advertised past the gap.
        const auto levels = store.levels("BTC-USD", "deep");
        ASSERT_FALSE(levels.empty());
        EXPECT_EQ(levels[0].oldestMs, day0 + 3 * day);
        // A request for the missing day composes its minutes: same bytes as a clean build.
        Hmc2Reader reader(hmc2());
        const auto reply = store.serve({"BTC-USD", "hmc2.deep", hour, day0 + 2 * day}, reader);
        ASSERT_TRUE(reply.wire);
        EXPECT_TRUE(reply.fallback);
        ReadControl control;
        EXPECT_EQ(*reply.wire, encodeHourFrame("BTC-USD", composeDay(reader, "BTC-USD", "deep", day0 + 2 * day, control),
                                               day0 + 2 * day));
    }
    // Restart: the stray temp file is removed and the day is built.
    RollupStore store(options());
    store.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120); // background QoS under load
    auto oldest = [&] {
        const auto levels = store.levels("BTC-USD", "deep");
        return levels.empty() ? int64_t(0) : levels.front().oldestMs;
    };
    while ((oldest() != day0 + 13 * hour || fs::exists(victim.string() + ".tmp-999-0")) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    store.stop();
    EXPECT_TRUE(fs::exists(victim));
    EXPECT_FALSE(fs::exists(victim.string() + ".tmp-999-0"));
    EXPECT_EQ(store.levels("BTC-USD", "deep").front().oldestMs, day0 + 13 * hour);
}

TEST_F(RollupTest, CorruptFileIsDetectedServedFromMinutesAndRebuilt) {
    RollupStore store(options());
    store.runPass();
    Hmc2Reader reader(hmc2());
    const ChunkKey key{"BTC-USD", "hmc2.near", hour, day0 + day};
    const auto good = store.serve(key, reader);
    ASSERT_TRUE(good.wire);
    EXPECT_FALSE(good.fallback);
    const auto path = store.filePath("BTC-USD", "near", hour, day0 + day);
    auto corrupt = bytes(path);
    ASSERT_FALSE(corrupt.empty());
    corrupt[corrupt.size() / 2] ^= 0x5a; // same length: only the frame decode notices
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(corrupt.data()),
                                                                   std::streamsize(corrupt.size()));
    const auto reply = store.serve(key, reader);
    ASSERT_TRUE(reply.wire);
    EXPECT_TRUE(reply.fallback);
    EXPECT_EQ(*reply.wire, *good.wire);
    // The 1d chunk holding a corrupt piece is also served from minutes.
    const auto piecePath = store.filePath("BTC-USD", "near", day, day0 + day);
    auto piece = bytes(piecePath);
    ASSERT_FALSE(piece.empty());
    piece.resize(piece.size() - 3); // torn
    std::ofstream(piecePath, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(piece.data()),
                                                                        std::streamsize(piece.size()));
    const int64_t span = chunkSpanMs(day);
    const ChunkKey dayKey{"BTC-USD", "hmc2.near", day, chunkStartOf(day, day0)};
    const auto dayReply = store.serve(dayKey, reader);
    ASSERT_TRUE(dayReply.wire);
    EXPECT_TRUE(dayReply.fallback);
    EXPECT_EQ(store.counters().fallbackServes.load(), 2u);
    // The next pass rebuilds exactly the suspect days.
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_FALSE(store.serve(key, reader).fallback);
    const auto rebuilt = store.serve(dayKey, reader);
    EXPECT_FALSE(rebuilt.fallback);
    EXPECT_EQ(*rebuilt.wire, *dayReply.wire);
}

TEST_F(RollupTest, ChangedMinuteFilesRebuildTheirDay) {
    RollupStore store(options());
    store.runPass();
    // A closed day's minute file is rewritten (e.g. a later backfill).
    const auto minutes = Hmc2Store::filePath(hmc2(), record("deep", day0 + day).header, day0 + day);
    ASSERT_TRUE(fs::exists(minutes));
    fs::last_write_time(minutes, fs::last_write_time(minutes) + std::chrono::seconds(5));
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_EQ(store.runPass().built, 0u);
}

// Scenario: the server restarts mid-backfill. Backfill runs newest first and
// each built day is advertised at once; 1d starts on a chunk boundary until
// the backfill reaches the first recorded day.
TEST_F(RollupTest, RestartMidBackfillResumesAndAdvertisesOnlyBuiltDays) {
    std::stop_source stop;
    auto o = options();
    int renames = 0;
    o.beforeRename = [&](const fs::path&, const fs::path&) {
        if (++renames == 4) stop.request_stop(); // near: days 3 and 2 written
    };
    {
        RollupStore store(o);
        const auto pass = store.runPass(stop.get_token());
        EXPECT_TRUE(pass.stopped);
        EXPECT_EQ(pass.built, 2u);
        EXPECT_EQ(pass.pending, 2u);
        EXPECT_EQ(store.counters().pendingDays.load(std::memory_order_relaxed), 2);
        const auto levels = store.levels("BTC-USD", "near");
        ASSERT_EQ(levels.size(), 2u);
        EXPECT_EQ(levels[0].oldestMs, day0 + 2 * day);
        EXPECT_EQ(levels[0].throughMs, day0 + 4 * day);
        const int64_t span = chunkSpanMs(day);
        EXPECT_EQ(levels[1].oldestMs, chunkStartOf(day, day0 + 2 * day + span - 1)); // chunk boundary
        EXPECT_TRUE(store.levels("BTC-USD", "deep").empty());
    }
    RollupStore store(options());
    const auto pass = store.runPass();
    EXPECT_EQ(pass.built, 6u); // near days 1, 0 and all deep days
    EXPECT_EQ(store.levels("BTC-USD", "near")[0].oldestMs, day0 + 13 * hour);
    EXPECT_EQ(store.levels("BTC-USD", "near")[1].oldestMs, day0);
}

TEST_F(RollupTest, PendingDaysCountsDownDuringStartupBackfill) {
    auto o = options();
    std::vector<int64_t> observed;
    RollupStore* active = nullptr;
    o.beforeRename = [&](const fs::path&, const fs::path&) {
        observed.push_back(active->counters().pendingDays.load(std::memory_order_relaxed));
    };
    RollupStore store(o);
    active = &store;
    const auto pass = store.runPass();
    ASSERT_EQ(pass.built, 8u);
    ASSERT_EQ(observed.size(), 16u);
    EXPECT_EQ(observed.front(), 4);
    EXPECT_EQ(observed[8], 4) << "the second series is counted when its pass begins";
    EXPECT_EQ(observed.back(), 1);
    EXPECT_TRUE(std::is_sorted(observed.rbegin(), observed.rbegin() + 8));
    EXPECT_TRUE(std::is_sorted(observed.rbegin() + 8, observed.rend()));
    EXPECT_EQ(store.counters().pendingDays.load(std::memory_order_relaxed), 0);
}

TEST_F(RollupTest, FailedAndSkippedDaysStayPendingAfterPass) {
    auto o = options();
    bool failOnce = true;
    o.beforeRename = [&](const fs::path&, const fs::path&) {
        if (failOnce) {
            failOnce = false;
            throw std::runtime_error("scripted rename failure");
        }
    };
    RollupStore store(o);
    const auto failed = store.runPass();
    EXPECT_EQ(failed.built, 7u);
    EXPECT_EQ(failed.pending, 1u);
    EXPECT_EQ(store.counters().pendingDays.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(store.runPass().pending, 0u);
    EXPECT_EQ(store.counters().pendingDays.load(std::memory_order_relaxed), 0);

    o = options();
    o.buildMissing = false;
    o.root = fs::path(temp.path().toStdString()) / "read-only-rollups";
    RollupStore readOnly(o);
    EXPECT_EQ(readOnly.runPass().pending, 8u);
    EXPECT_EQ(readOnly.counters().pendingDays.load(std::memory_order_relaxed), 8);
}

// Scenario: the UTC day rolls over while serving: the new day is built,
// availability advances (generation), the open 1d chunk gains it.
TEST_F(RollupTest, DayRolloverAdvancesLevelsAndTheOpenDayChunk) {
    through = day0 + 3 * day + 59 * minute;
    RollupStore store(options());
    store.runPass();
    Hmc2Reader reader(hmc2());
    const int64_t span = chunkSpanMs(day);
    const ChunkKey dayKey{"BTC-USD", "hmc2.deep", day, chunkStartOf(day, day0 + 3 * day)};
    const auto before = decodeChunk(*store.serve(dayKey, reader).wire);
    const auto generation = store.generation();
    EXPECT_FALSE(before.state.sealed);
    EXPECT_EQ(before.state.committedThroughMs, day0 + 3 * day);
    through = day0 + 4 * day + minute; // the recorder sealed day 3
    EXPECT_EQ(store.runPass().built, 2u);
    EXPECT_GT(store.generation(), generation);
    EXPECT_EQ(store.levels("BTC-USD", "deep")[0].throughMs, day0 + 4 * day);
    const auto after = decodeChunk(*store.serve(dayKey, reader).wire);
    EXPECT_FALSE(after.state.sealed);
    EXPECT_EQ(after.state.committedThroughMs, day0 + 4 * day);
    EXPECT_GE(after.state.revision, before.state.revision) << "growth is ordered by the cutoff";
    EXPECT_EQ(bucketState(after.columns, day0 + 3 * day), BucketState::Present);
    EXPECT_EQ(bucketState(before.columns, day0 + 3 * day), BucketState::NotLoaded);
    // The 1d chunk before it: sealed, scanning its days before the first minute as gaps.
    const auto first = decodeChunk(*store.serve({"BTC-USD", "hmc2.deep", day, chunkStartOf(day, day0)}, reader).wire);
    EXPECT_TRUE(first.state.sealed);
    EXPECT_EQ(bucketState(first.columns, day0 - day), BucketState::Gap);
}

// Scenario: the store directory is deleted while the server runs.
TEST_F(RollupTest, DeletedDirectoryServesMinutesThenRebuilds) {
    RollupStore store(options());
    store.runPass();
    Hmc2Reader reader(hmc2());
    const ChunkKey key{"BTC-USD", "hmc2.deep", hour, day0 + 2 * day};
    const auto good = store.serve(key, reader);
    fs::remove_all(rollupRoot());
    const auto reply = store.serve(key, reader);
    ASSERT_TRUE(reply.wire);
    EXPECT_TRUE(reply.fallback);
    EXPECT_EQ(*reply.wire, *good.wire);
    EXPECT_EQ(store.runPass().built, 8u);
    EXPECT_FALSE(store.serve(key, reader).fallback);
}

TEST_F(RollupTest, ChunkServiceAdvertisesAndServesStoredLevels) {
    auto rollups = std::make_shared<RollupStore>(options());
    rollups->runPass();
    ChunkService service(hmc2(), [this](const std::string&, const std::string&) {
        return BookRecorder::Watermarks{through, 0, 0};
    }, [] { return int64_t(0); });
    const auto noRollups = service.availability("BTC-USD");
    ASSERT_EQ(noRollups.size(), 2u);
    EXPECT_EQ(noRollups[0].levels.size(), 1u) << "minutes only without a store; no recorder hour level";
    const auto fingerprint = service.availabilityFingerprint("BTC-USD");
    service.attachRollups(rollups);
    EXPECT_NE(service.availabilityFingerprint("BTC-USD"), fingerprint);
    const auto sources = service.availability("BTC-USD");
    for (const auto& source : sources) {
        ASSERT_EQ(source.levels.size(), 3u) << source.id;
        EXPECT_EQ(source.levels[1].levelMs, hour);
        EXPECT_EQ(source.levels[1].chunkSpanMs, day);
        EXPECT_EQ(source.levels[1].committedThroughMs, day0 + 4 * day);
        EXPECT_EQ(source.levels[2].levelMs, day);
        EXPECT_EQ(source.levels[2].chunkSpanMs, chunkSpanMs(day));
    }
    const ChunkKey key{"BTC-USD", "hmc2.near", hour, day0 + day};
    const auto wire = service.serve(key, 0);
    const auto frame = decodeChunk(*wire);
    ASSERT_EQ(frame.kind, ChunkKind::Chunk);
    EXPECT_TRUE(frame.state.sealed);
    const auto again = decodeChunk(*service.serve(key, frame.contentHash));
    EXPECT_EQ(again.kind, ChunkKind::NotModified);
    EXPECT_EQ(decodeChunk(*service.serve({"BTC-USD", "hmc2.near", hour, day0 + day + hour}, 0)).kind,
              ChunkKind::Error) << "unaligned";
}

TEST(RollupRoot, DisjointFromTheTreeInBothDirectionsAndResolved) {
    EXPECT_EQ(rollupRootFor("", "/data/hmc2"), fs::path("/data/hmc2-rollups"));
    EXPECT_EQ(rollupRootFor("", "/data/hmc2/"), fs::path("/data/hmc2-rollups")) << "trailing slash";
    EXPECT_EQ(rollupRootFor("/other/rollups/", "/data/hmc2"), fs::path("/other/rollups"));
    EXPECT_TRUE(rollupRootFor("/data/hmc2/rollups", "/data/hmc2").empty()) << "inside the tree";
    EXPECT_TRUE(rollupRootFor("/data/hmc2", "/data/hmc2").empty()) << "the tree itself";
    EXPECT_TRUE(rollupRootFor("/data/hmc2/../hmc2/x", "/data/hmc2").empty()) << "inside, after normalization";
    EXPECT_TRUE(rollupRootFor("/data", "/data/hmc2").empty()) << "an ancestor of the tree";
    EXPECT_TRUE(rollupRootFor("/", "/data/hmc2").empty()) << "the filesystem root";
    EXPECT_TRUE(rollupRootFor("x", "").empty()) << "unresolvable tree";
    // Relative paths resolve against the working directory.
    EXPECT_EQ(rollupRootFor("rel-rollups", "/data/hmc2"),
              fs::weakly_canonical(fs::absolute("rel-rollups")));
    EXPECT_TRUE(rollupRootFor(".", fs::current_path() / "hmc2").empty()) << "relative ancestor";
    // Symlinks are followed: a link into the tree is inside it.
    QTemporaryDir temp;
    const fs::path base = fs::weakly_canonical(fs::path(temp.path().toStdString()));
    fs::create_directories(base / "hmc2");
    fs::create_directory_symlink(base / "hmc2", base / "link");
    EXPECT_TRUE(rollupRootFor((base / "link" / "rollups").string(), base / "hmc2").empty());
    fs::create_directory_symlink(base, base / "up");
    EXPECT_TRUE(rollupRootFor((base / "up").string(), base / "hmc2").empty()) << "a link to an ancestor";
    EXPECT_EQ(rollupRootFor("", base / "link"), base / "hmc2-rollups") << "the tree's real path decides";
    RollupStore::Options o;
    o.hmc2Root = base / "hmc2";
    o.root = base / "link" / "r";
    o.minuteThrough = [](const std::string&, const std::string&) { return int64_t(0); };
    EXPECT_THROW(RollupStore{o}, std::invalid_argument);
}

TEST_F(RollupTest, NeverWritesThroughALinkBelowTheRoot) {
    const fs::path outside = fs::path(temp.path().toStdString()) / "outside";
    fs::create_directories(outside);
    fs::create_directories(rollupRoot() / "BTC-USD");
    fs::create_directory_symlink(outside, rollupRoot() / "BTC-USD" / "near");
    RollupStore store(options());
    const auto pass = store.runPass();
    EXPECT_EQ(pass.failed, 4u) << "every closed near day";
    EXPECT_EQ(pass.built, 4u) << "deep is unaffected";
    EXPECT_TRUE(fs::is_empty(outside));
    EXPECT_TRUE(store.levels("BTC-USD", "near").empty());
}

TEST_F(RollupTest, StartupCleanupRemovesOnlyOwnedTempsAndSurvivesLinkLoops) {
    RollupStore(options()).runPass();
    const auto dir = rollupRoot() / "BTC-USD" / "deep" / std::to_string(hour);
    const auto owned = dir / "2000-01-15.shr.tmp-123-4";
    std::ofstream(owned) << "partial";
    std::ofstream(dir / "notes.tmp-1") << "not ours";
    std::ofstream(rollupRoot() / "2000-01-15.shr.tmp-1-2") << "not in a level directory";
    const fs::path outsideFile = fs::path(temp.path().toStdString()) / "keep.txt";
    std::ofstream(outsideFile) << "keep";
    fs::create_symlink(outsideFile, dir / "2000-01-16.shr.tmp-5-6"); // owned name, but a link
    fs::create_symlink(dir / "loop", dir / "loop");                  // ELOOP when followed
    fs::create_directory_symlink(dir, rollupRoot() / "BTC-USD" / "near" / "loop");
    RollupStore store(options());
    store.start(); // cleanup runs on the builder thread; a throw there would terminate the test
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (fs::exists(owned) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    store.stop();
    EXPECT_FALSE(fs::exists(owned));
    EXPECT_TRUE(fs::exists(dir / "notes.tmp-1"));
    EXPECT_TRUE(fs::exists(rollupRoot() / "2000-01-15.shr.tmp-1-2"));
    EXPECT_TRUE(fs::is_symlink(dir / "2000-01-16.shr.tmp-5-6"));
    EXPECT_TRUE(fs::exists(outsideFile));
}

// A suspect (payload-corrupt) day stays marked until its replacement is
// written: one failed replacement, then a pass with no serve in between.
TEST_F(RollupTest, SuspectDayStaysMarkedUntilReplaced) {
    bool failRename = false;
    auto o = options();
    o.beforeRename = [&](const fs::path&, const fs::path&) {
        if (failRename) throw std::runtime_error("disk full");
    };
    RollupStore store(o);
    store.runPass();
    Hmc2Reader reader(hmc2());
    const ChunkKey key{"BTC-USD", "hmc2.deep", hour, day0 + day};
    const auto good = store.serve(key, reader);
    const auto path = store.filePath("BTC-USD", "deep", hour, day0 + day);
    auto corrupt = bytes(path);
    ASSERT_FALSE(corrupt.empty());
    corrupt[corrupt.size() / 2] ^= 0x5a; // header intact: only a full read notices
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(corrupt.data()),
                                                                   std::streamsize(corrupt.size()));
    EXPECT_TRUE(store.serve(key, reader).fallback);
    failRename = true;
    EXPECT_EQ(store.runPass().failed, 1u);
    failRename = false;
    EXPECT_EQ(store.runPass().built, 1u) << "the mark survived the failed replacement";
    const auto repaired = store.serve(key, reader);
    EXPECT_FALSE(repaired.fallback);
    EXPECT_EQ(*repaired.wire, *good.wire);
    EXPECT_EQ(store.runPass().built, 0u) << "the mark is cleared once replaced";
}

// Changed minute files of a closed day (a backfill or repair): the next pass
// rebuilds the day from its fingerprint, and a new request (a new GUI) gets
// the new picture. Sealed rollup chunks keep revision 0: a running GUI does
// not revalidate what it holds; a restart guarantees a fresh fetch.
TEST_F(RollupTest, ChangedMinuteFilesGiveANewRequestTheRebuiltPicture) {
    RollupStore store(options());
    store.runPass();
    const ChunkKey key{"BTC-USD", "hmc2.deep", hour, day0 + day};
    Hmc2Reader before(hmc2());
    const auto old = decodeChunk(*store.serve(key, before).wire);
    EXPECT_EQ(bucketState(old.columns, day0 + day + 7 * hour), BucketState::Gap);
    for (const auto& entry : fs::directory_iterator(changedTree() / "BTC-USD" / "deep-60000")) {
        const auto target = hmc2() / "BTC-USD" / "deep-60000" / entry.path().filename();
        const auto time = fs::exists(target) ? fs::last_write_time(target) : fs::file_time_type::clock::now();
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        fs::last_write_time(target, time); // even a preserved timestamp: the size differs
    }
    EXPECT_EQ(store.runPass().built, 1u);
    Hmc2Reader after(hmc2());
    const auto rebuilt = decodeChunk(*store.serve(key, after).wire);
    EXPECT_TRUE(rebuilt.state.sealed);
    EXPECT_EQ(rebuilt.state.revision, 0u);
    EXPECT_NE(rebuilt.contentHash, old.contentHash);
    EXPECT_EQ(bucketState(rebuilt.columns, day0 + day + 7 * hour), BucketState::Present);
}

// The documented procedure for a rewrite the fingerprint cannot see (same
// names, sizes and modification times): the rewriting tool deletes that day's
// rollup files, and the next pass rebuilds them from the minutes.
TEST_F(RollupTest, DeletingADaysRollupFilesRebuildsIt) {
    RollupStore store(options());
    store.runPass();
    fs::remove(store.filePath("BTC-USD", "near", hour, day0 + 2 * day));
    fs::remove(store.filePath("BTC-USD", "near", day, day0 + 2 * day));
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_TRUE(fs::exists(store.filePath("BTC-USD", "near", hour, day0 + 2 * day)));
}

// Reconciliation uses one snapshot of the headers. A deletion during a build
// is handled by serve() immediately and by the following reconciliation pass.
TEST_F(RollupTest, HeaderChecksAreReusedWithinOnePassOnly) {
    bool removeOlder = false;
    auto o = options();
    const auto older = RollupStore(o).filePath("BTC-USD", "near", hour, day0 + day);
    o.beforeRename = [&](const fs::path&, const fs::path&) {
        if (std::exchange(removeOlder, false)) fs::remove(older);
    };
    RollupStore store(o);
    ASSERT_EQ(store.runPass().built, 8u);
    fs::remove(store.filePath("BTC-USD", "near", hour, day0 + 3 * day));
    removeOlder = true;
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_FALSE(fs::exists(older));
    Hmc2Reader reader(hmc2());
    EXPECT_TRUE(store.serve({"BTC-USD", "hmc2.near", hour, day0 + day}, reader).fallback);
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_TRUE(fs::exists(older));
    EXPECT_EQ(store.runPass().built, 0u);
}

// Ages the directory's modification time past the listing settle time, to a
// new value each call (a change the store must notice).
void settle(const fs::path& dir) {
    static int n = 0;
    fs::last_write_time(dir, fs::file_time_type::clock::now() - std::chrono::hours(1) + std::chrono::seconds(++n));
}

// Chunk requests cost O(requested days): an unchanged minute directory is
// listed once however many requests and days there are, and each request stats
// only its own day's files.
TEST_F(RollupTest, ChunkRequestsListTheMinuteDirectoryOnce) {
    RollupStore store(options());
    ASSERT_EQ(store.runPass().built, 8u);
    const auto dir = hmc2() / "BTC-USD" / "near-60000";
    for (int i = 10; i < 400; ++i) // a long history beyond the requested days (never read)
        std::ofstream(Hmc2Store::filePath(hmc2(), record("near", day0).header, day0 + i * day));
    settle(dir);
    std::map<int64_t, uint64_t> filesOfDay; // minute files per requested day
    for (int64_t d = day0; d < day0 + 4 * day; d += day) {
        const auto prefix = Hmc2Store::filePath(hmc2(), record("near", d).header, d).filename().string().substr(0, 11);
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().starts_with(prefix)) ++filesOfDay[d];
    }
    Hmc2Reader reader(hmc2());
    const auto listings = store.counters().minuteListings.load();
    const auto stats = store.counters().minuteFileStats.load();
    uint64_t expectedStats = 0;
    constexpr int requests = 40;
    for (int i = 0; i < requests; ++i) {
        const int64_t d = day0 + (i % 4) * day;
        const auto reply = store.serve({"BTC-USD", "hmc2.near", hour, d}, reader);
        ASSERT_TRUE(reply.wire) << reply.message;
        EXPECT_FALSE(reply.fallback);
        expectedStats += filesOfDay[d];
    }
    EXPECT_EQ(store.counters().minuteListings.load() - listings, 1u) << "one listing for every request";
    EXPECT_EQ(store.counters().minuteFileStats.load() - stats, expectedStats) << "only the requested days' files";
    EXPECT_EQ(expectedStats, uint64_t(requests)) << "one minute file per near day";
}

// A minute file added, replaced (in place, and by rename with the same size
// and a new time) or removed between requests is seen by the next request,
// including a change inside the directory's modification-time granularity.
TEST_F(RollupTest, MinuteChangesBetweenRequestsAreSeenByTheNextRequest) {
    RollupStore store(options());
    ASSERT_EQ(store.runPass().built, 8u);
    const auto dir = hmc2() / "BTC-USD" / "near-60000";
    const auto listings = [&] { return store.counters().minuteListings.load(); };
    auto serve = [&](int64_t d) {
        Hmc2Reader reader(hmc2()); // a fresh reader: only the store's own reuse is under test
        auto reply = store.serve({"BTC-USD", "hmc2.near", hour, d}, reader);
        EXPECT_TRUE(reply.wire) << reply.message;
        return reply;
    };
    // A recorder-gap day before the first recorded day, and its minute file
    // staged elsewhere.
    const int64_t gapDay = day0 - day;
    const auto header = record("near", gapDay).header;
    const fs::path staging = fs::path(temp.path().toStdString()) / "staging";
    {
        Hmc2Store staged(staging);
        for (int64_t t = gapDay + 5 * hour; t < gapDay + 5 * hour + 10 * minute; t += minute)
            staged.append(record("near", t));
    }
    const auto stagedFile = Hmc2Store::filePath(staging, header, gapDay);
    const auto gapFile = Hmc2Store::filePath(hmc2(), header, gapDay);
    ASSERT_TRUE(fs::exists(stagedFile));
    auto add = [&] {
        fs::copy_file(stagedFile, dir / "incoming.tmp");
        fs::rename(dir / "incoming.tmp", gapFile);
    };

    settle(dir);
    const auto gap = serve(gapDay);
    EXPECT_FALSE(gap.fallback) << "no minute files: nothing to fall back from";
    const auto good = serve(day0 + day);
    EXPECT_FALSE(good.fallback);
    auto before = listings();
    EXPECT_FALSE(serve(day0 + day).fallback);
    EXPECT_EQ(listings(), before) << "an unchanged directory is not listed again";

    // Added.
    add();
    settle(dir);
    EXPECT_TRUE(serve(gapDay).fallback);
    EXPECT_EQ(listings(), before + 1);

    // Replaced in place: same name and size, new time. No listing needed.
    before = listings();
    const auto inPlace = Hmc2Store::filePath(hmc2(), header, day0 + day);
    fs::last_write_time(inPlace, fs::last_write_time(inPlace) + std::chrono::seconds(5));
    EXPECT_TRUE(serve(day0 + day).fallback);
    EXPECT_EQ(listings(), before);

    // Replaced by rename over it: same size, new time.
    const auto renamed = Hmc2Store::filePath(hmc2(), header, day0 + 2 * day);
    EXPECT_FALSE(serve(day0 + 2 * day).fallback);
    fs::copy_file(renamed, dir / "replacement.tmp");
    fs::last_write_time(dir / "replacement.tmp", fs::last_write_time(renamed) + std::chrono::seconds(7));
    ASSERT_EQ(fs::file_size(dir / "replacement.tmp"), fs::file_size(renamed));
    fs::rename(dir / "replacement.tmp", renamed);
    settle(dir);
    EXPECT_TRUE(serve(day0 + 2 * day).fallback);

    // Removed.
    fs::remove(gapFile);
    settle(dir);
    const auto removed = serve(gapDay);
    EXPECT_FALSE(removed.fallback);
    EXPECT_EQ(*removed.wire, *gap.wire);

    // Added within the directory's time granularity: listed while the
    // directory time was recent, then a change that leaves the same time.
    const auto recent = fs::file_time_type::clock::now();
    fs::last_write_time(dir, recent);
    EXPECT_FALSE(serve(gapDay).fallback);
    add();
    fs::last_write_time(dir, recent);
    EXPECT_TRUE(serve(gapDay).fallback) << "a listing taken inside the granularity is never reused";
}

// The minute fallback is logged once per series day per interval, not per
// request; the counter still counts every fallback serve.
TEST_F(RollupTest, FallbackWarningIsLoggedOncePerSeriesDay) {
    RollupStore store(options());
    ASSERT_EQ(store.runPass().built, 8u);
    fs::remove(store.filePath("BTC-USD", "near", hour, day0 + day));
    fs::remove(store.filePath("BTC-USD", "near", hour, day0 + 2 * day));
    Hmc2Reader reader(hmc2());
    Logs logs;
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(store.serve({"BTC-USD", "hmc2.near", hour, day0 + day}, reader).fallback);
    EXPECT_EQ(logs.count("Rollup file missing, corrupt or stale"), 1u);
    EXPECT_TRUE(store.serve({"BTC-USD", "hmc2.near", hour, day0 + 2 * day}, reader).fallback);
    EXPECT_EQ(logs.count("Rollup file missing, corrupt or stale"), 2u) << "another day has its own line";
    EXPECT_EQ(store.counters().fallbackServes.load(), 6u);
}

TEST_F(RollupTest, FallbackWarningThrottleRetainsTrackedKeysAtCapacity) {
    RollupStore store(options());
    uint64_t repeats = 0;
    ASSERT_TRUE(RollupStoreTestAccess::logDue(store, "zz-oldest", day0, repeats));
    for (int64_t i = 0; i < 4095; ++i)
        ASSERT_TRUE(RollupStoreTestAccess::logDue(store, "symbol-" + std::to_string(i), day0, repeats));
    ASSERT_EQ(RollupStoreTestAccess::logCount(store), 4096u);

    EXPECT_FALSE(RollupStoreTestAccess::logDue(store, "symbol-0", day0, repeats))
        << "a tracked key must remain throttled when the map is full";
    EXPECT_EQ(RollupStoreTestAccess::logCount(store), 4096u);
    EXPECT_TRUE(RollupStoreTestAccess::logDue(store, "new-symbol", day0, repeats));
    EXPECT_EQ(RollupStoreTestAccess::logCount(store), 4096u);
    EXPECT_FALSE(RollupStoreTestAccess::logDue(store, "symbol-1", day0, repeats))
        << "admitting one new key must not reset unrelated throttles";
    EXPECT_FALSE(RollupStoreTestAccess::logDue(store, "symbol-0", day0, repeats))
        << "the map's first key is newer than zz-oldest and must remain throttled";
    EXPECT_TRUE(RollupStoreTestAccess::logDue(store, "zz-oldest", day0, repeats))
        << "the first inserted key makes room for the new key despite sorting last";
    EXPECT_EQ(RollupStoreTestAccess::logCount(store), 4096u);
}

namespace {
// Real roller writes and durable recorder fences, entirely in /tmp. The
// second pass fills more minutes of the same closed UTC day's HMC2 files.
class RollerRollupTest : public testing::Test {
protected:
    QTemporaryDir temp{QStringLiteral("/tmp/sentinel-rollup-rewrite-XXXXXX")};
    fs::path root() const {
        const auto inherited = qgetenv("SENTINEL_ROLLUP_CRASH_ROOT");
        return inherited.isEmpty() ? temp.path().toStdString() : inherited.toStdString();
    }
    int64_t start = 1'577'836'800'000; // 2020-01-01 UTC
    sentinel::roller::RollOptions rollOptions() const {
        sentinel::roller::RollOptions o{root() / "raw", root() / "hmc2", "BTC-USD", start, start + 4 * minute};
        o.useAnchors = o.writeAnchors = false;
        return o;
    }
    RollupStore::Options storeOptions() const {
        RollupStore::Options o;
        o.hmc2Root = root() / "hmc2";
        o.root = rollupRootFor("", o.hmc2Root);
        o.symbols = {"BTC-USD"};
        o.minuteThrough = [dayStart = start](const auto&, const auto&) { return dayStart + day; };
        return o;
    }
    void SetUp() override {
        static int argc = 1;
        static char name[] = "test_rollup_store";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
        if (!qgetenv("SENTINEL_ROLLUP_CRASH_ROOT").isEmpty()) {
            temp.remove(); // child uses the parent's tree and exits without destructors
            return;
        }
        ASSERT_TRUE(temp.isValid());
        using namespace sentinel::capture;
        WriterConfig cfg;
        cfg.root = QString::fromStdString((root() / "raw").string());
        cfg.fsyncBlocks = 0;
        Writer writer(cfg, {{"product_metadata", {{"product_id", "BTC-USD"},
            {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}});
        const std::string snapshot = R"({"channel":"l2_data","events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","price_level":"99999","new_quantity":"2"},{"side":"offer","price_level":"100001","new_quantity":"3"}]}]})";
        for (int64_t ms = 0; ms <= 245'000; ms += 1000)
            writer.append({Kind::Frame, {(start + ms) * 1'000'000, (ms + 1) * 1'000'000}, 1,
                           ms == 0 ? snapshot : R"({"channel":"heartbeats"})"});
        writer.close();
        auto prefix = rollOptions();
        prefix.toMs = start + minute;
        sentinel::roller::roll(prefix);
    }
};
class RollerOpenDayTest : public RollerRollupTest {
    void SetUp() override {
        // Tomorrow remains open even if this test runs across UTC midnight.
        start = QDateTime::currentMSecsSinceEpoch() / day * day + day;
        RollerRollupTest::SetUp();
    }
};
} // namespace

TEST_F(RollerOpenDayTest, WritesDoNotDependOnTheDerivedStore) {
    auto o = rollOptions();
    o.rollupRoot = o.outputRoot; // invalid for deletion; must never be touched for an open day
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_EQ(Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", minute, start, start + 4 * minute).size(), 4u);
}

TEST_F(RollerRollupTest, BackfillDeletesBothLayersAndLevelsAfterDurableWrites) {
    RollupStore store(storeOptions());
    ASSERT_EQ(store.runPass().built, 2u);
    const auto neighbour = store.filePath("BTC-USD", "near", hour, start - day);
    std::ofstream(neighbour) << "other day";
    const auto otherProduct = store.filePath("ETH-USD", "near", hour, start);
    fs::create_directories(otherProduct.parent_path());
    std::ofstream(otherProduct) << "other product";
    auto o = rollOptions();
    bool sawDurableBeforeDelete = false;
    o.afterFenceForTest = [&] {
        const auto rows = Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", minute, start, start + 4 * minute);
        if (rows.size() > 1 && !sawDurableBeforeDelete) {
            sawDurableBeforeDelete = true;
            EXPECT_TRUE(fs::exists(store.filePath("BTC-USD", "near", hour, start)));
        }
    };
    sentinel::roller::roll(o);
    EXPECT_TRUE(sawDurableBeforeDelete);
    EXPECT_TRUE(fs::exists(neighbour));
    EXPECT_TRUE(fs::exists(otherProduct));
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day})
            EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", layer, level, start)));
    EXPECT_EQ(store.runPass().built, 2u);
    // An idempotent retry with no new columns must leave rebuilt files alone.
    sentinel::roller::roll(o);
    EXPECT_EQ(store.runPass().built, 0u);
}

TEST_F(RollerRollupTest, CrashAfterRewriteBeforeDeleteNeverServesStaleRollups) {
    if (!qgetenv("SENTINEL_ROLLUP_CRASH_ROOT").isEmpty()) {
        auto o = rollOptions();
        o.afterFenceForTest = [&] {
            if (Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", minute, start, start + 4 * minute).size() > 1)
                std::_Exit(73);
        };
        sentinel::roller::roll(o);
        std::_Exit(74);
    }
    RollupStore store(storeOptions());
    ASSERT_EQ(store.runPass().built, 2u);
    Hmc2Reader reader(root() / "hmc2");
    std::map<std::pair<std::string, int64_t>, std::vector<uint8_t>> old;
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day}) {
            const ChunkKey key{"BTC-USD", std::string("hmc2.") + layer, level, chunkStartOf(level, start)};
            old[{layer, level}] = *store.serve(key, reader).wire; // warm the wire cache too
        }
    auto o = rollOptions();
    // Re-exec into a fresh process (no post-fork Qt/recorder locks). Abrupt
    // exit leaves no destructor drain or cleanup to close the crash window.
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SENTINEL_ROLLUP_CRASH_ROOT", QString::fromStdString(root().string()));
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
                {"--gtest_filter=RollerRollupTest.CrashAfterRewriteBeforeDeleteNeverServesStaleRollups"});
    ASSERT_TRUE(child.waitForStarted(10000));
    ASSERT_TRUE(child.waitForFinished(60000));
    ASSERT_EQ(child.exitStatus(), QProcess::NormalExit) << child.readAllStandardError().toStdString();
    ASSERT_EQ(child.exitCode(), 73) << child.readAllStandardError().toStdString();
    // No reconciliation has run since the crash. The very next server reads
    // must compose the new minutes, even though the stale .shr files survive.
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day}) {
            ASSERT_TRUE(fs::exists(store.filePath("BTC-USD", layer, level, start)));
            const ChunkKey key{"BTC-USD", std::string("hmc2.") + layer, level, chunkStartOf(level, start)};
            const auto reply = store.serve(key, reader);
            ASSERT_TRUE(reply.wire) << reply.message;
            EXPECT_TRUE(reply.fallback);
            EXPECT_NE(*reply.wire, (old[{layer, level}]));
            ReadControl control;
            const auto expected = composeDay(reader, "BTC-USD", layer, start, control);
            if (level == hour) EXPECT_EQ(*reply.wire, encodeHourFrame("BTC-USD", expected, start));
            else {
                const auto actual = decodeChunk(*reply.wire).columns;
                ASSERT_EQ(actual.columns.size(), expected.day.columns.size());
                EXPECT_EQ(actual.columns.front().native.front().observedMs,
                          expected.day.columns.front().native.front().observedMs);
            }
        }
    EXPECT_EQ(store.runPass().built, 2u);
    o.afterFenceForTest = {};
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_EQ(store.runPass().failed, 0u);
    RollupStore restarted(storeOptions());
    EXPECT_EQ(restarted.runPass().built, 0u);
    EXPECT_FALSE(restarted.serve({"BTC-USD", "hmc2.near", hour, start}, reader).fallback);
}

TEST_F(RollerRollupTest, CliBackfillUsesConfiguredRollupDirectory) {
    auto opts = storeOptions();
    opts.root = root() / "custom-rollups";
    RollupStore store(opts);
    ASSERT_EQ(store.runPass().built, 2u);
    const auto previous = fs::current_path();
    struct Restore { fs::path path; ~Restore() { fs::current_path(path); } } restore{previous};
    fs::create_directories(root() / "config");
    std::ofstream(root() / "config" / "server_config.yaml")
        << "recording:\n  source: roller\n  dir: '" << (root() / "unused-primary").string()
        << "'\n  rollup_dir: '" << opts.root.string()
        << "'\nroller_shadow:\n  dir: '" << opts.hmc2Root.string() << "'\n";
    fs::current_path(root());
    std::vector<std::string> args{"sentinel-roll", (root() / "raw").string(), opts.hmc2Root.string(),
        "--products", "BTC-USD", "--from", "2020-01-01", "--to", "2020-01-01T00:04:00Z",
        "--product-lease", "--no-anchors"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    ASSERT_EQ(sentinel::roller::rollMain(int(argv.size()), argv.data()), 0);
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day})
            EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", layer, level, start)));
    EXPECT_EQ(store.runPass().built, 2u);
}

TEST_F(RollerRollupTest, CliScratchOutputNeverDeletesConfiguredServedRollups) {
    RollupStore served(storeOptions());
    ASSERT_EQ(served.runPass().built, 2u);
    auto scratchPrefix = rollOptions();
    scratchPrefix.outputRoot = root() / "scratch-hmc2";
    scratchPrefix.toMs = start + minute;
    sentinel::roller::roll(scratchPrefix);
    auto scratchOptions = storeOptions();
    scratchOptions.hmc2Root = scratchPrefix.outputRoot;
    scratchOptions.root = rollupRootFor("", scratchOptions.hmc2Root);
    RollupStore scratch(scratchOptions);
    ASSERT_EQ(scratch.runPass().built, 2u);
    const auto previous = fs::current_path();
    struct Restore { fs::path path; ~Restore() { fs::current_path(path); } } restore{previous};
    fs::create_directories(root() / "config");
    const auto config = root() / "repair.yaml";
    std::ofstream(config)
        << "recording:\n  source: roller\n  dir: '" << (root() / "unused-primary").string()
        << "'\n  rollup_dir: '" << served.root().string()
        << "'\nroller_shadow:\n  dir: '" << (root() / "hmc2").string() << "'\n";
    fs::current_path(root());
    std::vector<std::string> args{"sentinel-roll", (root() / "raw").string(), scratchPrefix.outputRoot.string(),
        "--products", "BTC-USD", "--from", "2020-01-01", "--to", "2020-01-01T00:04:00Z",
        "--config", config.string(), "--product-lease", "--no-anchors"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    ASSERT_EQ(sentinel::roller::rollMain(int(argv.size()), argv.data()), 0);
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day}) {
            EXPECT_TRUE(fs::exists(served.filePath("BTC-USD", layer, level, start)));
            EXPECT_FALSE(fs::exists(scratch.filePath("BTC-USD", layer, level, start)));
        }
}

TEST_F(RollerRollupTest, LinkedRollupDirectoryFailsWithoutAdvancingCheckpointAndRetries) {
    RollupStore store(storeOptions());
    ASSERT_EQ(store.runPass().built, 2u);
    const auto near = store.root() / "BTC-USD" / "near";
    const auto outside = root() / "outside";
    fs::rename(near, outside);
    fs::create_directory_symlink(outside, near);
    const auto checkpoint = root() / "hmc2" / "BTC-USD" / "roller.json";
    const auto bytes = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto before = bytes(checkpoint);
    auto o = rollOptions(); // offline remains fail-closed
    EXPECT_THROW(sentinel::roller::roll(o), std::runtime_error);
    EXPECT_EQ(bytes(checkpoint), before);
    EXPECT_TRUE(fs::exists(outside / "3600000" / "2020-01-01.shr"));
    EXPECT_TRUE(fs::exists(outside / "86400000" / "2020-01-01.shr"));
    fs::remove(near);
    fs::rename(outside, near);
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "near", hour, start)));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "deep", day, start)));
}

TEST_F(RollerRollupTest, LiveLinkedDirectoryKeepsCheckpointAndComposesFreshMinutes) {
    RollupStore store(storeOptions());
    ASSERT_EQ(store.runPass().built, 2u);
    Hmc2Reader reader(root() / "hmc2");
    const ChunkKey key{"BTC-USD", "hmc2.near", hour, start};
    const auto old = store.serve(key, reader);
    ASSERT_TRUE(old.wire);
    const auto near = store.root() / "BTC-USD" / "near";
    const auto outside = root() / "outside";
    fs::rename(near, outside);
    fs::create_directory_symlink(outside, near);
    const auto checkpoint = root() / "hmc2" / "BTC-USD" / "roller.json";
    const auto bytes = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto before = bytes(checkpoint);
    sentinel::metrics::MetricsRegistry registry;
    auto& failures = registry.counter("sentinel_roller_rollup_invalidation_failures_total", "", {{"product", "BTC-USD"}});
    auto o = rollOptions();
    o.productWriterLease = true;
    o.nextRecord = [](sentinel::roller::JournalReader& source, sentinel::roller::JournalRecord& input) {
        return source.next(input);
    };
    o.rollupInvalidationBestEffort = true;
    o.rollupInvalidationFailuresCounter = &failures;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_NE(bytes(checkpoint), before);
    EXPECT_GT(failures.value(), 0u);
    EXPECT_NE(registry.render().find("sentinel_roller_rollup_invalidation_failures_total{product=\"BTC-USD\"} "), std::string::npos);
    EXPECT_TRUE(fs::exists(outside / "3600000" / "2020-01-01.shr"));
    const auto reply = store.serve(key, reader);
    ASSERT_TRUE(reply.wire);
    EXPECT_TRUE(reply.fallback);
    EXPECT_NE(*reply.wire, *old.wire);
    fs::remove(near);
    fs::rename(outside, near);
    EXPECT_EQ(store.runPass().built, 2u);
}

TEST_F(RollerRollupTest, RefusedRollupRootMeansNoDerivedStore) {
    auto o = rollOptions();
    o.rollupRoot = o.outputRoot;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_EQ(Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", minute, start, start + 4 * minute).size(), 4u);
}

TEST_F(RollerRollupTest, ShadowStyleBackfillUsesCustomRootAndDryRunDoesNotDelete) {
    auto opts = storeOptions();
    opts.root = root() / "shadow-rollups";
    RollupStore store(opts);
    ASSERT_EQ(store.runPass().built, 2u);
    auto o = rollOptions();
    o.rollupRoot = opts.root;
    o.productWriterLease = true;
    o.nextRecord = [](sentinel::roller::JournalReader& reader, sentinel::roller::JournalRecord& input) {
        return reader.next(input); // ShadowRoller's durable-input path
    };
    o.dryRun = true;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_TRUE(fs::exists(store.filePath("BTC-USD", "near", hour, start)));
    o.dryRun = false;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    for (const auto* layer : {"near", "deep"})
        for (const auto level : {hour, day})
            EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", layer, level, start)));
}

TEST_F(RollerRollupTest, LateBuilderPublicationCannotResurrectStaleData) {
    auto opts = storeOptions();
    bool rewriteBeforeRename = false;
    opts.beforeRename = [&](const fs::path&, const fs::path&) {
        if (std::exchange(rewriteBeforeRename, false)) sentinel::roller::roll(rollOptions());
    };
    RollupStore store(opts);
    ASSERT_EQ(store.runPass().built, 2u);
    Hmc2Reader reader(root() / "hmc2");
    const ChunkKey key{"BTC-USD", "hmc2.near", hour, start};
    const auto old = store.serve(key, reader);
    ASSERT_TRUE(old.wire);
    fs::remove(store.filePath("BTC-USD", "near", hour, start));
    // The builder already checked its source, then the roller writes and
    // invalidates, then the builder publishes the old snapshot after deletion.
    rewriteBeforeRename = true;
    EXPECT_EQ(store.runPass().failed, 0u);
    ASSERT_TRUE(fs::exists(store.filePath("BTC-USD", "near", hour, start)));
    const auto reply = store.serve(key, reader);
    ASSERT_TRUE(reply.wire);
    EXPECT_TRUE(reply.fallback);
    EXPECT_NE(*reply.wire, *old.wire);
    EXPECT_EQ(store.runPass().built, 1u);
    EXPECT_EQ(*store.serve(key, reader).wire, *reply.wire);
}

// One unusable level does not stop the other three from being invalidated; the
// failure is reported (logged) once, not once per level or per fence.
TEST_F(RollerRollupTest, InvalidationTriesEveryLevelAndReportsOnce) {
    RollupStore store(storeOptions());
    ASSERT_EQ(store.runPass().built, 2u);
    const auto nearHour = store.root() / "BTC-USD" / "near" / std::to_string(hour);
    const auto outside = root() / "outside";
    fs::rename(nearHour, outside);
    fs::create_directory_symlink(outside, nearHour); // the first (layer, level) fails
    sentinel::metrics::MetricsRegistry registry;
    auto& failures = registry.counter("sentinel_roller_rollup_invalidation_failures_total", "", {{"product", "BTC-USD"}});
    auto o = rollOptions();
    o.productWriterLease = true;
    o.nextRecord = [](sentinel::roller::JournalReader& source, sentinel::roller::JournalRecord& input) {
        return source.next(input);
    };
    o.rollupInvalidationBestEffort = true;
    o.rollupInvalidationFailuresCounter = &failures;
    Logs logs;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_GT(failures.value(), 0u);
    EXPECT_TRUE(fs::exists(outside / "2020-01-01.shr")) << "never through the link";
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "near", day, start)));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "deep", hour, start)));
    EXPECT_FALSE(fs::exists(store.filePath("BTC-USD", "deep", day, start)));
    EXPECT_EQ(logs.count("Roller rollup invalidation failed"), 1u);
    EXPECT_TRUE(logs.first("Roller rollup invalidation failed").contains("1 of 4 rollup levels failed"))
        << logs.first("Roller rollup invalidation failed").toStdString();
}

TEST_F(RollerRollupTest, RefusedRollupRootIsLoggedOnce) {
    auto o = rollOptions();
    o.rollupRoot = o.outputRoot;
    Logs logs;
    EXPECT_NO_THROW(sentinel::roller::roll(o));
    EXPECT_EQ(logs.count("Roller skips rollup invalidation: refused rollup root"), 1u);
}
