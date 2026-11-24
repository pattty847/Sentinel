// Synthetic durable RAWL2 fixtures exercise disk recovery, canonical seals and
// epoch fencing. Never opens the recorder's roots or starts a server/service.
#include "servermodel/FlowChunkStore.hpp"
#include "roller/fixtures/FlowJournal.hpp"
#include "roller/JournalReader.hpp"
#include "flow/FlowCompose.hpp"
#include "ConfigLoader.hpp"
#include "Crc32.hpp"
#include "heatmap/ChunkCodec.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <gtest/gtest.h>
#include <fstream>
#include <future>
using namespace flow_fixture;
namespace fs = std::filesystem;
namespace c = sentinel::flow::chunks;
using recording::FlowStore;
using recording::FlowChunkStore;
namespace {
constexpr int64_t Day = 24 * Hour;
const std::string Id = "coinbase:BTC-USD";
class ChunkStoreTest : public testing::Test {
protected:
    QTemporaryDir temp;
    fs::path base = temp.path().toStdString(), raw = base / "raw", hmc = base / "hmc", root = base / "flc";
    sentinel::metrics::MetricsRegistry metrics;
    int catalogs = 0;
    void SetUp() override { fs::create_directory(raw); fs::create_directory(hmc); }
    FlowStore::Options sourceOptions() {
        FlowStore::Options o;
        o.journalRoot = raw; o.hmc2Root = hmc; o.root = base / "fld";
        o.symbols = {"BTC-USD", "ETH-USD"};
        o.nowMs = [] { return Epoch + 30 * Day; };
        o.hourThrough = [](const std::string &) { return Epoch + Day; };
        o.afterCatalogConstructForTest = [&] { ++catalogs; };
        o.lowerPriority = false;
        return o;
    }
    FlowChunkStore::Options options() {
        FlowChunkStore::Options o; o.root = root; o.lowerPriority = false; o.metrics = &metrics;
        o.rules.sealWindowMs = 15000; // These synthetic journals place their seal records at W = 15 s.
        return o;
    }
    void fixture(int days = 1, const std::string &symbol = "BTC-USD") {
        Journal j(raw, symbol);
        j.snapshot(-Minute);
        for (int d = 0; d < days; ++d) for (int h = 0; h < 24; ++h) {
            const auto t = d * Day + h * Hour;
            if (h == 0) j.snapshot(t);
            j.trades(t + 1000, {trade(t + 100, std::to_string(1 + d * 24 + h), 100000 + h, "BUY", symbol)});
            j.heartbeat(t + Hour + 15000);
        }
        j.close();
    }
    std::vector<uint8_t> readBytes(const fs::path &p) {
        std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
    }
    void rollerOrderByFirstReceive() {
        sentinel::roller::JournalReader catalog(raw, "BTC-USD");
        std::map<std::string, int64_t> starts;
        for (const auto &file : catalog.files()) starts.try_emplace(file.run, file.openedNs);
        for (const auto &file : catalog.files()) {
            auto b = readBytes(file.path);
            uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length |= uint32_t(b[8 + i]) << (8 * i);
            auto header = nlohmann::json::parse(b.begin() + 16, b.begin() + 16 + length);
            header["run_started_system_ns"] = starts.at(file.run);
            const auto text = header.dump(); ASSERT_EQ(text.size(), length);
            std::copy(text.begin(), text.end(), b.begin() + 16);
            const auto crc = sentinel::crc32(text.data(), text.size());
            for (unsigned i = 0; i < 4; ++i) b[12 + i] = uint8_t(crc >> (8 * i));
            std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(b.data()), b.size());
        }
    }
    c::DayFile day(FlowChunkStore &store, int64_t start = Epoch) { return c::parseDayFile(readBytes(store.filePath(Id, start))); }
};
TEST_F(ChunkStoreTest, SealMembershipAndFirstSealRecord) {
    Journal j(raw); j.snapshot(-Minute);
    j.trades(Minute + 14999, {trade(100, "1", 100000)});
    j.trades(Minute + 15000, {trade(200, "2", 100001)});
    j.trades(Minute + 15001, {trade(300, "3", 100002)});
    j.trades(Hour + 15000, {trade(Hour - 100, "4", 100003)}); j.close();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto data = day(store);
    EXPECT_EQ(data.lateTrades, 3u); EXPECT_TRUE(data.sections[0].sealed); EXPECT_FALSE(data.sections[1].sealed);
    EXPECT_FALSE(data.sections[24].sealed); EXPECT_FALSE(data.sections[25].sealed);
    const auto payload = store.read(Id, Minute, Epoch); ASSERT_TRUE(payload);
    const auto bars = c::toBars(c::decode(payload->bytes, Minute, Epoch, 60, false, uint64_t(1) << payload->stepExponent));
    ASSERT_EQ(bars.size(), 1u); EXPECT_EQ(bars[0].count, 1u);
    EXPECT_NE(metrics.render().find("sentinel_flow_late_trades_total{instrument=\"coinbase:BTC-USD\"} 3"), std::string::npos);
    EXPECT_FALSE(store.read(Id, Minute, Epoch + Hour));
}
TEST_F(ChunkStoreTest, SealMustBeReachedByDurableRecord) {
    Journal j(raw); j.snapshot(-Minute); j.trades(1000, {trade(500, "1", 100000)}); j.heartbeat(Hour + 14999); j.close();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_FALSE(store.read(Id, Minute, Epoch));
    EXPECT_FALSE(fs::exists(store.filePath(Id, Epoch)));
}
TEST_F(ChunkStoreTest, WatermarkStaysAtMaximumAfterReceiveClockStepsBack) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(Minute + 15000, {trade(100, "1", 100000)});
    journal.trades(Minute + 10000, {trade(200, "2", 100001)}); // clock steps back, M stays at seal
    journal.heartbeat(Hour + 15000); journal.close();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto file = day(store);
    EXPECT_EQ(file.lateTrades, 2u);
    EXPECT_EQ(file.regressions, 1u);
    auto payload = store.read(Id, Minute, Epoch); ASSERT_TRUE(payload);
    EXPECT_TRUE(c::toBars(c::decode(payload->bytes, Minute, Epoch, 60, false,
        uint64_t(1) << payload->stepExponent)).empty());
}
TEST_F(ChunkStoreTest, ReorderedRunsStartAtFirstEligibleRecordInReaderOrder) {
    { Journal first(raw); first.snapshot(-30 * Minute);
      first.trades(-30000, {trade(100, "first", 100000)}); first.close(); }
    { Journal second(raw); second.snapshot(-20 * Minute);
      second.trades(-20000, {trade(200, "second", 100001)});
      second.heartbeat(Hour + 15000); second.close(); }
    rollerOrderByFirstReceive();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    auto payload = store.read(Id, Minute, Epoch); ASSERT_TRUE(payload);
    const auto bars = c::toBars(c::decode(payload->bytes, Minute, Epoch, 60, false,
        uint64_t(1) << payload->stepExponent));
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars.front().count, 2u); // last-opened-before-warmup loses "first"
}
TEST_F(ChunkStoreTest, FiveMinuteSealWindowIncludesTheLeadInAndSealsAtEquality) {
    EXPECT_EQ(FlowChunkStore::Options{}.rules.sealWindowMs, 300000u);
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(Minute + 299999, {trade(100, "1", 100000)});
    journal.trades(Minute + 300000, {trade(200, "2", 100001)});
    journal.heartbeat(Hour + 299999); journal.writer.flush();
    FlowStore source(sourceOptions()); auto o = options(); o.rules.sealWindowMs = 300000;
    FlowChunkStore store(source, o); EXPECT_FALSE(store.read(Id, Minute, Epoch));
    EXPECT_EQ(store.runPass().failed, 0u); EXPECT_FALSE(store.read(Id, Minute, Epoch));
    journal.heartbeat(Hour + 300000); journal.close();
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto file = day(store); EXPECT_EQ(file.lateTrades, 1u);
    auto payload = store.read(Id, Minute, Epoch); ASSERT_TRUE(payload);
    const auto bars = c::toBars(c::decode(payload->bytes, Minute, Epoch, 60, false,
        uint64_t(1) << payload->stepExponent));
    ASSERT_EQ(bars.size(), 1u); EXPECT_EQ(bars.front().count, 1u);
}
TEST_F(ChunkStoreTest, ResentAcceptedTradeDoesNotCountAsLate) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(1000, {trade(100, "1", 100000)});
    for (int i = 0; i < 17000; ++i)
        journal.trades(2000 + i, {trade(200 + i, std::to_string(i + 2), 100000)});
    journal.trades(Minute + 15000, {trade(100, "1", 100000)}, true);
    journal.heartbeat(Hour + 15000); journal.close();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(day(store).lateTrades, 0u);
}
TEST_F(ChunkStoreTest, CatalogIsSharedAndFldBytesUnchanged) {
    fixture(); FlowStore source(sourceOptions());
    ASSERT_EQ(source.runPass().failed, 0u);
    const auto before = readBytes(source.filePath("BTC-USD", Epoch));
    const auto archive = source.archive("BTC-USD"); const auto count = catalogs;
    FlowChunkStore store(source, options()); ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(catalogs, count); EXPECT_EQ(archive.catalog.get(), source.archive("BTC-USD").catalog.get());
    EXPECT_EQ(readBytes(source.filePath("BTC-USD", Epoch)), before);
    ASSERT_EQ(source.runPass().failed, 0u); EXPECT_EQ(readBytes(source.filePath("BTC-USD", Epoch)), before);
}
TEST_F(ChunkStoreTest, DeterministicIndependentReverseAndDayPassWithClockSteps) {
    // A later-created run starts earlier and sorts before the first run.
    { Journal j(raw); j.snapshot(3 * Hour); j.trades(3 * Hour + 100, {trade(3 * Hour, "50", 100005)});
      j.heartbeat(4 * Hour + 15000); j.close(); }
    { Journal j(raw); j.snapshot(-Minute); j.trades(1000, {trade(100, "1", 100000)});
      j.heartbeat(2000); j.trades(1500, {trade(200, "2", 100001)}); // within-hour regression
      j.heartbeat(Hour + 15000); j.trades(Hour + 1000, {trade(Hour + 100, "3", 100002)});
      j.heartbeat(2 * Hour + 15000); j.close(); }
    // Writer stamps run start from the host clock. Set fixture headers to the
    // simulated receive clock so the restarted run genuinely sorts earlier.
    rollerOrderByFirstReceive();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    auto expected = day(store), forward = expected, reverse = expected;
    const auto archive = source.archive("BTC-USD"); const auto grid = source.gridFor("BTC-USD", Epoch, *archive.catalog);
    for (auto *target : {&forward, &reverse}) {
        target->lateTrades = target->rejectedTrades = target->regressions = 0;
        for (int n = 0; n < 24; ++n) {
            const int h = target == &forward ? n : 23 - n;
            auto result = store.buildHour("BTC-USD", Epoch + h * Hour, grid, *archive.catalog);
            target->sections[h] = result.section;
            target->lateTrades += result.late; target->rejectedTrades += result.rejected; target->regressions += result.regressions;
        }
    }
    EXPECT_GT(expected.regressions, 0u);
    EXPECT_EQ(c::encodeDayFile(expected), c::encodeDayFile(forward));
    EXPECT_EQ(c::encodeDayFile(expected), c::encodeDayFile(reverse));
    // The durable locator cache must agree with independent full hour scans,
    // including the reordered run and receive-clock regression.
    const auto cached = nlohmann::json::parse(readBytes(store.filePath(Id, Epoch).string() + ".loc"));
    for (int h = 0; h < 4; ++h) {
        const auto full = store.buildHour("BTC-USD", Epoch + h * Hour, grid, *archive.catalog);
        if (!full.section.sealed) continue;
        ASSERT_TRUE(full.source);
        const auto &entry = cached.at("hours").at(h);
        EXPECT_EQ(entry.at("first").get<sentinel::roller::JournalPos>(), full.source->first);
        EXPECT_EQ(entry.at("last").get<sentinel::roller::JournalPos>(), full.source->last);
    }
    auto restartOptions = options(); size_t located = 0;
    restartOptions.afterLocateForTest = [&](int64_t) { ++located; };
    FlowChunkStore restarted(source, restartOptions);
    ASSERT_EQ(restarted.runPass().failed, 0u);
    EXPECT_EQ(located, 0u);
    EXPECT_EQ(readBytes(restarted.filePath(Id, Epoch)), c::encodeDayFile(expected));
    fs::remove(store.filePath(Id, Epoch).string() + ".loc");
    located = 0;
    std::map<int64_t, FlowChunkStore::SourceCheck> found;
    restartOptions.afterLocatedForTest = [&](int64_t hour, const auto &sourceCheck) {
        found.emplace(hour, sourceCheck);
    };
    FlowChunkStore withoutCache(source, restartOptions);
    const auto unchangedEpoch = restarted.epoch(Id);
    ASSERT_EQ(withoutCache.runPass().failed, 0u);
    EXPECT_GT(located, 0u);
    EXPECT_EQ(withoutCache.epoch(Id), unchangedEpoch);
    EXPECT_EQ(readBytes(withoutCache.filePath(Id, Epoch)), c::encodeDayFile(expected));
    for (int h = 0; h < 4; ++h) {
        const auto full = store.buildHour("BTC-USD", Epoch + h * Hour, grid, *archive.catalog);
        if (!full.section.sealed) continue;
        ASSERT_TRUE(found.contains(Epoch + h * Hour));
        ASSERT_TRUE(full.source);
        EXPECT_EQ(found.at(Epoch + h * Hour).first, full.source->first);
        EXPECT_EQ(found.at(Epoch + h * Hour).last, full.source->last);
    }
}
TEST_F(ChunkStoreTest, CoarseEntriesComposeOnlyWhenAllHoursSealedAndAssembleDays) {
    fixture(); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); const auto file = day(store);
    ASSERT_TRUE(file.sections[24].sealed); ASSERT_TRUE(file.sections[25].sealed);
    std::vector<sentinel::flow::FlowBar> minutes, hours;
    for (int h = 0; h < 24; ++h) {
        const auto &s = file.sections[h]; auto bars = c::toBars(c::decode(s.payload, Minute, Epoch + h * Hour, 60, false, uint64_t(1) << s.stepExponent));
        if (!bars.empty()) {
            auto composed = sentinel::flow::composeFlowBars(bars, Hour); composed.notable.clear();
            hours.push_back(std::move(composed));
        }
        minutes.insert(minutes.end(), bars.begin(), bars.end());
    }
    const auto &hourSection = file.sections[24];
    EXPECT_EQ(c::toBars(c::decode(hourSection.payload, Hour, Epoch, 24, false, uint64_t(1) << hourSection.stepExponent)), hours);
    auto expected = sentinel::flow::composeFlowBars(minutes, Day); expected.notable.clear();
    const auto &piece = file.sections[25];
    EXPECT_EQ(c::toBars(c::decode(piece.payload, Day, Epoch, 1, false, uint64_t(1) << piece.stepExponent)).front(), expected);
    for (int i = 26; i < 50; ++i) EXPECT_FALSE(file.sections[i].sealed);
}
TEST_F(ChunkStoreTest, CorruptOrLostFileRebuildsIdenticallyWithoutEpochChange) {
    fixture(); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); const auto path = store.filePath(Id, Epoch);
    const auto original = readBytes(path); const auto epoch = store.epoch(Id);
    { std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out); f.seekp(100); f.put(char(original[100] ^ 1)); }
    EXPECT_FALSE(store.read(Id, Minute, Epoch)); const auto ranges = store.sealed(Id, Hour);
    EXPECT_FALSE(std::any_of(ranges.ranges.begin(), ranges.ranges.end(), [](const auto &r) { return r.fromMs <= Epoch && Epoch < r.toMs; }));
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(readBytes(path), original); EXPECT_EQ(store.epoch(Id), epoch);
    fs::remove(path); EXPECT_FALSE(store.read(Id, Minute, Epoch));
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(readBytes(path), original); EXPECT_EQ(store.epoch(Id), epoch);
}
TEST_F(ChunkStoreTest, ChangedSourceMovesOnlyItsEpoch) {
    fixture(); fixture(1, "ETH-USD"); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); const auto btc = store.epoch(Id), eth = store.epoch("coinbase:ETH-USD");
    const auto archive = source.archive("BTC-USD"); const auto path = archive.catalog->files().front().path;
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_NE(store.epoch(Id), btc); EXPECT_EQ(store.epoch("coinbase:ETH-USD"), eth);
}
TEST_F(ChunkStoreTest, FailedEpochWriteRetriesTransitionBeforePublishing) {
    fixture(); FlowStore source(sourceOptions()); auto o = options();
    bool fail = false;
    o.beforeEpochWriteForTest = [&] { if (std::exchange(fail, false)) throw std::runtime_error("injected epoch write failure"); };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    const auto before = store.epoch(Id);
    const auto path = source.archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    fail = true;
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_NE(store.epoch(Id), before);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
}
TEST_F(ChunkStoreTest, ReadDiscardsBytesWhenEpochMovesAfterDiskRead) {
    fixture(); FlowStore source(sourceOptions()); auto o = options();
    FlowChunkStore *active = nullptr; bool changed = false;
    o.afterSectionReadForTest = [&] {
        if (changed) return;
        changed = true;
        const auto path = source.archive("BTC-USD").catalog->files().front().path;
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
        EXPECT_EQ(active->runPass().failed, 0u);
    };
    FlowChunkStore store(source, o); active = &store;
    ASSERT_EQ(store.runPass().failed, 0u); const auto before = store.epoch(Id);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
    EXPECT_NE(store.epoch(Id), before);
}
TEST_F(ChunkStoreTest, FullySealedRestartDoesNotFeedTrades) {
    fixture(); FlowStore source(sourceOptions());
    uint64_t first;
    { FlowChunkStore store(source, options()); ASSERT_EQ(store.runPass().failed, 0u); first = store.epoch(Id); }
    auto o = options(); size_t replayed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    FlowChunkStore restarted(source, o);
    ASSERT_EQ(restarted.runPass().failed, 0u);
    EXPECT_EQ(replayed, 0u);
    EXPECT_EQ(restarted.epoch(Id), first);
    EXPECT_TRUE(restarted.read(Id, Minute, Epoch));
}
TEST_F(ChunkStoreTest, RestartUsesDayBoundLocatorsWithoutFullHourScans) {
    fixture(2); FlowStore source(sourceOptions());
    { FlowChunkStore first(source, options()); ASSERT_EQ(first.runPass().failed, 0u); }
    auto o = options(); size_t located = 0, fed = 0;
    o.afterLocateForTest = [&](int64_t) { ++located; };
    o.afterRecordForTest = [&](std::stop_token) { ++fed; };
    FlowChunkStore restarted(source, o);
    ASSERT_EQ(restarted.runPass().failed, 0u);
    EXPECT_EQ(located, 0u); EXPECT_EQ(fed, 0u);
    EXPECT_TRUE(restarted.read(Id, Minute, Epoch));
    EXPECT_TRUE(restarted.read(Id, Minute, Epoch + Day));
}
TEST_F(ChunkStoreTest, MissingLocatorOnOpenSealFileRequiresHourReplay) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(1000, {trade(100, "1", 100000)});
    journal.heartbeat(Hour + 15000); journal.writer.flush();
    FlowStore source(sourceOptions());
    { FlowChunkStore first(source, options()); ASSERT_EQ(first.runPass().failed, 0u);
      fs::remove(first.filePath(Id, Epoch).string() + ".loc"); }
    auto o = options(); size_t fed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++fed; };
    FlowChunkStore restarted(source, o);
    ASSERT_EQ(restarted.runPass().failed, 0u);
    EXPECT_GT(fed, 0u);
    EXPECT_TRUE(restarted.read(Id, Minute, Epoch));
    journal.close();
}
TEST_F(ChunkStoreTest, AvailabilityDiscardsRangesWhenEpochChangesDuringEnumeration) {
    fixture(); FlowStore source(sourceOptions()); auto o = options();
    FlowChunkStore *active = nullptr; bool changed = false;
    o.afterAvailabilityReadForTest = [&] {
        if (changed) return;
        changed = true;
        const auto path = source.archive("BTC-USD").catalog->files().front().path;
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
        EXPECT_EQ(active->runPass().failed, 0u);
    };
    FlowChunkStore store(source, o); active = &store;
    ASSERT_EQ(store.runPass().failed, 0u); const auto epoch = store.epoch(Id);
    EXPECT_TRUE(store.sealed(Id, Hour).ranges.empty());
    EXPECT_NE(store.epoch(Id), epoch);
    const auto current = store.sealed(Id, Hour);
    EXPECT_EQ(current.epoch, store.epoch(Id));
    EXPECT_FALSE(current.ranges.empty());
}
TEST_F(ChunkStoreTest, RemovedArchiveFencesOldSectionsUntilSourcesReturn) {
    fixture(); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); const auto epoch = store.epoch(Id);
    const auto original = readBytes(store.filePath(Id, Epoch));
    struct Backup { fs::path path; std::vector<uint8_t> bytes; fs::file_time_type modified; };
    std::vector<Backup> backup;
    for (const auto &file : source.archive("BTC-USD").catalog->files()) {
        backup.push_back({file.path, readBytes(file.path), fs::last_write_time(file.path)});
        fs::remove(file.path); // synthetic fixture only
    }
    EXPECT_GT(store.runPass().failed, 0u); EXPECT_NE(store.epoch(Id), epoch);
    EXPECT_FALSE(store.read(Id, Minute, Epoch)); EXPECT_TRUE(store.sealed(Id, Hour).ranges.empty());
    for (const auto &b : backup) {
        { std::ofstream f(b.path, std::ios::binary); f.write(reinterpret_cast<const char *>(b.bytes.data()), b.bytes.size()); }
        fs::last_write_time(b.path, b.modified);
    }
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_TRUE(store.read(Id, Minute, Epoch));
    EXPECT_EQ(readBytes(store.filePath(Id, Epoch)), original);
}
TEST_F(ChunkStoreTest, RuleChangeFencesAllDaysAndCrashResumeAdvancesAgain) {
    fixture(2); fixture(1, "ETH-USD"); FlowStore source(sourceOptions());
    uint64_t btc, eth, interrupted;
    { FlowChunkStore original(source, options()); ASSERT_EQ(original.runPass().failed, 0u);
      btc = original.epoch(Id); eth = original.epoch("coinbase:ETH-USD"); }
    auto o = options(); o.bigFloors[Id] = 42;
    std::stop_source stop; std::vector<int64_t> order;
    o.afterDayForTest = [&](int64_t day) { order.push_back(day); stop.request_stop(); };
    { FlowChunkStore next(source, o); next.initialize();
      EXPECT_NE(next.epoch(Id), btc); EXPECT_EQ(next.epoch("coinbase:ETH-USD"), eth);
      EXPECT_FALSE(next.read(Id, Minute, Epoch)); EXPECT_FALSE(next.read(Id, Minute, Epoch + Day));
      EXPECT_TRUE(next.read("coinbase:ETH-USD", Minute, Epoch));
      EXPECT_TRUE(next.runPass(stop.get_token()).stopped); interrupted = next.epoch(Id); }
    ASSERT_FALSE(order.empty()); EXPECT_GE(order.front(), Epoch + Day);
    o.afterDayForTest = {};
    FlowChunkStore resumed(source, o); resumed.initialize(); EXPECT_NE(resumed.epoch(Id), interrupted);
    ASSERT_EQ(resumed.runPass().failed, 0u); EXPECT_TRUE(resumed.read(Id, Minute, Epoch));
    EXPECT_EQ(resumed.epoch("coinbase:ETH-USD"), eth);
}
TEST_F(ChunkStoreTest, ChangedSealWindowCompletesExistingEpochTransition) {
    fixture(); FlowStore source(sourceOptions());
    { FlowChunkStore original(source, options()); ASSERT_EQ(original.runPass().failed, 0u); }
    auto changed = options(); changed.rules.sealWindowMs = 20000;
    FlowChunkStore store(source, changed); store.initialize();
    const auto epoch = store.epoch(Id);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(store.epoch(Id), epoch);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
    EXPECT_FALSE(store.read(Id, Minute, Epoch + 23 * Hour));
}
TEST_F(ChunkStoreTest, StopsDuringReplayAndResumesFromPendingEpoch) {
    fixture(); FlowStore source(sourceOptions()); auto o = options();
    std::promise<void> entered; auto future = entered.get_future(); std::atomic<bool> once{false};
    o.afterRecordForTest = [&](std::stop_token stop) {
        if (once.exchange(true)) return;
        entered.set_value(); std::mutex m; std::condition_variable_any cv; std::unique_lock lock(m);
        cv.wait(lock, stop, [] { return false; });
    };
    { FlowChunkStore store(source, o); store.start(); ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready); store.stop(); }
    FlowChunkStore resumed(source, options()); ASSERT_EQ(resumed.runPass().failed, 0u); EXPECT_TRUE(resumed.read(Id, Minute, Epoch));
}
TEST_F(ChunkStoreTest, RefusesSourceAndFldRootOverlap) {
    FlowStore source(sourceOptions());
    for (const auto &path : {raw, hmc, source.root(), source.root() / "flc", base}) {
        auto o = options(); o.root = path; EXPECT_THROW(FlowChunkStore(source, o), std::invalid_argument);
    }
}
TEST_F(ChunkStoreTest, RefusesSymlinkRootAndCleansOnlyOwnedStaleTemps) {
    fixture(); FlowStore source(sourceOptions());
    fs::create_directories(root / "coinbase" / "BTC-USD");
    const auto directory = root / "coinbase" / "BTC-USD";
    { std::ofstream(directory / "epoch.A1b2C3") << "stale";
      std::ofstream(directory / "2027-01-01.flc.A1b2C3") << "stale";
      std::ofstream(directory / "other.A1b2C3") << "keep"; }
    FlowChunkStore store(source, options()); ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_FALSE(fs::exists(directory / "epoch.A1b2C3"));
    EXPECT_FALSE(fs::exists(directory / "2027-01-01.flc.A1b2C3"));
    EXPECT_TRUE(fs::exists(directory / "other.A1b2C3"));
    fs::create_directory(base / "real-root"); fs::create_directory_symlink(base / "real-root", base / "link-root");
    auto o = options(); o.root = base / "link-root";
    EXPECT_THROW(FlowChunkStore forbidden(source, o), std::invalid_argument);
}
TEST_F(ChunkStoreTest, DefaultRootWithTrailingSlashIsSiblingOfHmc) {
    auto sourceConfig = sourceOptions(); sourceConfig.hmc2Root = fs::path(hmc.string() + "/");
    FlowStore source(sourceConfig); auto o = options(); o.root.clear();
    FlowChunkStore store(source, o);
    EXPECT_EQ(store.filePath(Id, Epoch).parent_path(),
              fs::path(fs::weakly_canonical(hmc).string() + "-flc/coinbase/BTC-USD"));
}
TEST_F(ChunkStoreTest, ConfigFlatWrappedDefaultsAndInvalidWindow) {
    ServerConfig cfg;
    EXPECT_TRUE(cfg.recording.flowChunkDir.empty()); EXPECT_EQ(cfg.flow.sealWindowMs, 300000u);
    const auto path = base / "config.yaml";
    { std::ofstream f(path); f << "flow:\n  seal_window_ms: 25000\nrecording:\n  flow_chunk_dir: " << root.string() << "\n"; }
    ASSERT_TRUE(ConfigLoader::loadServerConfig(path.string(), &cfg));
    EXPECT_EQ(cfg.flow.sealWindowMs, 25000u); EXPECT_EQ(cfg.recording.flowChunkDir, root.string());
    { std::ofstream f(path); f << "server:\n  flow:\n    seal_window_ms: 15000\n"; }
    ASSERT_TRUE(ConfigLoader::loadServerConfig(path.string(), &cfg)); EXPECT_EQ(cfg.flow.sealWindowMs, 15000u);
    { std::ofstream f(path); f << "flow:\n  seal_window_ms: -1\n"; }
    EXPECT_FALSE(ConfigLoader::loadServerConfig(path.string(), &cfg));
}
TEST_F(ChunkStoreTest, SixteenDayReadRequiresEveryRecordedDayAndKeepsExactSections) {
    fixture(); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto start = heatmap::chunkStartOf(Day, Epoch);
    const auto payload = store.read(Id, Day, start);
    // The final source day contains only a receive tail, not a sealed day.
    EXPECT_FALSE(payload);
    const auto file = day(store);
    const auto hour = store.read(Id, Minute, Epoch); ASSERT_TRUE(hour);
    EXPECT_EQ(hour->bytes, file.sections[0].payload); EXPECT_EQ(hour->summaryLength, file.sections[0].summaryLength);
    EXPECT_EQ(hour->stepExponent, file.sections[0].stepExponent);
    const auto dailyHours = store.read(Id, Hour, Epoch); ASSERT_TRUE(dailyHours);
    EXPECT_EQ(dailyHours->bytes, file.sections[24].payload);
}
TEST_F(ChunkStoreTest, MissingAndUnreadableEpochFenceAndRevalidateAllDays) {
    fixture(); FlowStore source(sourceOptions()); uint64_t epoch;
    { FlowChunkStore store(source, options()); ASSERT_EQ(store.runPass().failed, 0u); epoch = store.epoch(Id); }
    const auto path = root / "coinbase" / "BTC-USD" / "epoch";
    { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << "broken"; }
    FlowChunkStore resumed(source, options()); resumed.initialize(); EXPECT_FALSE(resumed.read(Id, Minute, Epoch));
    ASSERT_EQ(resumed.runPass().failed, 0u); EXPECT_TRUE(resumed.read(Id, Minute, Epoch));
    // Recovery uses now-ms, not a reused small default counter.
    EXPECT_GE(c::parseEpochFile(readBytes(path)).counter, uint64_t(Epoch + 30 * Day));
}

