#include <gtest/gtest.h>
#include "capture/RawCapture.hpp"
#include "capture/CaptureVerifier.hpp"
#include "capture/CaptureSession.hpp"
#include "capture/CaptureRouting.hpp"
#include "capture/CaptureMetrics.hpp"
#include "legacy_v2_fixture.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "metrics/ProcessMetrics.hpp"
#include <thread>
#include <future>
#include <filesystem>
#include <iostream>
#include "servermodel/HmcolFormat.hpp"
#include "marketdata/fixtures/coinbase_messages.hpp"
#include <QDirIterator>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <QProcess>
#include <QProcessEnvironment>

using namespace sentinel::capture;
namespace {
constexpr int64_t Hour = 3600LL * 1000000000;
constexpr int64_t Epoch = 497424LL * Hour;
nlohmann::json metadata() {
    return {{"tool_version", "offline-fixture"}, {"product_metadata", {
        {"product_id", "BTC-USD"}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}};
}
QStringList paths(const QString& root) {
    QStringList result;
    QDirIterator it(root, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) result.push_back(it.next());
    result.sort(); return result;
}
QByteArray contents(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture read");
    return file.readAll();
}
void save(const QString& path, const QByteArray& bytes) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("fixture write");
    if (file.write(bytes) != bytes.size()) throw std::runtime_error("fixture short write");
}
Record record(Kind kind, int64_t offset, std::string payload = "{}", uint64_t connection = 1) {
    return {kind, {Epoch + offset, 1000000000000 + offset}, connection, std::move(payload)};
}
Record frame(nlohmann::json json, uint64_t sequence, int64_t offset, uint64_t connection = 1) {
    json["sequence_num"] = sequence;
    return record(Kind::Frame, offset, json.dump(), connection);
}
std::vector<Record> fixture() {
    auto snapshot = fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}, {99, 0}}, {{101, 2}});
    snapshot["events"][0]["updates"][0]["new_quantity"] = "1.00000001";
    auto trades = nlohmann::json::parse(R"({
      "channel":"market_trades","timestamp":"2026-09-29T00:00:00.000000001Z",
      "events":[{"type":"update","trades":[{"product_id":"BTC-USD","price":"100.01",
      "size":"0.00000001","side":"BUY","trade_id":"123","time":"2026-09-29T00:00:00.000000001Z"}]}]})");
    return {
        record(Kind::CaptureStarted, 0, "{}", 0), record(Kind::TransportUp, 1),
        frame(fixtures::coinbaseSubscriptionAck({"BTC-USD"}), 0, 100),
        frame(snapshot, 1, 200),
        frame(nlohmann::json{{"channel", "heartbeats"}, {"events", nlohmann::json::array()}}, 2, 1000000000),
        frame(trades, 3, 1100000000),
        frame(fixtures::coinbaseL2Update("BTC-USD", {{"bid", 100, 0}, {"bid", 99, 1.25}, {"offer", 99.5, 1}}), 4, 2000000000),
        record(Kind::CaptureStopped, 3000000000, R"({"reason":"signal=15"})")};
}
struct CaptureTest : testing::Test {
    QTemporaryDir dir;
    WriterConfig config;
    void SetUp() override { ASSERT_TRUE(dir.isValid()); config.root = dir.path(); config.fsyncBlocks = 0; }
};
TEST_F(CaptureTest, ExactBytesAndClocksRoundTripIncludingWhitespaceNulAndInvalidJson) {
    config.fsyncBlocks = 1;
    auto records = fixture();
    records.insert(records.end() - 1, record(Kind::Frame, 2100000000, std::string("\n { \"x\":\"0.0000000100\" }\t") + '\0' + "broken"));
    records.insert(records.end() - 1, record(Kind::BookInvalidated, 2200000000, R"({"reason":"sequence gap expected=5 got=8","product":"BTC-USD"})"));
    records.insert(records.end() - 1, record(Kind::ResyncRequested, 2300000000, R"({"reason":"sequence gap"})"));
    Writer writer(config, metadata());
    for (const auto& value : records) writer.append(value);
    writer.close(); writer.close();
    std::vector<Record> decoded;
    const auto scanResult = scan(paths(config.root).front(), [&](const Record& value) { decoded.push_back(value); });
    EXPECT_EQ(decoded, records);
    EXPECT_TRUE(scanResult.indexed); EXPECT_FALSE(scanResult.tornTail);
    EXPECT_EQ(scanResult.header["product_metadata"]["base_increment"], "0.00000001");
    EXPECT_EQ(scanResult.header["channels"].size(), 3);
    EXPECT_EQ(scanResult.fileBytes, scanResult.validBytes);
    EXPECT_THROW(writer.append(records.front()), std::runtime_error);
}
TEST_F(CaptureTest, ByteBoundaryLargeFrameAndTimedIdleFlush) {
    config.blockBytes = 100;
    Writer writer(config, metadata());
    const auto a = record(Kind::Frame, 0, std::string(50, 'a'));
    const auto b = record(Kind::Frame, 1, std::string(51, 'b'));
    const auto big = record(Kind::Frame, 2, std::string(5000, 'z'));
    writer.append(a); writer.append(b); writer.append(big);
    EXPECT_EQ(writer.stats().blocks, 3);
    const auto last = record(Kind::Frame, 3, "short");
    writer.append(last);
    writer.flushDue(last.time.steadyNs + 999999999); EXPECT_EQ(writer.stats().blocks, 3);
    writer.flushDue(last.time.steadyNs + 1000000000); EXPECT_EQ(writer.stats().blocks, 4);
    writer.close();
    const auto result = scan(paths(config.root).front());
    ASSERT_EQ(result.index.size(), 4);
    EXPECT_EQ(result.index[0].rawBytes, 82); EXPECT_EQ(result.index[1].rawBytes, 83);
    EXPECT_EQ(result.index[2].rawBytes, 5032);
    for (size_t i = 1; i < result.index.size(); ++i)
        EXPECT_EQ(result.index[i].offset, result.index[i - 1].offset + 48 + result.index[i - 1].compressedBytes);
}
TEST_F(CaptureTest, TornTailSkipsOnlyIncompleteFinalBlockAndRebuildsIndex) {
    Writer writer(config, metadata());
    writer.append(record(Kind::Frame, 0, "first")); writer.flush();
    writer.append(record(Kind::Frame, 1, std::string(10000, 'b'))); writer.close();
    const auto originalPath = paths(config.root).front();
    const auto original = contents(originalPath);
    const auto intact = scan(originalPath);
    ASSERT_EQ(intact.index.size(), 2);
    const auto& last = intact.index.back();
    for (const auto length : {last.offset + 1, last.offset + 20, last.offset + 48, last.offset + 48 + last.compressedBytes - 1}) {
        const auto path = dir.path() + "/torn.rawl2";
        save(path, original.first(length));
        std::vector<Record> records;
        const auto result = scan(path, [&](auto& r) { records.push_back(r); });
        EXPECT_TRUE(result.tornTail); EXPECT_FALSE(result.indexed);
        ASSERT_EQ(result.index.size(), 1); ASSERT_EQ(records.size(), 1);
        EXPECT_EQ(records.front().payload, "first");
        EXPECT_EQ(contents(path).size(), length); // read-only recovery
    }
    const auto footer = last.offset + 48 + last.compressedBytes;
    save(dir.path() + "/unindexed.rawl2", original.first(footer));
    const auto rebuilt = scan(dir.path() + "/unindexed.rawl2");
    EXPECT_FALSE(rebuilt.indexed); EXPECT_FALSE(rebuilt.tornTail); EXPECT_EQ(rebuilt.index, intact.index);
    save(dir.path() + "/partial-index.rawl2", original.first(footer + 7));
    EXPECT_TRUE(scan(dir.path() + "/partial-index.rawl2").tornTail);
}
TEST_F(CaptureTest, RejectsCompleteCorruptionInHeaderBlockAndIndex) {
    Writer writer(config, metadata());
    writer.append(record(Kind::Frame, 0, "first")); writer.flush();
    writer.append(record(Kind::Frame, 1, "second")); writer.close();
    const auto path = paths(config.root).front();
    const auto original = contents(path);
    const auto intact = scan(path);
    for (const auto offset : {uint64_t(18), intact.index[0].offset + 10, intact.index[1].offset + 50, uint64_t(original.size() - 1)}) {
        auto corrupt = original; corrupt[offset] = char(corrupt[offset] ^ 1);
        save(path, corrupt); EXPECT_THROW(scan(path), std::runtime_error) << offset;
    }
    // Keep the compressed stream and header CRC valid, but contradict raw CRC.
    auto wrongRawCrc = original;
    const auto block = intact.index.back().offset;
    wrongRawCrc[block + 40] = char(wrongRawCrc[block + 40] ^ 1);
    const auto headerCrc = hmcol::crc32(wrongRawCrc.constData() + block, 44);
    for (int i = 0; i < 4; ++i) wrongRawCrc[block + 44 + i] = char(headerCrc >> (8 * i));
    save(path, wrongRawCrc);
    try { scan(path); FAIL() << "expected raw CRC failure"; }
    catch (const std::runtime_error& e) { EXPECT_NE(std::string(e.what()).find("block CRC mismatch"), std::string::npos); }
    save(path, original + QByteArray("trailing")); EXPECT_THROW(scan(path), std::runtime_error);
}
TEST_F(CaptureTest, ZeroAndGarbageSuffixesAreTornButInteriorDamageIsFatal) {
    Writer writer(config, metadata());
    writer.append(record(Kind::Frame, 0, "one")); writer.flush();
    writer.append(record(Kind::Frame, 1, "two")); writer.close();
    const auto path = paths(config.root).front();
    const auto original = contents(path);
    const auto index = scan(path).index;
    const auto end = index.back().offset + 48 + index.back().compressedBytes;
    for (const auto& suffix : {QByteArray(8192, '\0'), QByteArray("garbage BLK1 invalid IDX1 junk"), QByteArray(131072, 'x')}) {
        save(path, original.first(end) + suffix);
        std::vector<Record> recovered;
        const auto result = scan(path, [&](auto& r) { recovered.push_back(r); });
        EXPECT_TRUE(result.tornTail); EXPECT_FALSE(result.indexed);
        EXPECT_EQ(result.validBytes, end); ASSERT_EQ(recovered.size(), 2);
        EXPECT_EQ(recovered.back().payload, "two");
    }
    auto broken = original;
    for (const auto entry : index) {
        broken = original;
        for (int i = 0; i < 4; ++i) broken[entry.offset + i] = 0;
        save(path, broken); EXPECT_THROW(scan(path), std::runtime_error);
    }
    // A later valid header split across a tail-scanning chunk still proves
    // interior damage; it must never be silently classified as a torn suffix.
    save(path, original.first(index[1].offset) + QByteArray(65535, 'x') + original.mid(index[1].offset));
    EXPECT_THROW(scan(path), std::runtime_error);
}
TEST_F(CaptureTest, HourlyRotationRestartAndClockRollbackNeverOverwrite) {
    Writer writer(config, metadata());
    writer.append(record(Kind::Frame, Hour - 1, "hour A"));
    writer.append(record(Kind::Frame, Hour, "hour B"));
    writer.append(record(Kind::Frame, Hour - 2, "clock rolled back")); writer.close();
    auto files = paths(config.root); ASSERT_EQ(files.size(), 3);
    std::vector<QByteArray> before;
    for (const auto& file : files) before.push_back(contents(file));
    Writer restart(config, metadata()); restart.append(record(Kind::Frame, Hour - 1, "new process")); restart.close();
    EXPECT_EQ(paths(config.root).size(), 4);
    for (int i = 0; i < files.size(); ++i) { EXPECT_EQ(contents(files[i]), before[i]); EXPECT_TRUE(scan(files[i]).indexed); }
}
TEST_F(CaptureTest, DestructorLeavesScannableUnindexedBlocks) {
    { Writer writer(config, metadata()); writer.append(record(Kind::Frame, 0, "committed")); writer.flush(); }
    const auto result = scan(paths(config.root).front());
    EXPECT_FALSE(result.indexed); EXPECT_FALSE(result.tornTail); ASSERT_EQ(result.index.size(), 1);
}
TEST_F(CaptureTest, VerifierReplaysEveryChannelWithExactDecimalGridAndReportsRates) {
    Writer writer(config, metadata());
    for (const auto& value : fixture()) writer.append(value);
    writer.close();
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["frames"], 5); EXPECT_EQ(report.json["sequence_gaps"], 0);
    EXPECT_EQ(report.json["snapshot_zero_entries"], 1); EXPECT_EQ(report.json["snapshot_entries"], 3);
    EXPECT_EQ(report.json["subscription_acks"], 1); EXPECT_EQ(report.json["replayed_l2_events"], 2);
    EXPECT_EQ(report.json["channels"].size(), 4);
    EXPECT_EQ(report.json["fps_p99_one_second_buckets"], 2);
    EXPECT_DOUBLE_EQ(report.json["fps_mean"].get<double>(), 5.0 / 3);
    EXPECT_DOUBLE_EQ(report.json["received_bytes_per_day"].get<double>(), report.json["received_bytes"].get<double>() * 28800);
}
TEST_F(CaptureTest, VerifierCarriesSequenceAndBookAcrossRotation) {
    auto records = fixture();
    for (auto& r : records) r.time.systemNs += Hour - 500000000;
    Writer writer(config, metadata());
    for (const auto& r : records) writer.append(r);
    writer.close();
    ASSERT_EQ(paths(config.root).size(), 2);
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["runs"], 1); EXPECT_EQ(report.json["connections"], 1);
    const auto alone = verify(paths(config.root).back());
    EXPECT_FALSE(alone.ok); // cannot claim a book without its preceding snapshot
    EXPECT_GT(alone.json["unanchored_l2_events"].get<int>(), 0);
}
TEST_F(CaptureTest, DetectsMissingHeartbeatSequenceAndReconnectResetsOnlyOnTransportUp) {
    auto records = fixture();
    records.erase(records.begin() + 4); // heartbeat sequence 2: gap visible in trade channel
    records.pop_back();
    records.push_back(record(Kind::BookInvalidated, 2100000000, R"({"reason":"sequence gap expected=2 got=3"})"));
    records.push_back(record(Kind::ResyncRequested, 2200000000, R"({"reason":"sequence gap"})"));
    records.push_back(record(Kind::TransportDown, 2300000000));
    records.push_back(record(Kind::TransportUp, 2400000000, "{}", 2));
    records.push_back(frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 2500000000, 2));
    records.push_back(record(Kind::CaptureStopped, 3000000000, "{}", 2));
    Writer writer(config, metadata()); for (auto& r : records) writer.append(r); writer.close();
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_EQ(report.json["sequence_gaps"], 1);
    EXPECT_EQ(report.json["reconnects"], 1); EXPECT_EQ(report.json["snapshots"], 2);
    EXPECT_EQ(report.json["invalidations"], 1); EXPECT_EQ(report.json["resync_requests"], 1);
    EXPECT_EQ(report.json["errors"], 0);
}
TEST_F(CaptureTest, FirstSequenceMustBeZeroAndMalformedOrCrossedBooksFail) {
    for (const auto mode : {0, 1, 2, 3}) {
        QTemporaryDir sub;
        config.root = sub.path();
        auto records = fixture();
        auto snapshot = nlohmann::json::parse(records[3].payload);
        if (mode == 0) records[2] = frame(fixtures::coinbaseSubscriptionAck({"BTC-USD"}), 5, 100);
        if (mode == 1) snapshot["events"][0]["updates"][0]["price_level"] = "101.00";
        if (mode == 2) snapshot["events"][0]["updates"][0]["new_quantity"] = "0.000000001";
        if (mode == 3) snapshot["events"][0]["updates"][0]["price_level"] = "NaN";
        records[3].payload = snapshot.dump();
        Writer writer(config, metadata()); for (auto& r : records) writer.append(r); writer.close();
        EXPECT_FALSE(verify(config.root).ok) << mode;
    }
}
TEST_F(CaptureTest, InvalidMetadataAcrossSeveralSegmentsProducesErrorsWithoutReplay) {
    auto invalid = metadata(); invalid["product_metadata"]["base_increment"] = "not-a-decimal";
    Writer writer(config, invalid);
    writer.append(record(Kind::Frame, Hour - 1, "{}"));
    writer.append(record(Kind::Frame, Hour, "{}")); writer.close();
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_GE(report.json["errors"].get<int>(), 2);
}
TEST_F(CaptureTest, MissingStopReportsOpenPrefixWithoutClaimingCompletion) {
    auto records = fixture(); records.pop_back();
    Writer writer(config, metadata()); for (auto& r : records) writer.append(r); writer.close();
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok); EXPECT_EQ(report.json["incomplete_runs"], 1);
    EXPECT_EQ(report.json["open_runs"], 1); EXPECT_EQ(report.json["complete"], false);
    EXPECT_EQ(report.json["ok_closed_runs"], true);
}
TEST_F(CaptureTest, SessionSigtermStyleCloseDrainsAcceptedRecordsAndSeals) {
    Session session(config, metadata());
    auto records = fixture();
    // Same lifecycle as SIGTERM: stop producer, submit stop marker, drain, close.
    for (auto& r : records) ASSERT_TRUE(session.submit(r));
    session.close(); session.close();
    ASSERT_TRUE(session.error().empty()) << session.error(); EXPECT_EQ(session.stats().frames, 5);
    std::vector<Record> readBack;
    const auto result = scan(paths(config.root).front(), [&](auto& r) { readBack.push_back(r); });
    EXPECT_TRUE(result.indexed); EXPECT_EQ(readBack, records);
    EXPECT_TRUE(verify(config.root).ok);
    EXPECT_FALSE(session.submit(records.front()));
}
TEST_F(CaptureTest, SessionDrainsOldReceiveTimesInBlocksInsteadOfFlushingEveryFrame) {
    Session session(config, metadata());
    // Steady times near 0 so the stop marker (Stamp::now) is later even on a freshly booted host.
    for (int i = 0; i < 200; ++i) {
        auto old = record(Kind::Frame, i, "old queued frame");
        old.time.steadyNs = i;
        ASSERT_TRUE(session.submit(old));
    }
    session.close();
    ASSERT_TRUE(session.error().empty()) << session.error();
    EXPECT_EQ(session.stats().frames, 200);
    EXPECT_EQ(session.stats().blocks, 2); // one data block, then the current-time stop marker
}
TEST_F(CaptureTest, BoundedQueueFailsInsteadOfEvictingAndStorageFailureSurfaces) {
    Session session(config, metadata(), 8192);
    EXPECT_FALSE(session.submit(record(Kind::Frame, 0, std::string(8192, 'a'))));
    session.close(); EXPECT_NE(session.error().find("limit"), std::string::npos);
    config.root = dir.path() + "/not-a-directory"; save(config.root, "file");
    Session failed(config, metadata()); failed.submit(record(Kind::Frame, 0, "x")); failed.close();
    EXPECT_FALSE(failed.error().empty());
}
TEST_F(CaptureTest, OverflowPersistsReservedStopWithFirstDroppedFrameAndExplicitGap) {
    Session session(config, metadata(), 16384);
    auto records = fixture();
    for (size_t i = 0; i + 1 < records.size(); ++i) ASSERT_TRUE(session.submit(records[i]));
    const auto dropped = record(Kind::Frame, 2100000000, std::string(32768, 'a'));
    EXPECT_FALSE(session.submit(dropped));
    EXPECT_FALSE(session.submit(record(Kind::Frame, 2200000000, "later loss")));
    EXPECT_TRUE(session.submit(records.back())); // reserved slot survives failure
    session.close();
    EXPECT_FALSE(session.error().empty());
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_EQ(report.json["explicit_capture_gaps"], 1);
    EXPECT_EQ(report.json["capture_stops"], 1); EXPECT_EQ(report.json["incomplete_runs"], 0);
    const auto gap = report.json["capture_gap_details"][0];
    EXPECT_EQ(gap["first_dropped_system_ns"], dropped.time.systemNs);
    EXPECT_EQ(gap["first_dropped_steady_ns"], dropped.time.steadyNs);
    EXPECT_EQ(gap["first_dropped_connection"], 1);
    EXPECT_NE(gap["reason"].get<std::string>().find("limit"), std::string::npos);
}
#ifndef _WIN32
TEST_F(CaptureTest, DiskWriteFailureLeavesTailAndPersistsGapInANewSegment) {
    // Exercise real short writes without filling a volume: the OS refuses any
    // one file larger than 8 KiB; a fresh small failure segment can still fit.
    struct Limit {
        rlimit previous{};
        decltype(std::signal(SIGXFSZ, SIG_IGN)) handler;
        Limit() : handler(std::signal(SIGXFSZ, SIG_IGN)) {
            if (getrlimit(RLIMIT_FSIZE, &previous)) throw std::runtime_error("getrlimit");
            auto limited = previous; limited.rlim_cur = 8192;
            if (setrlimit(RLIMIT_FSIZE, &limited)) throw std::runtime_error("setrlimit");
        }
        ~Limit() { setrlimit(RLIMIT_FSIZE, &previous); std::signal(SIGXFSZ, handler); }
    } limit;
    config.blockBytes = MaxRecordBytes;
    Session session(config, metadata());
    auto records = fixture();
    for (size_t i = 0; i < 4; ++i) ASSERT_TRUE(session.submit(records[i]));
    std::string noise(65536, '\0');
    uint32_t random = 0x12345678;
    for (char& c : noise) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; c = char(random); }
    ASSERT_TRUE(session.submit(record(Kind::Frame, 300, std::move(noise))));
    ASSERT_TRUE(session.submit(record(Kind::CaptureStopped, 400)));
    session.close();
    EXPECT_FALSE(session.error().empty());
    ASSERT_EQ(paths(config.root).size(), 2);
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_EQ(report.json["explicit_capture_gaps"], 1);
    const auto gap = report.json["capture_gap_details"][0];
    // The first frame in the failed block, not just the last dequeued frame.
    EXPECT_EQ(gap["first_dropped_system_ns"], records[2].time.systemNs);
    EXPECT_EQ(gap["first_dropped_steady_ns"], records[2].time.steadyNs);
}
#endif
TEST_F(CaptureTest, RefusesMissingVolumeRecordingPathSymlinkAndUnsafeSymbol) {
    EXPECT_THROW(validateRoot("/Volumes/SentinelDefinitelyAbsentCaptureVolume/raw"), std::runtime_error);
    EXPECT_THROW(validateRoot("/Volumes/T7/sentinel-data/recording/BTC-USD"), std::runtime_error);
    EXPECT_THROW(validateRoot("relative/root"), std::runtime_error);
    EXPECT_THROW(validateSymbol("../recording"), std::runtime_error);
    EXPECT_THROW(validateSymbol("BTC/USD"), std::runtime_error);
    EXPECT_EQ(validateRoot(dir.path() + "/new/root"), QFileInfo(dir.path()).canonicalFilePath() + "/new/root");
}
std::vector<ProductCapture> multiProducts(const WriterConfig& config) {
    std::vector<ProductCapture> result;
    for (const auto& symbol : {"BTC-USD", "ETH-USD"}) {
        auto cfg = config;
        cfg.symbol = symbol;
        auto meta = metadata();
        meta["product_metadata"]["product_id"] = symbol;
        if (cfg.symbol == "ETH-USD") meta["product_metadata"]["quote_increment"] = "0.001";
        result.push_back({cfg, meta});
    }
    return result;
}
std::vector<Record> multiFixture() {
    auto records = fixture();
    // Ack, BTC snapshot, ETH snapshot, ETH trades, mixed update, heartbeat.
    records[2] = frame(fixtures::coinbaseSubscriptionAck({"BTC-USD", "ETH-USD"}), 0, 100);
    records[3].payload = " \n" + records[3].payload + "\t";
    auto ethSnapshot = fixtures::coinbaseL2Snapshot("ETH-USD", {{10, 1}}, {{11, 1}});
    ethSnapshot["events"][0]["updates"][0]["price_level"] = "10.001";
    records[4] = frame(ethSnapshot, 2, 1000000000);
    auto trade = nlohmann::json::parse(records[5].payload);
    trade["events"][0]["trades"][0]["product_id"] = "ETH-USD";
    records[5] = frame(trade, 3, 1100000000);
    auto both = fixtures::coinbaseL2Update("BTC-USD", {{"bid", 99, 4}});
    both["events"].push_back(fixtures::coinbaseL2Update("ETH-USD", {{"offer", 11, 3}})["events"][0]);
    records[6] = frame(both, 4, 2000000000);
    records[6].payload = "\n " + records[6].payload + " \t";
    records.insert(records.end() - 1, frame(nlohmann::json{{"channel", "heartbeats"}, {"events", nlohmann::json::array()}}, 5, 2100000000));
    return records;
}
// Rewrites an existing file's stream (tamper/crash fixtures): a v2 header goes
// through the test-only v2 writer, a v1 header through the production writer.
std::unique_ptr<Writer> rewriteWriter(const WriterConfig& cfg, const nlohmann::json& header) {
    if (header.contains("connection_products")) return LegacyV2FixtureWriter::make(cfg, header);
    return std::make_unique<Writer>(cfg, header);
}
// RAWL2 v2 (one connection, several products) through the test-only writer:
// production writes v1 only; these files exercise the verifier's v2 reader.
void writeMulti(const WriterConfig& config, const std::vector<Record>& records) {
    ASSERT_NO_THROW(writeLegacyV2(multiProducts(config), records));
}
TEST_F(CaptureTest, MultiRoutesExactFramesAndClocksWithReceiptsAndMixedProductEnvelope) {
    const auto input = multiFixture();
    writeMulti(config, input);
    ASSERT_EQ(paths(config.root).size(), 2);
    std::string run;
    for (const auto& path : paths(config.root)) {
        std::vector<Record> records;
        const auto result = scan(path, [&](const auto& r) { records.push_back(r); });
        EXPECT_EQ(result.header["format_version"], 2);
        EXPECT_EQ(result.header["routing"], "product-ranges-v2");
        EXPECT_EQ(contents(path).left(8), QByteArray("RAWL2\r\n\2", 8));
        const auto id = result.header["run_id"].get<std::string>();
        if (run.empty()) run = id;
        EXPECT_EQ(run, id);
        const bool btc = result.header["product_metadata"]["product_id"] == "BTC-USD";
        std::vector<Record> expected;
        for (size_t i = 0; i < input.size(); ++i)
            if (!(btc ? (i == 4 || i == 5) : i == 3)) expected.push_back(input[i]);
        ASSERT_EQ(records.size(), expected.size() + 1);
        const auto proof = records[records.size() - 2];
        records.erase(records.end() - 2);
        EXPECT_EQ(records, expected); // mixed frame, whitespace and clocks are byte-exact
        EXPECT_EQ(proof.kind, Kind::FrameReference);
        EXPECT_EQ(proof.time, input[input.size() - 2].time);
        EXPECT_EQ(proof.connection, 1);
        const auto receipt = nlohmann::json::parse(proof.payload);
        EXPECT_EQ(receipt["first_seq"], 0); EXPECT_EQ(receipt["last_seq"], 5);
        EXPECT_EQ(receipt["count"], 6); EXPECT_EQ(receipt["foreign_count"], btc ? 2 : 1);
        EXPECT_EQ(receipt["sha256"].get<std::string>().size(), 64);
    }
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["routing_checks_deferred"], 0);
    EXPECT_EQ(report.json["connection_runs"].size(), 1);
}
TEST_F(CaptureTest, MultiVerifierPerProductAccountingAndUniqueConnectionTotals) {
    const auto input = multiFixture();
    writeMulti(config, input);
    const auto report = verify(config.root);
    ASSERT_TRUE(report.ok) << report.json.dump(2);
    uint64_t received = 0, btcBytes = 0, ethBytes = 0, disk = 0;
    for (size_t i = 0; i < input.size(); ++i) if (input[i].kind == Kind::Frame) {
        received += input[i].payload.size();
        if (i != 4 && i != 5) btcBytes += input[i].payload.size();
        if (i != 3) ethBytes += input[i].payload.size();
    }
    for (const auto& path : paths(config.root)) disk += QFileInfo(path).size();
    const auto& btc = report.json["products"]["BTC-USD"];
    const auto& eth = report.json["products"]["ETH-USD"];
    EXPECT_EQ(btc["frames"], 4); EXPECT_EQ(eth["frames"], 5);
    EXPECT_EQ(btc["l2_events"], 2); EXPECT_EQ(eth["l2_events"], 2);
    EXPECT_EQ(btc["snapshots"], 1); EXPECT_EQ(eth["snapshots"], 1);
    EXPECT_EQ(btc["received_bytes"], btcBytes); EXPECT_EQ(eth["received_bytes"], ethBytes);
    EXPECT_EQ(report.json["totals"]["frames"], 6);
    EXPECT_EQ(report.json["totals"]["received_bytes"], received);
    EXPECT_EQ(report.json["totals"]["file_bytes"], disk);
    EXPECT_EQ(report.json["totals"]["stored_frames"], 9);
    EXPECT_EQ(report.json["totals"]["l2_events"], 4);
    EXPECT_EQ(report.json["totals"]["connections"], 1);
    EXPECT_EQ(report.json["totals"]["days"]["2026-09-30"]["frames"], 6);
    EXPECT_EQ(report.json["totals"]["days"]["2026-09-30"]["received_bytes"], received);
    EXPECT_EQ(report.json["totals"]["days"]["2026-09-30"]["file_bytes"], disk);
    EXPECT_EQ(btc["days"]["2026-09-30"]["frames"], 4);
    EXPECT_EQ(eth["days"]["2026-09-30"]["l2_events"], 2);
    EXPECT_DOUBLE_EQ(btc["received_bytes_per_day"].get<double>(), btcBytes * 28800.0);
    EXPECT_DOUBLE_EQ(eth["file_bytes_per_day"].get<double>(), eth["file_bytes"].get<double>() * 28800.0);
    EXPECT_DOUBLE_EQ(report.json["totals"]["received_bytes_per_day"].get<double>(), received * 28800.0);
    const auto alone = verify(config.root + "/BTC-USD");
    EXPECT_TRUE(alone.ok); EXPECT_EQ(alone.json["sequence_gaps"], 0);
    EXPECT_EQ(alone.json["routing_checks_deferred"], 1);
}
TEST_F(CaptureTest, MultiGapAcrossProductsInvalidatesEveryBookIncludingReceiptOnlyStream) {
    auto input = multiFixture();
    input.erase(input.begin() + 5); // ETH trade #3 missing; mixed update #4 must invalidate BOTH books
    writeMulti(config, input);
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["totals"]["sequence_gaps"], 1);
    for (const auto& symbol : {"BTC-USD", "ETH-USD"}) {
        const auto& product = report.json["products"][symbol];
        EXPECT_EQ(product["sequence_gaps"], 1);
        EXPECT_EQ(product["unanchored_l2_events"], 1);
    }
}
TEST_F(CaptureTest, EmptyConnectionsPassButUpdatesWithoutSnapshotStillFailV1V2) {
    for (const bool multi : {false, true}) for (const bool updates : {false, true}) {
        SCOPED_TRACE(multi);
        SCOPED_TRACE(updates);
        QTemporaryDir temp; auto cfg = config; cfg.root = temp.path();
        std::vector<Record> input{record(Kind::CaptureStarted, 0, "{}", 0), record(Kind::TransportUp, 1)};
        if (updates) input.push_back(frame(fixtures::coinbaseL2Update("BTC-USD", {{"bid", 100, 1}}), 0, 1000000000));
        input.push_back(record(Kind::TransportDown, 21000000000LL));
        input.push_back(record(Kind::TransportUp, 22000000000LL, "{}", 2));
        input.push_back(frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 23000000000LL, 2));
        if (multi) input.push_back(frame(fixtures::coinbaseL2Snapshot("ETH-USD", {{10, 1}}, {{11, 1}}), 1, 24000000000LL, 2));
        input.push_back(record(Kind::CaptureStopped, 25000000000LL, "{}", 2));
        if (multi) writeMulti(cfg, input);
        else { Writer writer(cfg, metadata()); for (const auto& value : input) writer.append(value); writer.close(); }
        const auto r = verify(cfg.root);
        EXPECT_EQ(r.ok, !updates) << r.json.dump(2); EXPECT_EQ(r.exitCode(), updates ? 2 : 0);
        EXPECT_EQ(r.json["ok_closed_runs"], !updates);
        const auto& btc = r.json["products"]["BTC-USD"];
        EXPECT_EQ(btc["empty_connections"], updates ? 0 : 1);
        EXPECT_EQ(btc["missing_snapshot_connections"], updates ? 1 : 0);
        EXPECT_EQ(btc["unanchored_l2_events"], updates ? 1 : 0);
        EXPECT_EQ(r.json["totals"]["empty_connections"], (updates ? 0 : 1) + (multi ? 1 : 0));
        if (!updates) {
            ASSERT_EQ(btc["empty_connection_details"].size(), 1);
            const auto& detail = btc["empty_connection_details"][0];
            EXPECT_EQ(detail["connection"], 1); EXPECT_EQ(detail["product"], "BTC-USD");
            EXPECT_EQ(detail["start_system_ns"], Epoch + 1); EXPECT_EQ(detail["start_time"], "2026-09-30T00:00:00.000Z");
            EXPECT_DOUBLE_EQ(detail["duration_seconds"].get<double>(), 20.999999999);
            EXPECT_EQ(btc["run_reports"][0]["empty_connections"], 1);
        }
        if (multi) {
            // A BTC update must not make the receipt-only ETH connection nonempty.
            const auto& eth = r.json["products"]["ETH-USD"];
            EXPECT_TRUE(eth["ok"]); EXPECT_EQ(eth["empty_connections"], 1);
            EXPECT_EQ(eth["missing_snapshot_connections"], 0);
        }
    }
}
TEST_F(CaptureTest, EmptyConnectionsWithForeignFramesNeedNoProductSnapshot) {
    std::vector<Record> input{record(Kind::CaptureStarted, 0, "{}", 0), record(Kind::TransportUp, 1),
        frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 1000000000),
        record(Kind::CaptureStopped, 2000000000)};
    writeMulti(config, input);
    const auto r = verify(config.root);
    ASSERT_TRUE(r.ok) << r.json.dump(2); EXPECT_EQ(r.exitCode(), 0);
    EXPECT_EQ(r.json["products"]["BTC-USD"]["empty_connections"], 0);
    EXPECT_EQ(r.json["products"]["ETH-USD"]["empty_connections"], 1);
    EXPECT_EQ(r.json["products"]["ETH-USD"]["l2_events"], 0);
    EXPECT_EQ(r.json["empty_connections"], 1); EXPECT_EQ(r.json["empty_connection_details"].size(), 1);
    EXPECT_EQ(r.json["empty_connection_details"][0]["product"], "ETH-USD");
}
TEST_F(CaptureTest, EmptyConnectionsDetailsBoundedAndCountedOnce) {
    Writer writer(config, metadata()); writer.append(record(Kind::CaptureStarted, 0, "{}", 0));
    for (uint64_t id = 1; id <= 40; ++id) {
        writer.append(record(Kind::TransportUp, id * 1000000000LL, "{}", id));
        writer.append(record(Kind::TransportDown, id * 1000000000LL + 100, "{}", id));
    }
    writer.append(record(Kind::TransportUp, 41000000000LL, "{}", 41));
    writer.append(frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 42000000000LL, 41));
    writer.append(record(Kind::CaptureStopped, 43000000000LL, "{}", 41)); writer.close();
    const auto r = verify(config.root);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["empty_connections"], 40); EXPECT_EQ(r.json["missing_snapshot_connections"], 0);
    EXPECT_EQ(r.json["empty_connection_details"].size(), 30);
    EXPECT_EQ(r.json["totals"]["empty_connection_details"].size(), 30);
    EXPECT_EQ(r.json["empty_connection_details"][29]["connection"], 30);
    EXPECT_TRUE(r.json["details"].empty());
}
TEST_F(CaptureTest, EmptyConnectionsOpenPrefixUsesObservedDurationAndKeepsExitThree) {
    {
        Writer writer(config, metadata());
        writer.append(record(Kind::CaptureStarted, 0, "{}", 0)); writer.append(record(Kind::TransportUp, 1));
        writer.append(frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 1000000000));
        writer.append(record(Kind::TransportDown, 2000000000));
        writer.append(record(Kind::TransportUp, 3000000000, "{}", 2));
        writer.append(frame({{"channel", "heartbeats"}}, 0, 6000000000, 2)); writer.flush();
    }
    const auto r = verify(config.root);
    ASSERT_TRUE(r.ok) << r.json.dump(2); EXPECT_EQ(r.exitCode(), 3);
    EXPECT_EQ(r.json["empty_connections"], 1); EXPECT_EQ(r.json["missing_snapshot_connections"], 0);
    ASSERT_EQ(r.json["empty_connection_details"].size(), 1);
    EXPECT_EQ(r.json["empty_connection_details"][0]["connection"], 2);
    EXPECT_DOUBLE_EQ(r.json["empty_connection_details"][0]["duration_seconds"].get<double>(), 3.0);
}
TEST_F(CaptureTest, MultiSnapshotAnchorsAreIndependentOnEveryConnection) {
    for (const auto mode : {0, 1, 2}) {
        QTemporaryDir root;
        auto cfg = config; cfg.root = root.path();
        auto input = multiFixture();
        if (mode == 0) {
            // BTC's snapshot cannot anchor an ETH update.
            auto eth = nlohmann::json::parse(input[4].payload);
            eth["events"][0]["type"] = "update";
            input[4].payload = eth.dump();
        } else {
            input.pop_back();
            input.push_back(record(Kind::TransportDown, 2200000000));
            input.push_back(record(Kind::TransportUp, 2300000000, "{}", 2));
            input.push_back(frame(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}), 0, 2400000000, 2));
            input.push_back(frame(mode == 1 ? fixtures::coinbaseL2Update("ETH-USD", {{"bid", 10, 1}}) :
                fixtures::coinbaseL2Snapshot("ETH-USD", {{10, 1}}, {{11, 1}}), 1, 2500000000, 2));
            input.push_back(record(Kind::CaptureStopped, 3000000000, "{}", 2));
        }
        writeMulti(cfg, input);
        const auto report = verify(cfg.root);
        EXPECT_EQ(report.ok, mode == 2) << report.json.dump(2);
        EXPECT_EQ(report.json["products"]["BTC-USD"]["unanchored_l2_events"], 0);
        if (mode < 2) {
            EXPECT_GE(report.json["products"]["ETH-USD"]["unanchored_l2_events"].get<int>(), 1);
            EXPECT_EQ(report.json["products"]["ETH-USD"]["missing_snapshot_connections"], 1);
        }
    }
}
TEST_F(CaptureTest, MultiConnectionInvalidationClearsAllAnchors) {
    auto input = multiFixture();
    input.insert(input.begin() + 6, record(Kind::BookInvalidated, 1200000000, R"({"product":"","reason":"sequence gap"})"));
    writeMulti(config, input);
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    for (const auto& symbol : {"BTC-USD", "ETH-USD"})
        EXPECT_EQ(report.json["products"][symbol]["unanchored_l2_events"], 1);
}
TEST_F(CaptureTest, MultiBroadcastsUnclassifiedAndMalformedFramesWithoutChangingBytes) {
    auto input = multiFixture();
    input.insert(input.end() - 1, record(Kind::Frame, 2200000000, std::string(" \nmalformed\0json", 16)));
    input.insert(input.end() - 1, record(Kind::Frame, 2300000000, R"({"channel":"future","sequence_num":6,"product_id":"ETH-USD"})"));
    writeMulti(config, input);
    for (const auto& path : paths(config.root)) {
        std::vector<Record> last;
        scan(path, [&](const auto& r) { if ((r.kind == Kind::Frame || r.kind == Kind::CaptureStopped) && r.time.steadyNs >= input[input.size()-3].time.steadyNs) last.push_back(r); });
        ASSERT_EQ(last.size(), 3);
        EXPECT_EQ(last[0], input[input.size()-3]); EXPECT_EQ(last[1], input[input.size()-2]);
    }
    EXPECT_FALSE(verify(config.root).ok); // preservation does not bless malformed JSON
}
TEST_F(CaptureTest, OpenPrefixAcceptsOnlyTerminalUnfinishedDataAndStillRejectsActualGaps) {
    auto input = fixture(); input.pop_back();
    Writer writer(config, metadata());
    for (const auto& r : input) writer.append(r);
    writer.flush();
    const auto path = writer.currentPath();
    auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["open_runs"], 1); EXPECT_EQ(report.json["complete"], false);
    EXPECT_EQ(report.json["unindexed_files"], 1);
    // An observed terminal partial write is in progress, with its recoverable prefix explicit.
    const auto prefix = contents(path);
    save(path, prefix + QByteArray("BLK1\0", 5));
    report = verify(config.root);
    EXPECT_TRUE(report.ok); EXPECT_EQ(report.json["torn_tails"], 1);
    save(path, prefix);
    writer.append(frame(nlohmann::json{{"channel", "heartbeats"}}, 7, 2500000000)); writer.flush();
    report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_EQ(report.json["sequence_gaps"], 1);
}
TEST_F(CaptureTest, SingleProductKeepsV1FileAndExistingVerifyFields) {
    Session session(config, metadata());
    const auto input = fixture();
    for (const auto& r : input) ASSERT_TRUE(session.submit(r));
    session.close();
    ASSERT_EQ(paths(config.root).size(), 1);
    const auto path = paths(config.root).front();
    EXPECT_TRUE(path.endsWith("/BTC-USD/2026/09/30/00.rawl2"));
    EXPECT_EQ(contents(path).left(8), QByteArray("RAWL2\r\n\1", 8));
    std::vector<Record> output;
    const auto result = scan(path, [&](const auto& r) { output.push_back(r); });
    EXPECT_EQ(output, input);
    EXPECT_FALSE(result.header.contains("connection_products"));
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok); EXPECT_EQ(report.json["frames"], 5);
    EXPECT_EQ(report.json["snapshots"], 1); EXPECT_EQ(report.json["replayed_l2_events"], 2);
    EXPECT_EQ(report.json["fps_p99_one_second_buckets"], 2);
    EXPECT_EQ(report.json["products"]["BTC-USD"]["frames"], report.json["frames"]);
    EXPECT_EQ(report.json["totals"]["received_bytes"], report.json["received_bytes"]);
}

