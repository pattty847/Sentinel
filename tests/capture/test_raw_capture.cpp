#include <gtest/gtest.h>
#include "capture/RawCapture.hpp"
#include "capture/CaptureVerifier.hpp"
#include "capture/CaptureSession.hpp"
#include "servermodel/HmcolFormat.hpp"
#include "marketdata/fixtures/coinbase_messages.hpp"
#include <QDirIterator>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <csignal>
#ifndef _WIN32
#include <sys/resource.h>
#endif

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
      "size":"0.00000001","side":"BUY","trade_id":"123"}]}]})");
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
TEST_F(CaptureTest, MissingStopIsIncompleteEvenWithIntactFooter) {
    auto records = fixture(); records.pop_back();
    Writer writer(config, metadata()); for (auto& r : records) writer.append(r); writer.close();
    const auto report = verify(config.root);
    EXPECT_FALSE(report.ok); EXPECT_EQ(report.json["incomplete_runs"], 1);
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
    for (int i = 0; i < 200; ++i) ASSERT_TRUE(session.submit(record(Kind::Frame, i, "old queued frame")));
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