TEST_F(ChunkStoreTest, SixteenDayAssemblySucceedsWithUnrecordedDays) {
    fixture(10); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto start = heatmap::chunkStartOf(Day, Epoch);
    const auto payload = store.read(Id, Day, start); ASSERT_TRUE(payload);
    const auto chunk = c::decode(payload->bytes, Day, start, 16, false, uint64_t(1) << payload->stepExponent);
    ASSERT_EQ(chunk.bars, 10u);
    for (const auto count : chunk.count) EXPECT_EQ(count, 24u);
    EXPECT_EQ(chunk.coverage.front().status, c::Status::Unrecorded);
    const auto ranges = store.sealed(Id, Day);
    EXPECT_TRUE(std::any_of(ranges.ranges.begin(), ranges.ranges.end(), [&](const auto &r) { return r.fromMs <= start && start < r.toMs; }));
}
TEST_F(ChunkStoreTest, AppendingAndClosingSealFileKeepsExactBytesAndEpoch) {
    Journal j(raw); j.snapshot(-Minute); j.trades(1000, {trade(100, "1", 100000)});
    j.heartbeat(Hour + 15000); j.writer.flush();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u); const auto original = readBytes(store.filePath(Id, Epoch)); const auto epoch = store.epoch(Id);
    j.heartbeat(Hour + 2 * Minute); j.writer.flush();
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(readBytes(store.filePath(Id, Epoch)), original); EXPECT_EQ(store.epoch(Id), epoch);
    j.close();
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(readBytes(store.filePath(Id, Epoch)), original); EXPECT_EQ(store.epoch(Id), epoch);
}
TEST_F(ChunkStoreTest, FormingDayDoesNotReplaySealedHoursUntilNextSeal) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(1000, {trade(100, "1", 100000)}); journal.heartbeat(Hour + 15000); journal.writer.flush();
    FlowStore source(sourceOptions()); auto o = options(); size_t replayed = 0, located = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    o.afterLocateForTest = [&](int64_t) { ++located; };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    ASSERT_TRUE(store.read(Id, Minute, Epoch)); EXPECT_FALSE(store.read(Id, Minute, Epoch + Hour));
    replayed = 0;
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(replayed, 0u);
    journal.trades(Hour + 20000, {trade(Hour + 100, "2", 100001)}); journal.writer.flush();
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_EQ(replayed, 0u);
    // Rotate the first hour's seal file while the next hour is still forming.
    // Its one-time close revalidation is allowed; the later seal must not
    // locate that already closed hour again.
    journal.trades(2 * Hour + 1000, {trade(2 * Hour + 100, "3", 100002)}); journal.writer.flush();
    ASSERT_EQ(store.runPass().failed, 0u);
    journal.heartbeat(2 * Hour + 15000); journal.close();
    located = 0;
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_GT(replayed, 0u);
    EXPECT_EQ(located, 0u); // the prior sealed hour has cached positions
    EXPECT_TRUE(store.read(Id, Minute, Epoch + Hour));
}
TEST_F(ChunkStoreTest, SteadyPassOverClosedDaysFeedsNoTrades) {
    fixture(3); FlowStore source(sourceOptions()); auto o = options(); size_t replayed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    replayed = 0;
    const auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    EXPECT_EQ(replayed, 0u);
    const auto rendered = metrics.render();
    EXPECT_NE(rendered.find("sentinel_flow_chunk_pending_days{instrument=\"coinbase:BTC-USD\"} 0"), std::string::npos);
    std::cout << "steady_pass_days=3 feed_records=" << replayed << " wall_ms=" << ms << '\n';
}
TEST_F(ChunkStoreTest, RestartWithMissingRootRebuildsIdenticalDay) {
    fixture(); FlowStore source(sourceOptions()); std::vector<uint8_t> original;
    { FlowChunkStore store(source, options()); ASSERT_EQ(store.runPass().failed, 0u); original = readBytes(store.filePath(Id, Epoch)); }
    fs::remove_all(root); // this test's private QTemporaryDir only
    FlowChunkStore restarted(source, options()); EXPECT_FALSE(restarted.read(Id, Minute, Epoch));
    ASSERT_EQ(restarted.runPass().failed, 0u); EXPECT_EQ(readBytes(restarted.filePath(Id, Epoch)), original);
}
TEST_F(ChunkStoreTest, RewriteDuringReplayCannotPublishMixedSourceBytes) {
    fixture(); FlowStore source(sourceOptions()); auto o = options(); bool changed = false;
    const auto archive = source.archive("BTC-USD"); const auto path = archive.catalog->files().front().path;
    o.afterRecordForTest = [&](std::stop_token) {
        if (std::exchange(changed, true)) return;
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    };
    FlowChunkStore store(source, o);
    EXPECT_GT(store.runPass().failed, 0u); EXPECT_FALSE(store.read(Id, Minute, Epoch));
    ASSERT_EQ(store.runPass().failed, 0u); EXPECT_TRUE(store.read(Id, Minute, Epoch));
}
TEST_F(ChunkStoreTest, LaterJournalRotationDoesNotInvalidateEarlierDayBuild) {
    fixture(); FlowStore source(sourceOptions()); auto o = options(); bool rotated = false;
    o.afterRecordForTest = [&](std::stop_token) {
        if (std::exchange(rotated, true)) return;
        Journal later(raw); later.snapshot(20 * Day); later.close();
    };
    FlowChunkStore store(source, o);
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
}
TEST_F(ChunkStoreTest, DamagedHourLeavesHealthySectionsAndBacksOff) {
    fixture();
    const auto victim = raw / "BTC-USD" / "2027" / "01" / "01" / "05.rawl2";
    ASSERT_TRUE(fs::exists(victim)); fs::resize_file(victim, fs::file_size(victim) - 6);
    FlowStore source(sourceOptions()); auto o = options(); size_t replayed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    FlowChunkStore store(source, o);
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
    EXPECT_FALSE(store.read(Id, Minute, Epoch + 5 * Hour));
    EXPECT_TRUE(store.read(Id, Minute, Epoch + 8 * Hour));
    replayed = 0; store.runPass(); EXPECT_EQ(replayed, 0u);
}