TEST_F(CaptureTest, MultiWholeRootRejectsMissingStreamsAndReceiptsThatDoNotMatchRawBytes) {
    writeMulti(config, multiFixture());
    const auto originalPaths = paths(config.root);
    for (const auto mutation : {0, 1, 2, 3}) {
        QTemporaryDir altered;
        for (const auto& path : originalPaths) {
            const auto header = readHeader(path);
            const auto symbol = header["product_metadata"]["product_id"].get<std::string>();
            if (mutation == 3 && symbol == "ETH-USD") continue;
            auto cfg = config; cfg.root = altered.path(); cfg.symbol = symbol;
            auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
            scan(path, [&](const Record& original) {
                auto r = original;
                if (symbol == "BTC-USD" && r.kind == Kind::Frame) {
                    const auto envelope = nlohmann::json::parse(r.payload);
                    if (envelope["sequence_num"] == 1 && mutation == 0) r.payload += " ";
                    if (envelope["sequence_num"] == 4 && mutation == 1) ++r.time.systemNs;
                }
                if (symbol == "BTC-USD" && r.kind == Kind::FrameReference && mutation == 2) {
                    auto receipt = nlohmann::json::parse(r.payload);
                    receipt["sha256"] = std::string(64, '0');
                    r.payload = receipt.dump();
                }
                writer.append(r);
            });
            writer.close();
        }
        const auto report = verify(altered.path());
        EXPECT_FALSE(report.ok) << mutation << report.json.dump(2);
        EXPECT_GT(report.json["routing_errors"].get<int>(), 0);
        // Local sequences and books remain valid; only root comparison can see this loss/mismatch.
        if (mutation != 0) EXPECT_TRUE(report.json["products"]["BTC-USD"]["ok"].get<bool>());
    }
}
TEST_F(CaptureTest, MultiHourRotationAndMixedLegacyRunsKeepConnectionIdentity) {
    auto input = multiFixture();
    for (auto& r : input) r.time.systemNs += Hour - 500000000;
    writeMulti(config, input);
    Writer legacy(config, metadata());
    for (const auto& r : fixture()) legacy.append(r);
    legacy.close();
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["totals"]["runs"], 2);
    EXPECT_EQ(report.json["totals"]["frames"], 11);
    EXPECT_EQ(report.json["totals"]["connections"], 2);
    EXPECT_EQ(report.json["products"]["BTC-USD"]["snapshots"], 2);
    EXPECT_EQ(report.json["products"]["ETH-USD"]["snapshots"], 1);
}