TEST_F(ChunkStoreTest, StartupPublishesUnchangedInstrumentBeforeAnyOtherReplay) {
    fixture(); fixture(1, "ETH-USD"); FlowStore source(sourceOptions()); uint64_t eth;
    { FlowChunkStore original(source, options()); ASSERT_EQ(original.runPass().failed, 0u); eth = original.epoch("coinbase:ETH-USD"); }
    auto o = options(); o.bigFloors[Id] = 42;
    FlowChunkStore *active = nullptr; bool observed = false;
    o.afterRecordForTest = [&](std::stop_token) {
        if (observed) return;
        observed = true;
        EXPECT_EQ(active->epoch("coinbase:ETH-USD"), eth);
        EXPECT_TRUE(active->read("coinbase:ETH-USD", Minute, Epoch));
        EXPECT_FALSE(active->read(Id, Minute, Epoch));
    };
    FlowChunkStore restarted(source, o); active = &restarted;
    ASSERT_EQ(restarted.runPass().failed, 0u); EXPECT_TRUE(observed);
}

TEST_F(ChunkStoreTest, FormingDayRunsBeforeBackfillAndBetweenOlderDays) {
    fixture(3);
    Journal forming(raw); forming.snapshot(3 * Day + 1000);
    forming.trades(3 * Day + 2000, {trade(3 * Day + 100, "forming", 100000)});
    forming.heartbeat(3 * Day + Hour + 15000);
    forming.writer.flush();
    FlowStore source(sourceOptions()); auto o = options();
    FlowChunkStore *active = nullptr; std::vector<int64_t> order;
    bool advanced = false, checked = false;
    o.afterDayForTest = [&](int64_t day) {
        order.push_back(day);
        if (day == Epoch + 2 * Day && !advanced) {
            advanced = true;
            forming.heartbeat(3 * Day + 2 * Hour + 15000); forming.close();
        }
        if (day == Epoch + Day) {
            checked = true;
            EXPECT_TRUE(active->read(Id, Minute, Epoch + 3 * Day + Hour));
        }
    };
    FlowChunkStore store(source, o); active = &store;
    ASSERT_EQ(store.runPass().failed, 0u);
    ASSERT_GE(order.size(), 3u);
    EXPECT_EQ(order.front(), Epoch + 3 * Day);
    EXPECT_TRUE(advanced); EXPECT_TRUE(checked);
}

TEST_F(ChunkStoreTest, DamagedPreviouslySealedHourAdvancesEpoch) {
    fixture(); FlowStore source(sourceOptions()); auto o = options(); o.damageRetry = std::chrono::milliseconds(0);
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    const auto before = store.epoch(Id);
    const auto victim = raw / "BTC-USD" / "2027" / "01" / "01" / "05.rawl2";
    const auto original = readBytes(victim);
    fs::resize_file(victim, original.size() - 6);
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_NE(store.epoch(Id), before);
    EXPECT_FALSE(store.read(Id, Minute, Epoch + 5 * Hour));
    { std::ofstream f(victim, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(original.data()), original.size()); }
    fs::last_write_time(victim, fs::last_write_time(victim) + std::chrono::seconds(5));
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_TRUE(store.read(Id, Minute, Epoch + 5 * Hour));
}

TEST_F(ChunkStoreTest, OnlyAffectedDayRetriesFailedEpochWrite) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(1000, {trade(100, "1", 100000)});
    journal.heartbeat(Hour + 15000); journal.close();
    FlowStore source(sourceOptions()); auto o = options(); bool fail = false;
    o.beforeEpochWriteForTest = [&] { if (std::exchange(fail, false)) throw std::runtime_error("epoch write"); };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    const auto before = store.epoch(Id);
    const auto victim = source.archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(victim, fs::last_write_time(victim) + std::chrono::seconds(2));
    fail = true;
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_NE(store.epoch(Id), before);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
}