TEST_F(CaptureTest, MultiDailyCountsFollowReceiveUtcDayAcrossMidnight) {
    auto input = multiFixture();
    for (auto& r : input) r.time.systemNs += 24 * Hour - 500000000;
    writeMulti(config, input);
    const auto report = verify(config.root);
    ASSERT_TRUE(report.ok) << report.json.dump(2);
    const auto& days = report.json["totals"]["days"];
    ASSERT_EQ(days.size(), 2);
    EXPECT_EQ(days["2026-09-30"]["frames"], 2);
    EXPECT_EQ(days["2026-10-01"]["frames"], 4);
    EXPECT_EQ(days["2026-09-30"]["l2_events"], 1);
    EXPECT_EQ(days["2026-10-01"]["l2_events"], 3);
    EXPECT_GT(days["2026-09-30"]["file_bytes"].get<int>(), 0);
    EXPECT_GT(days["2026-10-01"]["file_bytes"].get<int>(), 0);
    EXPECT_EQ(report.json["products"]["BTC-USD"]["days"]["2026-10-01"]["frames"], 2);
}

TEST_F(CaptureTest, SupersededStoplessRunIsInterruptedEvenWhenVerifyingAMonth) {
    {
        Writer old(config, metadata());
        auto input = fixture(); input.pop_back();
        for (const auto& r : input) old.append(r);
        old.flush();
    }
    EXPECT_TRUE(verify(config.root).ok);
    Writer newer(config, metadata());
    for (const auto& r : fixture()) newer.append(r);
    newer.close();
    for (const auto& path : {config.root, config.root + "/BTC-USD/2026/09"}) {
        const auto report = verify(path);
        EXPECT_FALSE(report.ok);
        EXPECT_EQ(report.json["interrupted_runs"], 1);
        EXPECT_EQ(report.json["open_runs"], 0);
        EXPECT_EQ(report.json["ok_closed_runs"], false);
    }
}
TEST_F(CaptureTest, ProductMonthScopeDefersUnavailablePeerChecks) {
    writeMulti(config, multiFixture());
    const auto report = verify(config.root + "/BTC-USD/2026/09");
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["scope"], "product");
    EXPECT_EQ(report.json["routing_errors"], 0);
    EXPECT_EQ(report.json["routing_checks_deferred"], 1);
}
TEST_F(CaptureTest, InterruptedRunChecksReceiptAgainstLostRawAtCommonPrefix) {
    QTemporaryDir source;
    auto src = config; src.root = source.path(); writeMulti(src, multiFixture());
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto crashedHolder = rewriteWriter(cfg, header); auto& crashed = *crashedHolder;
        bool lost = false;
        scan(path, [&](const Record& r) {
            if (r.kind == Kind::CaptureStopped) return;
            // ETH loses its final raw block AND the as-yet-unflushed receipt.
            // BTC's durable receipt still certifies that those frames existed.
            if (cfg.symbol == "ETH-USD" && r.kind == Kind::Frame &&
                nlohmann::json::parse(r.payload).at("sequence_num") == 2) lost = true;
            if (!lost) crashed.append(r);
        });
        crashed.flush(); // no index, no stop
    }
    // A newer run in just ONE member makes the shared old run interrupted.
    Writer newer(config, metadata());
    for (const auto& r : fixture()) newer.append(r);
    newer.close();
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["totals"]["interrupted_runs"], 1);
    EXPECT_EQ(report.json["totals"]["open_runs"], 0);
    EXPECT_GT(report.json["routing_errors"].get<int>(), 0) << report.json.dump(2);
    EXPECT_TRUE(report.json["connection_runs"][0]["routing_checked"].get<bool>());
}
TEST_F(CaptureTest, ScopeTailIsTruncatedRatherThanInterruptedOrOpen) {
    {
        Writer writer(config, metadata());
        auto input = fixture(); input.back().time.systemNs += 24 * Hour;
        for (const auto& r : input) writer.append(r);
        writer.close();
    }
    // Ensure the scope classification is independent of newest-run status.
    Writer newer(config, metadata());
    auto input = fixture();
    for (auto& r : input) { r.time.systemNs += 48 * Hour; newer.append(r); }
    newer.close();
    for (const auto& scope : {config.root + "/BTC-USD/2026/09", paths(config.root).front()}) {
        const auto report = verify(scope);
        EXPECT_TRUE(report.ok) << report.json.dump(2);
        EXPECT_EQ(report.json["complete"], true);
        EXPECT_EQ(report.json["truncated_by_scope_runs"], 1);
        EXPECT_EQ(report.json["interrupted_runs"], 0);
        EXPECT_EQ(report.json["open_runs"], 0);
        EXPECT_EQ(report.json["connection_runs"][0]["truncated_by_scope"], true);
    }
    EXPECT_EQ(verify(config.root).json["truncated_by_scope_runs"], 0);
}
TEST_F(CaptureTest, TruncatedScopeRejectsUnindexedAndTornSelectedTail) {
    Writer writer(config, metadata());
    auto input = fixture(); input.back().time.systemNs += 24 * Hour;
    for (const auto& r : input) writer.append(r);
    writer.close();
    const auto selected = paths(config.root).front();
    const auto original = contents(selected);
    const auto scanResult = scan(selected);
    const auto& last = scanResult.index.back();
    const auto end = last.offset + 48 + last.compressedBytes;
    for (const auto& suffix : {QByteArray{}, QByteArray("BLK1\0", 5)}) {
        save(selected, original.first(end) + suffix);
        const auto report = verify(config.root + "/BTC-USD/2026/09");
        EXPECT_FALSE(report.ok);
        EXPECT_EQ(report.json["complete"], false);
        EXPECT_EQ(report.json["truncated_by_scope_runs"], 1);
        EXPECT_EQ(report.json["run_reports"][0]["bad_tails"], 1);
        EXPECT_EQ(report.json["torn_tails"], suffix.isEmpty() ? 0 : 1);
    }
}
TEST_F(CaptureTest, AnotherRunOutsideScopeDoesNotTruncateSelectedRun) {
    {
        Writer writer(config, metadata());
        auto input = fixture(); input.pop_back();
        for (const auto& r : input) writer.append(r);
        writer.flush();
    }
    {
        Writer writer(config, metadata());
        for (auto r : fixture()) { r.time.systemNs += 24 * Hour; writer.append(r); writer.sealSegment(); }
        writer.close();
    }
    const auto report = verify(config.root + "/BTC-USD/2026/09");
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["interrupted_runs"], 1);
    EXPECT_EQ(report.json["truncated_by_scope_runs"], 0);
}
TEST_F(CaptureTest, AnotherProductsLaterSegmentDoesNotTruncateSelectedRun) {
    QTemporaryDir source;
    auto src = config; src.root = source.path(); writeMulti(src, multiFixture());
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
        scan(path, [&](Record r) {
            if (cfg.symbol == "ETH-USD") r.time.systemNs += 24 * Hour;
            if (r.kind != Kind::CaptureStopped) writer.append(r);
        });
        if (cfg.symbol == "ETH-USD") {
            writer.sealSegment();
            auto stop = record(Kind::CaptureStopped, 3000000000);
            stop.time.systemNs += 24 * Hour;
            writer.append(stop);
        }
        writer.close();
    }
    const auto report = verify(config.root + "/BTC-USD/2026/09");
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["open_runs"], 1);
    EXPECT_EQ(report.json["complete"], false);
    EXPECT_EQ(report.json["truncated_by_scope_runs"], 0);
}
TEST_F(CaptureTest, MultiProductTruncatedScopeDefersPeerProofButChecksItsSealedTail) {
    auto input = multiFixture(); input.back().time.systemNs += 24 * Hour;
    writeMulti(config, input);
    for (const auto& symbol : {"BTC-USD", "ETH-USD"}) {
        const auto report = verify(config.root + '/' + symbol + "/2026/09");
        EXPECT_TRUE(report.ok) << report.json.dump(2);
        EXPECT_EQ(report.json["complete"], true);
        EXPECT_EQ(report.json["truncated_by_scope_runs"], 1);
        EXPECT_EQ(report.json["routing_checks_deferred"], 1);
        EXPECT_EQ(report.json["connection_runs"][0]["routing_checked"], false);
        EXPECT_EQ(report.json["connection_runs"][0]["products"].size(), 2);
        EXPECT_EQ(report.json["run_reports"][0]["bad_tails"], 0);
    }
    EXPECT_TRUE(verify(config.root).ok);
}
TEST_F(CaptureTest, MultiProductRotationAfterSelectionComparesOnlyScopedCommonPrefix) {
    QTemporaryDir source;
    auto src = config; src.root = source.path(); writeMulti(src, multiFixture());
    std::vector<std::pair<std::string, std::unique_ptr<Writer>>> writers;
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writer = rewriteWriter(cfg, header);
        scan(path, [&](const Record& r) { if (r.kind != Kind::CaptureStopped) writer->append(r); });
        // Product writers can have different observed end positions during
        // rotation. This marker has not reached ETH's selected segment yet.
        if (cfg.symbol == "BTC-USD") writer->append(record(Kind::BookInvalidated, 3000000000));
        writer->sealSegment();
        writers.emplace_back(cfg.symbol, std::move(writer));
    }
    const auto report = verify(config.root, [&] {
        // Deterministically model new segments appearing after selection, but
        // before the inventory that classifies truncated/open/interrupted runs.
        for (auto& [symbol, writer] : writers) {
            if (symbol == "ETH-USD") writer->append(record(Kind::BookInvalidated, 3000000000));
            writer->append(record(Kind::CaptureStopped, 4000000000));
            writer->close();
        }
    });
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["complete"], true);
    EXPECT_EQ(report.json["files"], 2);
    EXPECT_EQ(report.json["inventory_files"], 4);
    EXPECT_EQ(report.json["totals"]["truncated_by_scope_runs"], 1);
    EXPECT_EQ(report.json["totals"]["interrupted_runs"], 0);
    EXPECT_EQ(report.json["routing_errors"], 0);
    EXPECT_EQ(report.json["connection_runs"][0]["routing_checked"], true);
    EXPECT_TRUE(verify(config.root).ok); // the complete archive has matching markers
}
TEST_F(CaptureTest, InterruptedComparisonStopsAfterFirstPeersFinalGroup) {
    QTemporaryDir source;
    auto src = config; src.root = source.path();
    auto input = multiFixture(); input.pop_back();
    for (uint64_t i = 0; i < 12; ++i)
        input.push_back(frame(nlohmann::json{{"channel", "heartbeats"}}, 6 + i, (i + 1) * RoutingIntervalNs));
    input.push_back(record(Kind::CaptureStopped, 14 * RoutingIntervalNs));
    writeMulti(src, input);
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
        bool ended = false;
        scan(path, [&](const Record& r) {
            if (ended || r.kind == Kind::CaptureStopped) return;
            writer.append(r);
            if (cfg.symbol == "ETH-USD" && r.kind == Kind::FrameReference) ended = true;
        });
        writer.flush();
    }
    Writer newer(config, metadata());
    for (const auto& r : fixture()) newer.append(r);
    newer.close();
    const auto report = verify(config.root);
    EXPECT_EQ(report.json["totals"]["interrupted_runs"], 1);
    // Check the available receipt in the first group where ETH ends, but do
    // not manufacture more missing ETH copies from every later BTC group.
    EXPECT_EQ(report.json["routing_errors"], 1) << report.json.dump(2);
    EXPECT_NE(report.json["routing_details"].dump().find("missing or wrongly routed raw copy product=ETH-USD"), std::string::npos);
}
TEST_F(CaptureTest, ExternalSortOrdersDistinctRunsAndSegmentsAcrossMultipleLevels) {
    constexpr int Runs = 3, Segments = 701; // two full sort chunks plus a remainder
    std::vector<std::pair<int64_t, std::string>> expected;
    for (int run = 0; run < Runs; ++run) {
        Writer writer(config, metadata());
        auto input = fixture(); input.pop_back();
        for (const auto& r : input) writer.append(r);
        writer.flush();
        const auto header = readHeader(writer.currentPath());
        expected.emplace_back(header.at("run_started_system_ns").get<int64_t>(), header.at("run_id").get<std::string>());
        for (int segment = 1; segment < Segments; ++segment) {
            writer.sealSegment();
            writer.append(frame(nlohmann::json{{"channel", "heartbeats"}}, 4 + segment, int64_t(3 + segment) * 1000000000));
        }
        writer.append(record(Kind::CaptureStopped, int64_t(3 + Segments) * 1000000000));
        writer.close();
    }
    // Inventory order and path order both differ from numeric run/segment
    // order. Modulo permutation interleaves the runs without changing headers.
    const auto files = paths(config.root);
    ASSERT_EQ(files.size(), Runs * Segments);
    for (int i = 0; i < files.size(); ++i)
        ASSERT_TRUE(QFile::rename(files[i], config.root + '/' + QString::number((i * 17) % files.size()) + ".rawl2"));
    std::sort(expected.begin(), expected.end());
    const auto report = verify(config.root);
    ASSERT_TRUE(report.ok) << report.json["details"].dump();
    EXPECT_EQ(report.json["files"], Runs * Segments);
    EXPECT_EQ(report.json["inventory_peak_buffered_files"], 1024);
    ASSERT_EQ(report.json["run_reports"].size(), Runs);
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(report.json["run_reports"][i]["run_id"], expected[i].second);
        EXPECT_EQ(report.json["run_reports"][i]["frames"], Segments + 4);
    }
}
TEST_F(CaptureTest, EmptyAndUnreadableArchivesRetainNoFramesDetail) {
    auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["errors"], 1);
    EXPECT_EQ(report.json["details"], nlohmann::json::array({"no captured frames verified"}));
    for (int i = 0; i < 34; ++i) save(config.root + '/' + QString::number(i) + ".rawl2", "bad header");
    report = verify(config.root);
    EXPECT_EQ(report.json["errors"], 35);
    EXPECT_EQ(report.json["details"].size(), 30);
    EXPECT_EQ(report.json["details"][0], "no captured frames verified");
}
TEST_F(CaptureTest, RoutingChecksContinueAfterEveryBadGroupWithBoundedDetails) {
    QTemporaryDir source;
    auto src = config; src.root = source.path();
    auto input = multiFixture(); input.pop_back();
    for (uint64_t i = 0; i < 40; ++i)
        input.push_back(frame(nlohmann::json{{"channel", "heartbeats"}}, 6 + i, (i + 1) * RoutingIntervalNs));
    input.push_back(record(Kind::CaptureStopped, 42 * RoutingIntervalNs));
    writeMulti(src, input);
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
        scan(path, [&](const Record& r) {
            if (cfg.symbol == "BTC-USD" && r.kind == Kind::Frame &&
                nlohmann::json::parse(r.payload).at("sequence_num").get<uint64_t>() >= 6) return;
            writer.append(r);
        });
        writer.close();
    }
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["routing_errors"], 40); // one missing BTC copy in each independently proven group
    EXPECT_EQ(report.json["routing_details"].size(), 30);
}
TEST_F(CaptureTest, UnsequencedBroadcastUsesStreamOrderAndDoesNotAbortRoutingProof) {
    auto input = multiFixture();
    // Same steady clock as neighboring frames tests ordering by stream position.
    input.insert(input.begin() + 4, record(Kind::Frame, 200, R"({"type":"error","message":"one"})"));
    input.insert(input.begin() + 5, record(Kind::Frame, 200, R"({"type":"error","message":"two"})"));
    writeMulti(config, input);
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); // unsequenced input remains visible as an integrity anomaly
    EXPECT_EQ(report.json["totals"]["unsequenced_frames"], 2);
    EXPECT_EQ(report.json["routing_errors"], 0) << report.json.dump(2);
}
TEST_F(CaptureTest, RepeatedAndProductOnlyUnsequencedFramesRetainRoutingOrder) {
    auto input = multiFixture();
    auto btc = fixtures::coinbaseL2Update("BTC-USD", {{"bid", 99, 4}});
    auto eth = fixtures::coinbaseL2Update("ETH-USD", {{"bid", 9, 4}});
    btc.erase("sequence_num"); eth.erase("sequence_num");
    const auto broadcast = record(Kind::Frame, 2000000000, R"({"type":"error"})");
    input.insert(input.end() - 2, {record(Kind::Frame, 2000000000, btc.dump()), broadcast,
        record(Kind::Frame, 2000000000, eth.dump()), broadcast});
    writeMulti(config, input);
    const auto report = verify(config.root);
    EXPECT_EQ(report.json["routing_errors"], 0) << report.json.dump(2);
}
TEST_F(CaptureTest, InterruptedUnreceiptedMergedTailChecksSequenceContinuity) {
    QTemporaryDir source;
    auto src = config; src.root = source.path(); writeMulti(src, multiFixture());
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
        scan(path, [&](const Record& r) {
            if (r.kind == Kind::FrameReference || r.kind == Kind::CaptureStopped) return;
            if (r.kind == Kind::Frame && nlohmann::json::parse(r.payload).at("sequence_num") == 2) return;
            writer.append(r);
        });
        writer.flush();
    }
    Writer newer(config, metadata());
    for (const auto& r : fixture()) newer.append(r);
    newer.close();
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.json["routing_errors"], 1) << report.json.dump(2);
    EXPECT_NE(report.json["routing_details"].dump().find("merged tail sequence gap expected=2 got=3"), std::string::npos);
}
TEST_F(CaptureTest, InterruptedMergedTailChecksFirstSequenceAfterDurableReceipt) {
    QTemporaryDir source;
    auto src = config; src.root = source.path(); writeMulti(src, multiFixture());
    for (const auto& path : paths(src.root)) {
        const auto header = readHeader(path);
        auto cfg = config; cfg.symbol = header["product_metadata"]["product_id"].get<std::string>();
        auto writerHolder = rewriteWriter(cfg, header); auto& writer = *writerHolder;
        scan(path, [&](const Record& r) { if (r.kind != Kind::CaptureStopped) writer.append(r); });
        writer.append(frame(nlohmann::json{{"channel", "heartbeats"}}, 8, 4000000000));
        writer.flush();
    }
    Writer newer(config, metadata());
    for (const auto& r : fixture()) newer.append(r);
    newer.close();
    const auto report = verify(config.root);
    EXPECT_EQ(report.json["routing_errors"], 1) << report.json.dump(2);
    EXPECT_NE(report.json["routing_details"].dump().find("merged tail sequence gap expected=6 got=8"), std::string::npos);
}
TEST_F(CaptureTest, InventoryOver100000FilesHasBoundedBuffersAtEveryScope) {
    Writer writer(config, metadata());
    for (const auto& r : fixture()) writer.append(r);
    writer.close();
    const auto selected = paths(config.root).front();
    // Header-only historical segments suffice to exercise discovery and replay
    // without 100k compressed payloads. Hard links keep fixture creation cheap.
    const auto headerSize = scan(selected).index.front().offset;
    const auto outside = config.root + "/BTC-USD/2025/01";
    ASSERT_TRUE(QDir().mkpath(outside));
    const auto seed = outside + "/seed.rawl2";
    save(seed, contents(selected).first(headerSize));
    for (int i = 0; i < 100000; ++i) {
        const auto bucket = outside + '/' + QString::number(i / 1000);
        if (i % 1000 == 0) ASSERT_TRUE(QDir().mkpath(bucket));
        const auto destination = bucket + '/' + QString::number(i) + ".rawl2";
        if (i % 1000 == 0) ASSERT_TRUE(QFile::copy(seed, destination));
        else std::filesystem::create_hard_link((bucket + '/' + QString::number(i - i % 1000) + ".rawl2").toStdString(), destination.toStdString());
    }
    for (const auto& scope : {selected, config.root + "/BTC-USD/2026/09", config.root}) {
        const auto report = verify(scope);
        EXPECT_EQ(report.json["inventory_files"], 100002);
        EXPECT_LE(report.json["inventory_peak_buffered_files"].get<size_t>(), 1024);
        if (scope != config.root) {
            EXPECT_TRUE(report.ok) << report.json.dump(2);
            EXPECT_EQ(report.json["files"], 1);
        } else {
            EXPECT_FALSE(report.ok); // duplicated, unindexed historical segments are real errors
            EXPECT_EQ(report.json["files"], 100002);
            EXPECT_EQ(report.json["inventory_peak_buffered_files"], 1024);
            EXPECT_EQ(report.json["details"].size(), 30);
        }
    }
}
TEST(DecimalGrid, ExactAtomsWithoutFloatingPointOrSilentRounding) {
    DecimalGrid prices("0.01"), quantities("0.00000001");
    EXPECT_EQ(prices.atoms("123456.78000000000000000000"), 12345678);
    EXPECT_EQ(quantities.atoms("1.00000001"), 100000001);
    EXPECT_EQ(quantities.atoms("0.00000000"), 0);
    EXPECT_THROW(prices.atoms("1.001"), std::runtime_error);
    EXPECT_THROW(quantities.atoms("-1"), std::runtime_error);
    EXPECT_THROW(quantities.atoms("1e-8"), std::runtime_error);
    EXPECT_THROW(DecimalGrid("0"), std::runtime_error);
    EXPECT_THROW(DecimalGrid("0.0000000000000000001"), std::runtime_error);
}
} // namespace