TEST_F(ChunkStoreTest, HeaderCountersSurviveNewHourAndMetricsCountOnce) {
    Journal journal(raw); journal.snapshot(-Minute);
    journal.trades(Minute + 15000, {trade(100, "late", 100000)});
    journal.heartbeat(Hour + 15000); journal.writer.flush();
    FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    ASSERT_EQ(day(store).lateTrades, 1u);
    journal.trades(Hour + 20000, {trade(Hour + 100, "on-time", 100001)});
    journal.heartbeat(2 * Hour + 15000); journal.close();
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(day(store).lateTrades, 1u);
    const auto counters = metrics.render();
    const auto victim = source.archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(victim, fs::last_write_time(victim) + std::chrono::seconds(2));
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(day(store).lateTrades, 1u);
    EXPECT_EQ(metrics.render(), counters);
}

TEST_F(ChunkStoreTest, ClosedDayPassDecodesNoCursorRecords) {
    Journal journal(raw); journal.snapshot(-Minute);
    for (int h = 0; h < 24; ++h) {
        const auto t = h * Hour;
        journal.trades(t + 1000, {trade(t + 100, std::to_string(h + 1), 100000 + h)});
        journal.heartbeat(t + Hour + 15000);
    }
    journal.writer.flush();
    FlowStore source(sourceOptions()); auto o = options(); size_t cursorRecords = 0;
    o.afterCursorRecordForTest = [&] { ++cursorRecords; };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    cursorRecords = 0;
    journal.heartbeat(Day + 20000); journal.writer.flush(); // pure append to the open tail
    ASSERT_EQ(store.runPass().failed, 0u);
    EXPECT_EQ(cursorRecords, 0u);
    journal.close();
}