namespace {
nlohmann::json tradeEvent(const char* type, const char* product, std::initializer_list<uint64_t> ids) {
    nlohmann::json trades = nlohmann::json::array();
    for (const auto id : ids) trades.push_back({{"product_id", product}, {"trade_id", std::to_string(id)},
        {"size", id % 2 ? "0.25" : "0.50"}, {"price", "100.00"}, {"side", id % 2 ? "BUY" : "SELL"},
        {"time", id % 2 ? "2026-09-29T00:00:00.1Z" : "2026-09-29T00:00:00.09Z"}});
    return {{"type", type}, {"trades", trades}};
}
struct TradeFixture {
    std::vector<Record> records{record(Kind::CaptureStarted, 0, "{}", 0)};
    uint64_t sequence = 0, connection = 0;
    void append(Kind kind, std::string payload = "{}") {
        records.push_back(record(kind, records.size() * 100, std::move(payload), connection));
    }
    void json(nlohmann::json value) {
        value["sequence_num"] = sequence++;
        append(Kind::Frame, value.dump());
    }
    void up(bool multi = false) {
        if (connection) append(Kind::TransportDown);
        ++connection; sequence = 0; append(Kind::TransportUp);
        json(fixtures::coinbaseL2Snapshot("BTC-USD", {{100, 1}}, {{101, 1}}));
        if (multi) json(fixtures::coinbaseL2Snapshot("ETH-USD", {{10, 1}}, {{11, 1}}));
    }
    void trades(std::initializer_list<nlohmann::json> events) {
        json({{"channel", "market_trades"}, {"events", events}});
    }
    VerificationReport verifyAt(const WriterConfig& config, bool multi = false) {
        append(Kind::CaptureStopped);
        if (multi) writeMulti(config, records);
        else {
            Writer writer(config, metadata());
            for (const auto& value : records) writer.append(value);
            writer.close();
        }
        return verify(config.root);
    }
};
}
TEST_F(CaptureTest, TradeContiguousDescendingUpdatesAndAggressorTotalsV1) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {101, 100})});
    f.trades({tradeEvent("update", "BTC-USD", {104, 103, 102})});
    f.trades({tradeEvent("update", "BTC-USD", {105})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["trades"], 6);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
    EXPECT_EQ(r.json["reconnect_trade_gaps"], 0);
    EXPECT_EQ(r.json["duplicate_trades"], 0);
    EXPECT_EQ(r.json["buy_trades"], 3); EXPECT_EQ(r.json["sell_trades"], 3);
    EXPECT_EQ(r.json["buy_volume"], "1.5"); EXPECT_EQ(r.json["sell_volume"], "0.75");
    EXPECT_EQ(r.json["first_trade_time"], "2026-09-29T00:00:00.090000000Z");
    EXPECT_EQ(r.json["last_trade_time"], "2026-09-29T00:00:00.100000000Z");
    EXPECT_EQ(readHeader(paths(config.root).front())["format_version"], 1);
}
TEST_F(CaptureTest, TradeWithinConnectionGapDoesNotFailIntegrity) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {100})});
    f.trades({tradeEvent("update", "BTC-USD", {103, 102})});
    f.trades({tradeEvent("update", "BTC-USD", {106})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2); EXPECT_TRUE(r.json["ok_closed_runs"]);
    EXPECT_FALSE(r.json["trade_tape_complete"]);
    EXPECT_EQ(r.exitCode(), 0); EXPECT_EQ(r.exitCode(true), 2);
    EXPECT_TRUE(r.json["details"].empty()); EXPECT_EQ(r.json["errors"], 0);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 2);
    EXPECT_EQ(r.json["upstream_missing_trades"], 3);
    EXPECT_EQ(r.json["reconnect_trade_gaps"], 0);
    const auto& detail = r.json["upstream_trade_gap_details"][0];
    EXPECT_EQ(detail["product"], "BTC-USD"); EXPECT_EQ(detail["connection"], 1);
    EXPECT_EQ(detail["first_id"], "101"); EXPECT_EQ(detail["count"], 1);
    EXPECT_EQ(detail["approx_time"], "2026-09-29T00:00:00.090000000Z");
}
TEST_F(CaptureTest, TradeSnapshotOverlapAndUpdateDuplicatesCountOnce) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {101, 100})});
    f.trades({tradeEvent("update", "BTC-USD", {102})});
    f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {103, 102, 101, 100, 99})});
    f.trades({tradeEvent("update", "BTC-USD", {104, 103, 103})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["trades"], 6); EXPECT_EQ(r.json["duplicate_trades"], 5);
    EXPECT_EQ(r.json["buy_volume"], "1.5"); EXPECT_EQ(r.json["sell_volume"], "0.75");
    EXPECT_EQ(r.json["upstream_trade_gaps"], 0); EXPECT_EQ(r.json["reconnect_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeReconnectMissingCountIsNotCorruption) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {106, 105})});
    f.trades({tradeEvent("update", "BTC-USD", {107})});
    f.up();
    f.trades({tradeEvent("update", "BTC-USD", {110})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["trades"], 5); EXPECT_EQ(r.json["reconnect_trade_gaps"], 2);
    EXPECT_EQ(r.json["reconnect_missing_trades"], 6);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
    EXPECT_EQ(r.json["reconnect_trade_gap_details"][0]["first_id"], "101");
    EXPECT_EQ(r.json["reconnect_trade_gap_details"][0]["count"], 4);
    EXPECT_EQ(r.json["reconnect_trade_gap_details"][0]["connection"], 2);
}
TEST_F(CaptureTest, TradeMultiProductIsolationV2MixedEnvelopeAndGap) {
    TradeFixture f; f.up(true);
    f.trades({tradeEvent("snapshot", "BTC-USD", {101, 100}), tradeEvent("snapshot", "ETH-USD", {501, 500})});
    f.trades({tradeEvent("update", "BTC-USD", {102}), tradeEvent("update", "ETH-USD", {503})});
    const auto r = f.verifyAt(config, true);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.json["trade_tape_complete"]);
    const auto& btc = r.json["products"]["BTC-USD"];
    const auto& eth = r.json["products"]["ETH-USD"];
    EXPECT_EQ(btc["ok"], true); EXPECT_EQ(btc["trades"], 3); EXPECT_EQ(btc["upstream_trade_gaps"], 0);
    EXPECT_EQ(eth["ok"], true); EXPECT_EQ(eth["trade_tape_complete"], false); EXPECT_EQ(eth["trades"], 3); EXPECT_EQ(eth["upstream_trade_gaps"], 1);
    EXPECT_EQ(eth["upstream_trade_gap_details"][0]["first_id"], "502");
    EXPECT_EQ(r.json["totals"]["trades"], 6);
    EXPECT_EQ(btc["buy_volume"], "1"); EXPECT_EQ(eth["sell_volume"], "0.5");
    EXPECT_EQ(readHeader(paths(config.root).front())["format_version"], 2);
}
TEST_F(CaptureTest, TradeSnapshotHolesAreSeparateAndPreviouslyMissingIdIsNotDuplicate) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {102, 100})});
    f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {102, 101, 100})});
    f.trades({tradeEvent("update", "BTC-USD", {103})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["trades"], 4); EXPECT_EQ(r.json["duplicate_trades"], 2);
    EXPECT_EQ(r.json["snapshot_trade_gaps"], 0); EXPECT_EQ(r.json["snapshot_missing_trades"], 0);
    EXPECT_EQ(r.json["filled_later"], 1); EXPECT_EQ(r.json["trade_tape_complete"], true);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeMalformedScalarsFailRatherThanSilentlySkipping) {
    for (const auto* key : {"trade_id", "time", "size", "side", "product_id"}) {
        QTemporaryDir temp; auto cfg = config; cfg.root = temp.path();
        TradeFixture f; f.up();
        auto event = tradeEvent("update", "BTC-USD", {100});
        event["trades"][0][key] = "invalid";
        f.trades({event});
        const auto r = f.verifyAt(cfg);
        EXPECT_FALSE(r.ok) << key << r.json.dump(2);
        EXPECT_EQ(r.json["trades"], 0);
    }
}
TEST_F(CaptureTest, TradeV2ReconnectOverlapRemainsContiguousAfterDuplicateOnlyUpdate) {
    TradeFixture f; f.up(true);
    f.trades({tradeEvent("snapshot", "BTC-USD", {100, 99}), tradeEvent("snapshot", "ETH-USD", {9007199254740993ULL})});
    f.trades({tradeEvent("update", "BTC-USD", {103, 102, 101}), tradeEvent("update", "ETH-USD", {9007199254740994ULL})});
    f.up(true);
    f.trades({tradeEvent("snapshot", "BTC-USD", {100, 99}), tradeEvent("snapshot", "ETH-USD", {9007199254740994ULL})});
    f.trades({tradeEvent("update", "BTC-USD", {101})});
    f.trades({tradeEvent("update", "BTC-USD", {105, 104}), tradeEvent("update", "ETH-USD", {9007199254740995ULL})});
    const auto r = f.verifyAt(config, true);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["totals"]["trades"], 10);
    EXPECT_EQ(r.json["products"]["BTC-USD"]["duplicate_trades"], 3);
    EXPECT_EQ(r.json["products"]["ETH-USD"]["duplicate_trades"], 1);
    EXPECT_EQ(r.json["totals"]["upstream_trade_gaps"], 0);
    EXPECT_EQ(r.json["totals"]["reconnect_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeContinuitySurvivesHourlySegmentsAndProcessRuns) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {100})});
    f.trades({tradeEvent("update", "BTC-USD", {101})});
    f.records.back().time.systemNs += Hour;
    f.records.back().time.steadyNs += Hour;
    f.append(Kind::CaptureStopped);
    f.records.back().time.systemNs += Hour;
    f.records.back().time.steadyNs += Hour;
    {
        Writer writer(config, metadata());
        for (const auto& value : f.records) writer.append(value);
        writer.close();
    }
    TradeFixture next; next.up();
    next.trades({tradeEvent("snapshot", "BTC-USD", {105, 104})});
    next.trades({tradeEvent("update", "BTC-USD", {106})});
    const auto r = next.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["files"], 3); EXPECT_EQ(r.json["runs"], 2);
    EXPECT_EQ(r.json["trades"], 5); EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
    EXPECT_EQ(r.json["reconnect_trade_gaps"], 1); EXPECT_EQ(r.json["reconnect_missing_trades"], 2);
}
TEST_F(CaptureTest, TradeIdOverflowFails) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    auto event = tradeEvent("update", "BTC-USD", {101});
    event["trades"][0]["trade_id"] = "18446744073709551616";
    f.trades({event});
    const auto r = f.verifyAt(config);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.json["trades"], 1);
}
TEST_F(CaptureTest, TradeMidConnectionSnapshotBridgesNewIdsAndOldSparseUpdateIsDuplicate) {
    // Real Coinbase pattern: update 100, recent snapshot 102..99, sparse old
    // update 100/99, then fresh update 103. No transport-up between them.
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100, 99})});
    f.trades({tradeEvent("snapshot", "BTC-USD", {102, 101, 100, 99})});
    f.trades({tradeEvent("update", "BTC-USD", {100, 99})});
    f.trades({tradeEvent("update", "BTC-USD", {103})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["trades"], 5); EXPECT_EQ(r.json["duplicate_trades"], 4);
    EXPECT_EQ(r.json["trade_resnapshots"], 1); EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeMidConnectionSnapshotCannotHideMissingIds) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.trades({tradeEvent("snapshot", "BTC-USD", {104, 103})});
    f.trades({tradeEvent("update", "BTC-USD", {105})});
    const auto r = f.verifyAt(config);
    EXPECT_TRUE(r.ok); EXPECT_FALSE(r.json["trade_tape_complete"]);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 1);
    EXPECT_EQ(r.json["upstream_missing_trades"], 2);
    EXPECT_EQ(r.json["reconnect_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeForeignSnapshotInMixedEnvelopeDoesNotResetOrRejectActiveProduct) {
    TradeFixture f; f.up(true);
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.trades({tradeEvent("update", "BTC-USD", {101}), tradeEvent("snapshot", "ETH-USD", {501, 500})});
    f.trades({tradeEvent("update", "BTC-USD", {102}), tradeEvent("update", "ETH-USD", {502})});
    const auto r = f.verifyAt(config, true);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["products"]["BTC-USD"]["trades"], 3);
    EXPECT_EQ(r.json["products"]["ETH-USD"]["trades"], 3);
    EXPECT_EQ(r.json["products"]["BTC-USD"]["trade_snapshots"], 0);
    EXPECT_EQ(r.json["products"]["ETH-USD"]["trade_snapshots"], 1);
    EXPECT_EQ(r.json["totals"]["upstream_trade_gaps"], 0);
}