TEST_F(ChunkStoreTest, ReadRevalidatesOnlyItsCopiedIndex) {
    fixture(); FlowStore source(sourceOptions()); auto o = options();
    FlowChunkStore *active = nullptr; bool restored = false;
    o.beforeReadInvalidationForTest = [&] {
        if (restored) return;
        restored = true;
        const auto path = active->filePath(Id, Epoch);
        const auto saved = readBytes(base / "saved.flc");
        { std::ofstream f(path, std::ios::binary | std::ios::trunc);
          f.write(reinterpret_cast<const char *>(saved.data()), saved.size()); }
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
        EXPECT_EQ(active->runPass().failed, 0u);
    };
    FlowChunkStore store(source, o); active = &store;
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto path = store.filePath(Id, Epoch);
    fs::copy_file(path, base / "saved.flc");
    fs::resize_file(path, fs::file_size(path) - 1);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
    EXPECT_TRUE(restored);
    EXPECT_TRUE(store.read(Id, Minute, Epoch));
}

TEST_F(ChunkStoreTest, SameStatSectionCorruptionIsRejectedByReadCrc) {
    fixture(); FlowStore source(sourceOptions()); FlowChunkStore store(source, options());
    ASSERT_EQ(store.runPass().failed, 0u);
    const auto path = store.filePath(Id, Epoch);
    const auto modified = fs::last_write_time(path);
    auto encoded = readBytes(path); const auto payload = day(store).sections[0].payload;
    auto at = std::search(encoded.begin(), encoded.end(), payload.begin(), payload.end());
    ASSERT_NE(at, encoded.end()); *at ^= 1;
    { std::ofstream f(path, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(encoded.data()), encoded.size()); }
    fs::last_write_time(path, modified);
    EXPECT_FALSE(store.read(Id, Minute, Epoch));
}

TEST_F(ChunkStoreTest, FirstBlockHourFailureLeavesLaterHoursAndBacksOff) {
    fixture();
    const auto victim = raw / "BTC-USD" / "2027" / "01" / "01" / "05.rawl2";
    auto encoded = readBytes(victim);
    uint32_t header = 0; for (unsigned i = 0; i < 4; ++i) header |= uint32_t(encoded[8 + i]) << (8 * i);
    encoded.at(16 + header + 48) ^= 1;
    { std::ofstream f(victim, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(encoded.data()), encoded.size()); }
    FlowStore source(sourceOptions()); auto o = options(); size_t replayed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    FlowChunkStore store(source, o);
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_TRUE(store.read(Id, Minute, Epoch + 8 * Hour));
    replayed = 0; store.runPass(); EXPECT_EQ(replayed, 0u);
}
TEST_F(ChunkStoreTest, FirstBlockDamageAfterSealUsesPerHourBackoff) {
    fixture(); FlowStore source(sourceOptions()); auto o = options(); size_t replayed = 0;
    o.afterRecordForTest = [&](std::stop_token) { ++replayed; };
    FlowChunkStore store(source, o); ASSERT_EQ(store.runPass().failed, 0u);
    const auto victim = raw / "BTC-USD" / "2027" / "01" / "01" / "05.rawl2";
    auto encoded = readBytes(victim);
    uint32_t header = 0; for (unsigned i = 0; i < 4; ++i) header |= uint32_t(encoded[8 + i]) << (8 * i);
    encoded.at(16 + header + 48) ^= 1;
    { std::ofstream f(victim, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(encoded.data()), encoded.size()); }
    EXPECT_GT(store.runPass().failed, 0u);
    EXPECT_FALSE(store.read(Id, Minute, Epoch + 5 * Hour));
    EXPECT_TRUE(store.read(Id, Minute, Epoch + 8 * Hour));
    replayed = 0; store.runPass(); EXPECT_EQ(replayed, 0u);
}