TEST(TradeWindow, BridgeTwoIntervalsAndUint64Max) {
    TradeIdWindow ids;
    EXPECT_TRUE(ids.observe(10)); EXPECT_TRUE(ids.observe(12));
    ASSERT_EQ(ids.retainedIntervals(), 2);
    EXPECT_TRUE(ids.observe(11)); EXPECT_EQ(ids.retainedIntervals(), 1);
    EXPECT_FALSE(ids.observe(10)); EXPECT_FALSE(ids.observe(11)); EXPECT_FALSE(ids.observe(12));
    EXPECT_TRUE(ids.observe(UINT64_MAX - 2)); EXPECT_TRUE(ids.observe(UINT64_MAX));
    EXPECT_EQ(ids.retainedIntervals(), 2);
    EXPECT_TRUE(ids.observe(UINT64_MAX - 1)); EXPECT_EQ(ids.retainedIntervals(), 1);
    EXPECT_FALSE(ids.observe(UINT64_MAX)); EXPECT_EQ(ids.high(), UINT64_MAX);
}
TEST(TradeWindow, ManyHolesOverLongSpanEvictWithoutFailure) {
    TradeIdWindow ids;
    for (uint64_t i = 0; i < 150000; ++i) ASSERT_TRUE(ids.observe(i * 200));
    EXPECT_GT(ids.evictions(), 90000);
    EXPECT_LE(ids.retainedIntervals(), 50001);
    EXPECT_LE(ids.peakIntervals(), 50002);
    EXPECT_TRUE(ids.observe(0)); EXPECT_EQ(ids.outOfWindow(), 1);
    EXPECT_LE(ids.retainedIntervals(), 50001);
}
TEST_F(CaptureTest, TradeSnapshotJumpAndCrossFrameReorderingFillCandidatesLate) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.trades({tradeEvent("snapshot", "BTC-USD", {105, 104})});
    f.trades({tradeEvent("update", "BTC-USD", {103, 101})});
    f.trades({tradeEvent("update", "BTC-USD", {102})});
    f.trades({tradeEvent("update", "BTC-USD", {108})});
    f.trades({tradeEvent("update", "BTC-USD", {107, 106})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_TRUE(r.json["trade_tape_complete"]); EXPECT_EQ(r.exitCode(true), 0);
    EXPECT_EQ(r.json["trade_gap_candidates"], 2); EXPECT_EQ(r.json["filled_later"], 5);
    EXPECT_EQ(r.json["still_missing"], 0); EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
    EXPECT_TRUE(r.json["missing_trade_ranges"].empty()); EXPECT_EQ(r.json["trades"], 9);
}
TEST_F(CaptureTest, TradePartialLateFillSplitsExactBackfillRangesAcrossReconnect) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100, 99})});
    f.trades({tradeEvent("update", "BTC-USD", {105})});
    f.up();
    f.trades({tradeEvent("snapshot", "BTC-USD", {105, 102, 100, 99})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["filled_later"], 1); EXPECT_EQ(r.json["still_missing"], 3);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 1); EXPECT_EQ(r.json["reconnect_trade_gaps"], 0);
    const auto& ranges = r.json["missing_trade_ranges"];
    ASSERT_EQ(ranges.size(), 2);
    EXPECT_EQ(ranges[0]["product"], "BTC-USD"); EXPECT_EQ(ranges[0]["first_id"], "101");
    EXPECT_EQ(ranges[0]["last_id"], "101"); EXPECT_EQ(ranges[1]["first_id"], "103");
    EXPECT_EQ(ranges[1]["last_id"], "104"); EXPECT_TRUE(ranges[0].contains("approx_time"));
}
TEST_F(CaptureTest, TradeReconnectOldHistoryScopeIsExplicit) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.up();
    // No coverage before the original 100 anchor; this sparse old history must
    // not claim to prove 91..98 were captured or manufacture a forward gap.
    f.trades({tradeEvent("snapshot", "BTC-USD", {100, 99, 90})});
    f.trades({tradeEvent("update", "BTC-USD", {101})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["reconnect_overlap_check"],
        "previously detected candidates only; no new holes inferred at or below previous connection high");
    EXPECT_EQ(r.json["still_missing"], 0); EXPECT_EQ(r.json["duplicate_trades"], 1);
}
TEST_F(CaptureTest, TradeVeryLateFillSurvivesDedupWindowEviction) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100, 102})});
    f.trades({tradeEvent("update", "BTC-USD", {TradeIdWindow::Window + 200})});
    f.trades({tradeEvent("update", "BTC-USD", {101})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_GT(r.json["dedup_window_evictions"].get<uint64_t>(), 0);
    EXPECT_EQ(r.json["filled_later"], 1);
    const auto& ranges = r.json["missing_trade_ranges"];
    ASSERT_EQ(ranges.size(), 1); EXPECT_EQ(ranges[0]["first_id"], "103");
    EXPECT_EQ(ranges[0]["last_id"], std::to_string(TradeIdWindow::Window + 199));
}
TEST_F(CaptureTest, TradeGapWithSameConnectionCaptureDamageRemainsIntegrityFailure) {
    for (const auto damage : {"sequence", "explicit", "tail"}) {
        QTemporaryDir temp; auto cfg = config; cfg.root = temp.path();
        TradeFixture f; f.up();
        f.trades({tradeEvent("update", "BTC-USD", {100})});
        f.trades({tradeEvent("update", "BTC-USD", {102})});
        // Damage arrives AFTER the candidate, exercising deferred classification.
        if (std::string(damage) == "sequence") {
            ++f.sequence; f.json({{"channel", "heartbeats"}});
        }
        f.append(Kind::CaptureStopped, std::string(damage) == "explicit" ? R"({"gap":true})" : "{}");
        {
            Writer writer(cfg, metadata());
            for (const auto& value : f.records) writer.append(value);
            writer.close();
        }
        if (std::string(damage) == "tail") {
            QFile file(paths(cfg.root).front()); ASSERT_TRUE(file.open(QIODevice::ReadWrite));
            ASSERT_TRUE(file.resize(file.size() - 1)); file.close();
        }
        const auto r = verify(cfg.root);
        EXPECT_FALSE(r.ok) << damage; EXPECT_FALSE(r.json["ok_closed_runs"]) << damage;
        EXPECT_EQ(r.json["integrity_trade_gaps"], 1) << damage << r.json.dump(2);
        EXPECT_EQ(r.json["integrity_missing_trades"], 1); EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
        EXPECT_EQ(r.json["missing_trade_ranges"][0]["category"], "integrity");
        EXPECT_GT(r.json["errors"].get<uint64_t>(), 0);
    }
}
TEST_F(CaptureTest, TradeOpenTailBecomesCaptureDamageWhenSuperseded) {
    TradeFixture f; f.up(); f.trades({tradeEvent("update", "BTC-USD", {100, 102})});
    {
        Writer old(config, metadata());
        for (const auto& value : f.records) old.append(value);
        old.flush(); // No stop/index: a legitimate live prefix until superseded.
    }
    const auto live = verify(config.root);
    ASSERT_TRUE(live.ok); EXPECT_EQ(live.json["upstream_trade_gaps"], 1);
    EXPECT_EQ(live.json["integrity_trade_gaps"], 0); EXPECT_EQ(live.exitCode(), 3);
    TradeFixture next; next.up(); next.trades({tradeEvent("update", "BTC-USD", {103})});
    const auto r = next.verifyAt(config);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.json["interrupted_runs"], 1);
    EXPECT_EQ(r.json["integrity_trade_gaps"], 1); EXPECT_EQ(r.json["upstream_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeDamageInAnotherConnectionDoesNotReclassifyUpstreamGap) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.trades({tradeEvent("update", "BTC-USD", {102})});
    f.up(); ++f.sequence; f.json({{"channel", "heartbeats"}});
    const auto r = f.verifyAt(config);
    EXPECT_FALSE(r.ok); EXPECT_EQ(r.json["upstream_trade_gaps"], 1);
    EXPECT_EQ(r.json["integrity_trade_gaps"], 0);
}
TEST_F(CaptureTest, TradeUpstreamDetailsAreBoundedAndSeparateFromIntegrityDetails) {
    TradeFixture f; f.up();
    for (uint64_t id = 100; id <= 180; id += 2) f.trades({tradeEvent("update", "BTC-USD", {id})});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok); EXPECT_TRUE(r.json["ok_closed_runs"]);
    EXPECT_EQ(r.json["upstream_trade_gaps"], 40); EXPECT_EQ(r.json["still_missing"], 40);
    EXPECT_EQ(r.json["upstream_trade_gap_details"].size(), 30);
    EXPECT_EQ(r.json["missing_trade_ranges"].size(), 40);
    EXPECT_TRUE(r.json["details"].empty()); EXPECT_EQ(r.json["errors"], 0);
}
TEST_F(CaptureTest, TradeFineDecimalSizesAndEighteenPlaceSumsAtMaxId) {
    TradeFixture f; f.up();
    auto event = tradeEvent("update", "BTC-USD", {UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 2});
    event["trades"][0]["size"] = "0.000000000000000001";
    event["trades"][1]["size"] = "0.123456789012345678";
    event["trades"][2]["size"] = "0.000000000000000009";
    for (auto& t : event["trades"]) t["side"] = "SELL";
    f.trades({event}); f.trades({event});
    const auto r = f.verifyAt(config);
    ASSERT_TRUE(r.ok) << r.json.dump(2);
    EXPECT_EQ(r.json["buy_volume"], "0.123456789012345688"); EXPECT_EQ(r.json["sell_volume"], "0");
    EXPECT_EQ(r.json["trades"], 3); EXPECT_EQ(r.json["duplicate_trades"], 3);
    EXPECT_TRUE(r.json["trade_tape_complete"]);
}
TEST_F(CaptureTest, TradePlainDecimalGrammarRejectsZeroSignExponentAndEmptyParts) {
    for (const auto* size : {"0", "-1", "+1", "1e-8", ".1", "1.", "1.2.3"}) {
        QTemporaryDir temp; auto cfg = config; cfg.root = temp.path();
        TradeFixture f; f.up(); auto event = tradeEvent("update", "BTC-USD", {100});
        event["trades"][0]["size"] = size; f.trades({event});
        EXPECT_FALSE(f.verifyAt(cfg).ok) << size;
    }
}
TEST_F(CaptureTest, TradeStrictCliChangesExitOnlyAndDefaultOpenStatusSurvives) {
    TradeFixture f; f.up();
    f.trades({tradeEvent("update", "BTC-USD", {100})});
    f.trades({tradeEvent("update", "BTC-USD", {102})});
    ASSERT_TRUE(f.verifyAt(config).ok);
    for (const bool strict : {false, true}) {
        QProcess child; QStringList args{"--verify", config.root};
        auto env = QProcessEnvironment::systemEnvironment(); env.insert("SENTINEL_LOG_DIR", config.root + "/logs");
        child.setProcessEnvironment(env);
        if (strict) args << "--strict-trades";
        child.start(CAPTURE_APP_FIXTURE, args);
        ASSERT_TRUE(child.waitForFinished(5000)); EXPECT_EQ(child.exitCode(), strict ? 2 : 0);
        const auto report = nlohmann::json::parse(child.readAllStandardOutput().toStdString());
        EXPECT_EQ(report["ok"], true); EXPECT_EQ(report["ok_closed_runs"], true);
        EXPECT_EQ(report["trade_tape_complete"], false);
    }
    auto r = verify(config.root); r.json["complete"] = false;
    EXPECT_EQ(r.exitCode(), 3); EXPECT_EQ(r.exitCode(true), 2);
}
TEST_F(CaptureTest, V1EnvelopeMissingChannelAndMalformedJsonKeepLegacyCounters) {
    for (const bool malformed : {false, true}) {
        QTemporaryDir temp; auto cfg = config; cfg.root = temp.path();
        TradeFixture f; f.up();
        if (malformed) f.append(Kind::Frame, R"({"channel":"heartbeats","sequence_num":1,)");
        else f.json({{"events", nlohmann::json::array()}});
        const auto r = f.verifyAt(cfg);
        EXPECT_EQ(r.json["unsequenced_frames"], 0);
        EXPECT_EQ(r.json["sequence_gaps"], 0);
        EXPECT_EQ(r.json["channels"][malformed ? "<invalid-envelope>" : "<unclassified>"]["frames"], 1);
        EXPECT_EQ(r.ok, !malformed);
        EXPECT_EQ(r.json["errors"], malformed ? 1 : 0);
    }
}

TEST_F(CaptureTest, ProductScopedRecoveryMarkersPreserveOtherReplayBooks) {
    for (const auto& product : {"ETH-USD", "BTC-USD", ""}) {
        for (const auto kind : {Kind::BookInvalidated, Kind::ResyncRequested}) {
            QTemporaryDir output;
            auto cfg = config;
            cfg.root = output.path();
            auto records = fixture();
            records.insert(records.end() - 2, record(kind, 1500000000,
                nlohmann::json{{"product", product}, {"reason", "level2 silent"}}.dump()));
            Writer writer(cfg, metadata());
            for (const auto& r : records) writer.append(r);
            writer.close();
            const auto report = verify(cfg.root);
            const bool foreign = std::string_view(product) == "ETH-USD";
            EXPECT_EQ(report.ok, foreign) << report.json.dump(2);
            EXPECT_EQ(report.json["replayed_l2_events"], foreign ? 2 : 1);
            EXPECT_EQ(report.json["unanchored_l2_events"], foreign ? 0 : 1);
            EXPECT_EQ(report.json["errors"], 0);
        }
    }
}


// ---- Shared capture queue pool (per-symbol connections, owner decision 7) ----
namespace {
size_t queuedCost(const Record& r) { return r.payload.capacity() + sizeof(Record) + 64; }
WriterConfig productConfig(WriterConfig config, const std::string& symbol) { config.symbol = symbol; return config; }
nlohmann::json productMetadata(const std::string& symbol) {
    auto meta = metadata(); meta["product_metadata"]["product_id"] = symbol; return meta;
}
// Holds a session's disk worker before its first drain, so queued bytes stay
// queued; released on scope exit even when an assertion returns early.
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> future = promise.get_future().share();
    bool open = false;
    std::function<void()> hook() { return [f = future] { f.wait(); }; }
    void release() { if (!open) { open = true; promise.set_value(); } }
    ~Gate() { release(); }
};
std::optional<double> residentBytes() {
    sentinel::metrics::MetricsRegistry registry;
    sentinel::metrics::registerProcessMetrics(registry);
    const auto text = registry.render();
    const std::string key = "\nprocess_resident_memory_bytes ";
    const auto at = text.find(key);
    if (at == std::string::npos) return std::nullopt;
    return std::stod(text.substr(at + key.size(), text.find('\n', at + key.size()) - at - key.size()));
}
} // namespace