TEST_F(ChunkStoreTest, OfflineDiffListsEveryMovedTrade) {
    Journal j(raw); j.snapshot(-Minute);
    j.trades(1000, {trade(100, "1", 100000)});
    j.trades(Minute + 300000, {trade(200, "2", 100001)});
    j.heartbeat(Day + 300000); j.close();
    FlowStore source(sourceOptions()); ASSERT_EQ(source.runPass().failed, 0u);
    QProcess process;
    process.start(FLOW_CHUNK_DIFF_EXE, {QString::fromStdString(raw.string()), QString::fromStdString(hmc.string()),
        QString::fromStdString(source.root().string()), QString::fromStdString((base / "diff").string()),
        "BTC-USD", "2027-01-01"});
    ASSERT_TRUE(process.waitForFinished(60000));
    const auto output = process.readAllStandardOutput().toStdString();
    EXPECT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString() << output;
    EXPECT_NE(output.find("differing_bars=1 trades_moved=1 late_trades=1"), std::string::npos) << output;
    EXPECT_NE(output.find("moved=excluded id=2"), std::string::npos) << output;
    EXPECT_NE(output.find("legacy_membership_proven=1"), std::string::npos) << output;
    QProcess repeated;
    repeated.start(FLOW_CHUNK_DIFF_EXE, {QString::fromStdString(raw.string()), QString::fromStdString(hmc.string()),
        QString::fromStdString(source.root().string()), QString::fromStdString((base / "diff").string()),
        "BTC-USD", "2027-01-01"});
    ASSERT_TRUE(repeated.waitForFinished(60000));
    EXPECT_EQ(repeated.exitCode(), 0) << repeated.readAllStandardError().toStdString();
    EXPECT_NE(repeated.readAllStandardOutput().toStdString().find("differing_bars=1 trades_moved=1"), std::string::npos);
}

}