TEST(CaptureQueuePool, FloorsGuaranteeEveryProductAdmissionUnderAFloodFromOne) {
    QueuePool pool(1000, 100, 3); // shared remainder: 700
    size_t flooded = 0;
    while (pool.reserve(0, 10)) flooded += 10;
    EXPECT_EQ(flooded, 800u); // its own floor plus the whole shared remainder
    EXPECT_EQ(pool.used(0), 800u);
    // The flood cannot take the other products' floors.
    EXPECT_TRUE(pool.reserve(1, 100)); EXPECT_FALSE(pool.reserve(1, 1));
    EXPECT_TRUE(pool.reserve(2, 60)); EXPECT_TRUE(pool.reserve(2, 40)); EXPECT_FALSE(pool.reserve(2, 1));
    EXPECT_EQ(pool.used(), 1000u);
    // Draining the flooding product returns shared capacity to whoever asks first.
    pool.release(0, 50);
    EXPECT_TRUE(pool.reserve(1, 50)); EXPECT_FALSE(pool.reserve(2, 1));
    EXPECT_EQ(pool.used(1), 150u); EXPECT_EQ(pool.used(), 1000u);
}
TEST(CaptureQueuePool, TotalCapHoldsAcrossProductsAndConfigurationIsChecked) {
    QueuePool pool(1000, 0, 2); // no floors: everything is shared
    EXPECT_TRUE(pool.reserve(0, 600)); EXPECT_TRUE(pool.reserve(1, 400));
    EXPECT_FALSE(pool.reserve(0, 1)); EXPECT_FALSE(pool.reserve(1, 1));
    EXPECT_FALSE(pool.reserve(0, SIZE_MAX)); // no wrap-around
    EXPECT_EQ(pool.used(), 1000u);
    pool.release(1, 400); pool.release(1, 1); // over-release clamps, never underflows
    EXPECT_EQ(pool.used(1), 0u); EXPECT_EQ(pool.used(), 600u);
    EXPECT_TRUE(pool.reserve(1, 400)); EXPECT_FALSE(pool.reserve(1, 1));
    EXPECT_THROW(QueuePool(1000, 501, 2), std::runtime_error); // floors exceed the pool
    EXPECT_NO_THROW(QueuePool(1000, 500, 2));
    EXPECT_THROW(QueuePool(1000, 0, 0), std::runtime_error);
    EXPECT_THROW(QueuePool(1000, 0, MaxProducts + 1), std::runtime_error);
}
TEST_F(CaptureTest, FloodedProductFailsAloneWhileItsPeerKeepsItsFloor) {
    auto pool = std::make_shared<QueuePool>(64 * 1024, 16 * 1024, 2);
    Gate gate;
    Session btc(productConfig(config, "BTC-USD"), productMetadata("BTC-USD"), pool, 0, {.beforeDrain = gate.hook()});
    Session eth(productConfig(config, "ETH-USD"), productMetadata("ETH-USD"), pool, 1, {.beforeDrain = gate.hook()});
    size_t btcAccepted = 0;
    for (int i = 0; i < 1000 && btc.submit(record(Kind::Frame, i, std::string(1024, 'b'))); ++i) ++btcAccepted;
    EXPECT_GT(btcAccepted, 30u); // floor + shared remainder (48 KiB)
    EXPECT_NE(btc.error().find("pool limit"), std::string::npos) << btc.error();
    EXPECT_LE(pool->used(), pool->total());
    size_t ethAccepted = 0;
    for (int i = 0; i < 10; ++i) ethAccepted += eth.submit(record(Kind::Frame, i, std::string(1024, 'e')));
    EXPECT_EQ(ethAccepted, 10u); // ~11 KiB, inside ETH's 16 KiB floor
    EXPECT_TRUE(eth.error().empty()) << eth.error();
    gate.release();
    btc.close(); eth.close();
    EXPECT_EQ(eth.stats().frames, 10u);
    EXPECT_EQ(btc.stats().frames, btcAccepted);
    EXPECT_EQ(pool->used(), 0u);
}
TEST_F(CaptureTest, FailedSessionReturnsItsReservationAtFailureNotAtClose) {
    auto pool = std::make_shared<QueuePool>(64 * 1024, 0, 2);
    Gate gate;
    Session failing(productConfig(config, "BTC-USD"), productMetadata("BTC-USD"), pool, 0,
        {.beforeDrain = gate.hook(), .beforeWriterOperation = [](auto&, auto operation, auto*) {
            if (operation == "append") throw std::runtime_error("injected disk failure");
        }});
    Session healthy(productConfig(config, "ETH-USD"), productMetadata("ETH-USD"), pool, 1);
    const auto item = record(Kind::Frame, 1, std::string(8000, 'x'));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(failing.submit(item));
    EXPECT_EQ(pool->used(0), 3 * queuedCost(item));
    gate.release();
    QElapsedTimer waited; waited.start();
    while ((pool->used(0) || failing.error().empty()) && waited.elapsed() < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Released while the failed session is still open (the process only quits on
    // its next 100 ms supervisor tick): the healthy peer can use the whole pool.
    EXPECT_FALSE(failing.error().empty());
    EXPECT_EQ(pool->used(0), 0u);
    EXPECT_FALSE(failing.submit(item)); EXPECT_EQ(pool->used(0), 0u); // refused, nothing reserved
    const auto large = record(Kind::Frame, 2, std::string(60 * 1024 - sizeof(Record) - 64 - 64, 'y'));
    EXPECT_TRUE(healthy.submit(large)) << healthy.error();
    failing.close(); healthy.close();
    EXPECT_TRUE(healthy.error().empty()) << healthy.error();
    const auto report = verify(config.root + "/BTC-USD");
    EXPECT_EQ(report.json["explicit_capture_gaps"], 1); // the loss is still recorded
    EXPECT_EQ(pool->used(), 0u);
}
TEST_F(CaptureTest, PoolAllocatesOnlyAsFramesQueue) {
    // 512 MiB pool for seven products, as in production: an idle capture must
    // hold ~0 queued bytes and the pool must never be allocated up front.
    const auto before = residentBytes();
    if (!before) GTEST_SKIP() << "no RSS sampler on this platform";
    auto pool = std::make_shared<QueuePool>(512ULL * 1024 * 1024, 2ULL * 1024 * 1024, 7);
    std::vector<std::unique_ptr<Session>> sessions;
    for (const auto* symbol : {"BTC-USD", "ETH-USD", "SOL-USD", "FARTCOIN-USD", "PEPE-USD", "DOGE-USD", "AVAX-USD"})
        sessions.push_back(std::make_unique<Session>(productConfig(config, symbol), productMetadata(symbol), pool, sessions.size()));
    for (auto& session : sessions) ASSERT_TRUE(session->submit(record(Kind::Frame, 1, "frame")));
    QElapsedTimer waited; waited.start();
    while (pool->used() && waited.elapsed() < 3000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(pool->used(), 0u); // drained: steady state queues nothing
    const auto after = residentBytes();
    ASSERT_TRUE(after);
    EXPECT_LT(*after - *before, 64.0 * 1024 * 1024) << "before=" << *before << " after=" << *after;
    for (auto& session : sessions) session->close();
}
TEST_F(CaptureTest, WriterIsV1OnlyAndNewRunsVerifyBesideTheLegacyV2Archive) {
    // The 2026-09-30..10-02 archive: one v2 run (BTC + ETH on one connection).
    writeMulti(config, multiFixture());
    // Production can no longer write v2.
    auto v2Metadata = productMetadata("BTC-USD");
    v2Metadata["connection_products"] = {"BTC-USD", "ETH-USD"};
    v2Metadata["routing"] = RoutingId;
    EXPECT_THROW(Writer(config, v2Metadata), std::runtime_error);
    EXPECT_THROW(Session(config, v2Metadata), std::runtime_error);
    // After the switch: one v1 session per product, two hours later.
    for (const std::string symbol : {"BTC-USD", "ETH-USD"}) {
        Session session(productConfig(config, symbol), productMetadata(symbol));
        auto records = fixture();
        if (symbol == "ETH-USD") {
            records[2] = frame(fixtures::coinbaseSubscriptionAck({"ETH-USD"}), 0, 100);
            records[3] = frame(fixtures::coinbaseL2Snapshot("ETH-USD", {{10, 1}}, {{11, 1}}), 1, 200);
            auto trade = nlohmann::json::parse(records[5].payload);
            trade["events"][0]["trades"][0]["product_id"] = "ETH-USD";
            records[5] = frame(trade, 3, 1100000000);
            records[6] = frame(fixtures::coinbaseL2Update("ETH-USD", {{"bid", 10, 2}}), 4, 2000000000);
        }
        for (auto& r : records) { r.time.systemNs += 2 * Hour; r.time.steadyNs += 2 * Hour; ASSERT_TRUE(session.submit(r)); }
        session.close();
        ASSERT_TRUE(session.error().empty()) << session.error();
    }
    int v1 = 0, v2 = 0;
    for (const auto& path : paths(config.root)) {
        const auto header = readHeader(path);
        if (header.at("format_version") == 1) {
            ++v1;
            EXPECT_FALSE(header.contains("connection_products")) << path.toStdString();
            EXPECT_FALSE(header.contains("routing"));
            EXPECT_EQ(contents(path).left(8), QByteArray("RAWL2\r\n\1", 8));
            scan(path, [](const Record& r) { EXPECT_NE(r.kind, Kind::FrameReference); }); // no routing receipts
        } else ++v2;
    }
    EXPECT_EQ(v1, 2); EXPECT_EQ(v2, 2);
    const auto report = verify(config.root);
    EXPECT_TRUE(report.ok) << report.json.dump(2);
    EXPECT_EQ(report.json["products"]["BTC-USD"]["connections"], 2);
    EXPECT_EQ(report.json["products"]["ETH-USD"]["connections"], 2);
    EXPECT_EQ(report.json["routing_errors"], 0);
}
TEST_F(CaptureTest, CaptureMetricsExposePerProductFeedQueueAndStoredFrames) {
    auto pool = std::make_shared<QueuePool>(1024 * 1024, 64 * 1024, 2);
    Gate gate;
    Session btc(productConfig(config, "BTC-USD"), productMetadata("BTC-USD"), pool, 0, {.beforeDrain = gate.hook()});
    Session eth(productConfig(config, "ETH-USD"), productMetadata("ETH-USD"), pool, 1);
    int64_t now = 1'000'000'000'000;
    FeedMetrics btcFeed(now), ethFeed(now);
    sentinel::metrics::MetricsRegistry registry;
    registerCaptureMetrics(registry, pool, {{"BTC-USD", &btcFeed, &btc}, {"ETH-USD", &ethFeed, &eth}}, [&] { return now; });
    const auto has = [&](const std::string& line) {
        const auto text = registry.render();
        return text.find("\n" + line + "\n") != std::string::npos;
    };
    EXPECT_TRUE(has("sentinel_capture_queue_pool_bytes 1048576"));
    EXPECT_TRUE(has("sentinel_capture_queue_floor_bytes 65536"));
    EXPECT_TRUE(has("# TYPE sentinel_capture_stored_frames_total counter"));
    now += 30'000'000'000; // never connected: down since start
    EXPECT_TRUE(has("sentinel_capture_feed_up{product=\"BTC-USD\"} 0"));
    EXPECT_TRUE(has("sentinel_capture_feed_down_seconds{product=\"BTC-USD\"} 30"));
    btcFeed.transport(true, 3, now); ethFeed.transport(true, 1, now);
    ethFeed.transport(false, 1, now + 5'000'000'000);
    ethFeed.transport(false, 1, now + 9'000'000'000); // duplicate down keeps the first time
    now += 155'000'000'000;
    EXPECT_TRUE(has("sentinel_capture_feed_up{product=\"BTC-USD\"} 1"));
    EXPECT_TRUE(has("sentinel_capture_feed_down_seconds{product=\"BTC-USD\"} 0"));
    EXPECT_TRUE(has("sentinel_capture_connection{product=\"BTC-USD\"} 3"));
    EXPECT_TRUE(has("sentinel_capture_feed_up{product=\"ETH-USD\"} 0"));
    EXPECT_TRUE(has("sentinel_capture_feed_down_seconds{product=\"ETH-USD\"} 150"));
    const auto item = record(Kind::Frame, 1, std::string(1000, 'q'));
    ASSERT_TRUE(btc.submit(item)); ASSERT_TRUE(btc.submit(item));
    const auto queued = std::to_string(2 * queuedCost(item));
    EXPECT_TRUE(has("sentinel_capture_queue_bytes{product=\"BTC-USD\"} " + queued));
    EXPECT_TRUE(has("sentinel_capture_queue_used_bytes " + queued));
    EXPECT_TRUE(has("sentinel_capture_stored_frames_total{product=\"BTC-USD\"} 0"));
    gate.release(); btc.close(); eth.close();
    EXPECT_TRUE(has("sentinel_capture_queue_bytes{product=\"BTC-USD\"} 0"));
    EXPECT_TRUE(has("sentinel_capture_stored_frames_total{product=\"BTC-USD\"} 2"));
    EXPECT_FALSE(has("sentinel_capture_file_bytes_total{product=\"BTC-USD\"} 0"));
}
