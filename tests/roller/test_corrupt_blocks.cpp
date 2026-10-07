#include "roller/Roller.hpp"
#include "roller/ShadowRoller.hpp"
#include "capture/CaptureFanout.hpp"
#include <QCoreApplication>
#include <thread>
#include "capture/CaptureVerifier.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "metrics/MetricsHttpServer.hpp"
#include <QTcpSocket>
#include "servermodel/HmcolFormat.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <gtest/gtest.h>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <zstd.h>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Minute = 60'000, Day = 86'400'000;
constexpr int64_t Epoch = 1'798'761'600'000;
QString qpath(const fs::path& p) { return QString::fromStdString(p.string()); }
std::string bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void save(const fs::path& p, const std::string& b) {
    std::ofstream out(p, std::ios::binary);
    out.write(b.data(), b.size());
    if (!out) throw std::runtime_error("failed to write scratch fixture: " + p.string());
}
void put32(std::string& b, size_t at, uint32_t n) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>(n >> (8 * i));
}
void corrupt(const fs::path& p, const capture::BlockIndex& block, bool zstd) {
    auto b = bytes(p);
    if (zstd) b[block.offset + 48] ^= 1; // destroy zstd magic, keep RAWL2 header valid
    else {
        b[block.offset + 40] ^= 1; // wrong raw CRC, correctly re-checksummed header
        put32(b, block.offset + 44, hmcol::crc32(
            reinterpret_cast<const uint8_t*>(b.data() + block.offset), 44));
    }
    save(p, b);
}
std::string frame(bool snap) {
    const auto levels = json::array({
        {{"side", "bid"}, {"price_level", "99999"}, {"new_quantity", "2"}},
        {{"side", "offer"}, {"price_level", "100001"}, {"new_quantity", "3"}}});
    const auto event = json{{"type", snap ? "snapshot" : "update"},
                            {"product_id", "BTC-USD"}, {"updates", levels}};
    return json{{"channel", "l2_data"}, {"events", json::array({event})}}.dump();
}
fs::path fixture(const fs::path& root, bool twoDays = false, capture::JournalObserver observer = {}) {
    capture::WriterConfig c; c.root = qpath(root); c.fsyncBlocks = 0;
    c.onJournal = std::move(observer);
    capture::Writer w(c, {{"product_metadata", {{"product_id", "BTC-USD"},
        {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}});
    const auto append = [&](int64_t t, bool snap) {
        w.append({capture::Kind::Frame, {(Epoch+t)*1'000'000, (t+1)*1'000'000}, 1, frame(snap)});
        w.flush();
    };
    for (int t = 0; t <= 365; ++t) append(t*1000, t == 0 || t == 180);
    const auto first = fs::path(w.currentPath().toStdString());
    if (twoDays) {
        append(Day-1000, false);
        for (int t = 0; t <= 245; ++t) append(Day+t*1000, t == 0);
    }
    w.close(); return first;
}
double counter(const metrics::MetricsRegistry& registry) {
    const auto text = registry.render();
    const auto at = text.find("sentinel_roller_journal_corrupt_blocks_total{product=\"BTC-USD\"} ");
    if (at == std::string::npos) throw std::runtime_error("missing corruption metric");
    return std::stod(text.substr(text.find(' ', at) + 1));
}
void equalRange(const fs::path& a, const fs::path& b, int64_t from, int64_t to) {
    for (const auto* layer : {"near", "deep"}) {
        auto d = diff(a, b, "BTC-USD", layer, from, to, Minute, true);
        EXPECT_EQ(d["mismatching"], 0) << d.dump();
        EXPECT_GT(d["matching"].get<int>(), 0);
    }
}
void shadowStyle(const RollOptions& options) {
    auto o = options;
    o.productWriterLease = true;
    // Same durable source seam as ShadowRoller, reopening inclusively at the
    // applied cursor and consuming only successors after a simulated reconnect.
    std::unique_ptr<JournalReader> reopened;
    JournalPos applied;
    unsigned n = 0;
    o.nextRecord = [&](JournalReader& disk, JournalRecord& r) {
        if (++n == 130) {
            reopened = std::make_unique<JournalReader>(o.journalRoot, o.product, applied);
            JournalRecord overlap;
            if (!reopened->next(overlap) || overlap.pos != applied) throw std::runtime_error("lost cursor");
        }
        const bool found = (reopened ? *reopened : disk).next(r);
        if (found) applied = r.pos;
        return found;
    };
    roll(o);
}
}

class CorruptBlock : public testing::TestWithParam<bool> {};
TEST_P(CorruptBlock, GapRecoveryMetricAuditAndShadowParity) {
    QTemporaryDir temp; ASSERT_TRUE(temp.isValid()); const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw");
    const auto scan = capture::scan(qpath(p)); ASSERT_GT(scan.index.size(), 181);
    RollOptions o{root/"raw", root/"clean", "BTC-USD", Epoch, Epoch+360'000};
    roll(o);
    metrics::MetricsRegistry registry; registerJournalMetrics(registry, "BTC-USD");
    const auto before = counter(registry);
    corrupt(p, scan.index[75], GetParam()); const auto damaged = bytes(p);
    o.outputRoot = root/"batch";
    const auto report = roll(o);
    EXPECT_EQ(report["days"][0]["committedThroughMs"], o.toMs);
    EXPECT_EQ(counter(registry), before+1);
    const auto rows = recording::Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", Minute, Epoch, o.toMs);
    ASSERT_EQ(rows.size(), 5); // minute 2 has no valid observation, so no column
    EXPECT_EQ(rows[1].bucketStartMs, Epoch+Minute);
    EXPECT_EQ(rows[1].observedMs, 14'000); // boundary is last good receive, at 74 s
    EXPECT_EQ(rows[2].bucketStartMs, Epoch+180'000);
    equalRange(root/"clean", o.outputRoot, Epoch+180'000, o.toMs);
    o.outputRoot = root/"shadow"; shadowStyle(o);
    equalRange(root/"batch", o.outputRoot, Epoch, o.toMs);
    EXPECT_EQ(counter(registry), before+1); // anchor/reconnect/oracle replay deduplicated
    EXPECT_THROW(capture::scan(qpath(p)), std::runtime_error);
    const auto audit = capture::verify(qpath(p)); EXPECT_FALSE(audit.ok);
    EXPECT_GT(audit.json["errors"].get<int>(), 0);
    EXPECT_NE(audit.json.dump().find(GetParam() ? "zstd" : "block CRC"), std::string::npos);
    EXPECT_EQ(bytes(p), damaged);
}
TEST_P(CorruptBlock, FirstDayCompletesAndNextDayRolls) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw", true); const auto scan = capture::scan(qpath(p));
    RollOptions o{root/"raw", root/"clean", "BTC-USD", Epoch, Epoch+Day+240'000};
    roll(o); corrupt(p, scan.index[75], GetParam()); o.outputRoot = root/"damaged";
    const auto report = roll(o); ASSERT_EQ(report["days"].size(), 2);
    EXPECT_EQ(report["days"][0]["committedThroughMs"], Epoch+Day);
    EXPECT_EQ(report["days"][1]["committedThroughMs"], o.toMs);
    equalRange(root/"clean", o.outputRoot, Epoch+Day, o.toMs);
}
TEST_P(CorruptBlock, GapSurvivesSegmentBoundaryAndLostCursor) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw", true); const auto scan = capture::scan(qpath(p));
    const auto last = scan.index.back(); corrupt(p, last, GetParam());
    const JournalPos lost{"BTC-USD", scan.header.at("run_id"), last.ordinal, 0};
    JournalReader reader(root/"raw", "BTC-USD", lost); JournalRecord r;
    ASSERT_TRUE(reader.next(r)); EXPECT_GT(r.pos.block, lost.block); EXPECT_TRUE(r.gapBefore);
    JournalReader all(root/"raw", "BTC-USD");
    bool crossed = false;
    while (all.next(r)) if (r.pos.block == last.ordinal+1) { crossed = true; EXPECT_TRUE(r.gapBefore); }
    EXPECT_TRUE(crossed);
}
// Explicit real-hour acceptance: all writes are to QTemporaryDir copies. The
// optional source is read-only, never modified or passed to a writer/verifier.
TEST_P(CorruptBlock, RealHourCopyRecoversAfterExchangeSnapshot) {
    const auto source = qEnvironmentVariable("SENTINEL_CORRUPTION_HOUR");
    if (source.isEmpty()) GTEST_SKIP() << "set SENTINEL_CORRUPTION_HOUR to recorded BTC 2026-10-05/17.rawl2";
    QTemporaryDir temp; ASSERT_TRUE(temp.isValid()); const fs::path root = temp.path().toStdString();
    fs::create_directories(root/"raw"); const auto p = root/"raw"/"hour.rawl2";
    ASSERT_TRUE(QFile::copy(source, qpath(p)));
    ASSERT_TRUE(QFile::setPermissions(qpath(p), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    std::vector<std::pair<uint64_t, int64_t>> snaps;
    capture::RecordReader reader(qpath(p)); capture::Record r;
    while (reader.next(r)) if (r.kind == capture::Kind::Frame && r.payload.find("\"snapshot\"") != std::string::npos) {
        const auto j = json::parse(r.payload);
        if (j.value("channel", "") == "l2_data") snaps.emplace_back(reader.result().recordOrdinal, r.time.systemNs/1'000'000);
    }
    ASSERT_GE(snaps.size(), 2);
    const auto& index = reader.result().index;
    auto damage = std::find_if(index.begin(), index.end(), [&](const auto& b) { return b.firstSystemNs/1'000'000 > snaps[0].second+120'000+(GetParam() ? 5'000 : 0); });
    ASSERT_NE(damage, index.end()); ASSERT_LT(damage->ordinal, snaps[1].first);
    const auto from = snaps[0].second/Minute*Minute, to = (snaps[1].second/Minute+3)*Minute;
    RollOptions o{root/"raw", root/"clean", "BTC-USD", from, to}; roll(o);
    metrics::MetricsRegistry registry; registerJournalMetrics(registry, "BTC-USD"); const auto before = counter(registry);
    corrupt(p, *damage, GetParam()); const auto damaged = bytes(p);
    o.outputRoot = root/"batch"; const auto report = roll(o);
    EXPECT_EQ(report["days"][0]["committedThroughMs"], to);
    EXPECT_EQ(counter(registry), before+1);
    const auto damagedMinute = damage->firstSystemNs/1'000'000/Minute*Minute;
    const auto partial = recording::Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", Minute, damagedMinute, damagedMinute+Minute);
    ASSERT_EQ(partial.size(), 1); EXPECT_GT(partial[0].observedMs, 0); EXPECT_LT(partial[0].observedMs, Minute);
    const auto gapMinute = (damage->lastSystemNs/1'000'000/Minute+1)*Minute;
    const auto recoveryMinute = snaps[1].second/Minute*Minute;
    ASSERT_LT(gapMinute, recoveryMinute);
    EXPECT_TRUE(recording::Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", Minute, gapMinute, recoveryMinute).empty());
    equalRange(root/"clean", o.outputRoot, recoveryMinute+Minute, to);
    o.outputRoot = root/"shadow"; shadowStyle(o); equalRange(root/"batch", o.outputRoot, from, to);
    EXPECT_THROW(capture::scan(qpath(p)), std::runtime_error);
    const auto audit = capture::verify(qpath(p)); EXPECT_FALSE(audit.ok);
    EXPECT_NE(audit.json.dump().find(GetParam() ? "zstd" : "block CRC"), std::string::npos);
    EXPECT_EQ(bytes(p), damaged);
}
INSTANTIATE_TEST_SUITE_P(Payload, CorruptBlock, testing::Bool());

TEST(CorruptBlockPolicy, DecompressionErrorSkipsOnlyValidatedPayload) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw"); const auto scan = capture::scan(qpath(p));
    const auto block = scan.index[75]; auto b = bytes(p);
    // Preserve the frame extent but flip compressed content until zstd rejects
    // decoding. This exercises decompression, separately from bad zstd magic.
    bool found = false;
    std::string raw(block.rawBytes, '\0');
    for (size_t at = block.offset+54; at < block.offset+48+block.compressedBytes; ++at) {
        b[at] ^= 0x40;
        const auto* data = b.data()+block.offset+48;
        if (ZSTD_findFrameCompressedSize(data, block.compressedBytes) == block.compressedBytes &&
            ZSTD_isError(ZSTD_decompress(raw.data(), raw.size(), data, block.compressedBytes))) {
            found = true; break;
        }
        b[at] ^= 0x40;
    }
    ASSERT_TRUE(found); save(p, b);
    unsigned corruptions = 0, records = 0;
    capture::RecordReader reader(qpath(p), false, {}, [&](const auto& entry, const char* reason) {
        ++corruptions; EXPECT_EQ(entry.offset, block.offset); EXPECT_STREQ(reason, "zstd decompression failed");
    });
    capture::Record r; while (reader.next(r)) ++records;
    EXPECT_EQ(records, 365); EXPECT_EQ(corruptions, 1);
    EXPECT_TRUE(reader.result().indexed); EXPECT_EQ(reader.result().index.size(), scan.index.size());
    EXPECT_THROW(capture::scan(qpath(p)), std::runtime_error);
    // The validated closing index also permits skipping a damaged header.
    b[block.offset+44] ^= 1; save(p, b);
    capture::RecordReader strictHeader(qpath(p), false, {}, [](const auto&, const char*) {});
    EXPECT_NO_THROW(while (strictHeader.next(r)) {});
    EXPECT_THROW(capture::scan(qpath(p)), std::runtime_error);
}
TEST(CorruptBlockPolicy, AlertResolvesAfterTwoHoursWithoutNewCorruption) {
    const auto root = YAML::LoadFile(std::string(SENTINEL_SOURCE_ROOT)+"/ops/monitoring/grafana/provisioning/alerting/rules.yaml");
    YAML::Node alert;
    for (size_t g = 0; g < root["groups"].size(); ++g) {
        const auto rules = root["groups"][g]["rules"];
        for (size_t i = 0; i < rules.size(); ++i)
            if (rules[i]["uid"].as<std::string>() == "sentinel-roller-journal-corruption") alert = rules[i];
    }
    ASSERT_TRUE(alert.IsMap()); EXPECT_EQ(alert["for"].as<std::string>(), "0s");
    EXPECT_EQ(alert["noDataState"].as<std::string>(), "OK");
    EXPECT_EQ(alert["labels"]["severity"].as<std::string>(), "page");
    EXPECT_EQ(alert["data"][0]["model"]["expr"].as<std::string>(),
        "sum by (product) (increase(sentinel_roller_journal_corrupt_blocks_total{job=\"sentinel-server\"}[2h]))");
    const auto evaluator = alert["data"][1]["model"]["conditions"][0]["evaluator"];
    EXPECT_EQ(evaluator["type"].as<std::string>(), "gt"); EXPECT_EQ(evaluator["params"][0].as<int>(), 0);
}

#ifndef _WIN32
TEST(CorruptBlockPolicy, ShadowWorkerCrossesDamagedDayWithoutRetry) {
    static int argc = 1;
    static char name[] = "test_corrupt_blocks";
    static char* argv[] = {name, nullptr};
    if (!QCoreApplication::instance()) { static QCoreApplication app(argc, argv); }
    QTemporaryDir temp("/tmp/corrupt-worker-XXXXXX"); ASSERT_TRUE(temp.isValid());
    const fs::path root = temp.path().toStdString();
    metrics::MetricsRegistry registry;
    capture::FanoutConfig fc; fc.socketPath = qpath(root/"capture.sock");
    capture::CaptureFanout fanout(fc, {"BTC-USD"}, registry, [](const auto&) {});
    const auto p = fixture(root/"raw", true, [&](const auto& e) { fanout.publish(0, e); });
    const auto scan = capture::scan(qpath(p)); corrupt(p, scan.index[75], false);
    registerJournalMetrics(registry, "BTC-USD"); const auto before = counter(registry);
    ShadowConfig c; c.enabled = true; c.from = "2027-01-01T00:00:00Z";
    c.journalRoot = (root/"raw").string(); c.outputRoot = (root/"shadow").string();
    c.socketPath = fc.socketPath.toStdString();
    c.retryMin = std::chrono::milliseconds(20); c.retryMax = std::chrono::milliseconds(100);
    ShadowRoller shadow(c, {"BTC-USD"}, root/"primary", registry);
    registerJournalMetrics(registry, "BTC-USD"); // production constructor also registers
    metrics::MetricsHttpServer http(registry); ASSERT_TRUE(http.listen(0));
    QTcpSocket client; client.connectToHost("127.0.0.1", http.port()); ASSERT_TRUE(client.waitForConnected(1000));
    client.write("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n"); client.flush();
    QByteArray response;
    const auto httpDeadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        QCoreApplication::processEvents(); response += client.readAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (client.state() != QAbstractSocket::UnconnectedState && std::chrono::steady_clock::now() < httpDeadline);
    response += client.readAll(); ASSERT_TRUE(response.startsWith("HTTP/1.1 200"));
    const auto exposition = response.toStdString();
    const std::string series = "sentinel_roller_journal_corrupt_blocks_total{product=\"BTC-USD\"} ";
    const auto first = exposition.find(series); ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(exposition.find(series, first+series.size()), std::string::npos);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (shadow.watermarks("BTC-USD", "near").minuteThroughMs < Epoch+Day+240'000 &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GE(shadow.watermarks("BTC-USD", "near").minuteThroughMs, Epoch+Day+240'000);
    shadow.stop();
    EXPECT_NE(registry.render().find("sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0\n"), std::string::npos);
    EXPECT_EQ(counter(registry), before+1);
    RollOptions o{root/"raw", root/"batch", "BTC-USD", Epoch, Epoch+Day+240'000}; roll(o);
    equalRange(o.outputRoot, root/"shadow", Epoch, o.toMs);
    const auto hours = diff(o.outputRoot, root/"shadow", "BTC-USD", "deep", Epoch, Epoch+Day, 3'600'000, true);
    EXPECT_EQ(hours["mismatching"], 0); EXPECT_EQ(hours["matching"], 24);
}
#endif

TEST(CorruptBlockPolicy, MetricRegistrationSurvivesRepeatedAndReusedRegistries) {
    std::optional<metrics::MetricsRegistry> registry(std::in_place);
    for (int lifetime = 0; lifetime < 2; ++lifetime) {
        for (const auto* product : {"BTC-USD", "ETH-USD"}) {
            registerJournalMetrics(*registry, product);
            EXPECT_NO_THROW(registerJournalMetrics(*registry, product));
            EXPECT_TRUE(registry->hasSeries("sentinel_roller_journal_corrupt_blocks_total", {{"product", product}}));
        }
        EXPECT_FALSE(registry->hasSeries("sentinel_roller_journal_corrupt_blocks_total", {{"product", "SOL-USD"}}));
        registry.emplace(); // same address, new series storage
    }
}

namespace {
constexpr int64_t FramingStart = Epoch+Day-240'000;
fs::path framingFixture(const fs::path& root, bool splitAfter75) {
    capture::WriterConfig c; c.root = qpath(root); c.fsyncBlocks = 0;
    capture::Writer w(c, {{"product_metadata", {{"product_id", "BTC-USD"},
        {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}});
    fs::path first;
    for (int t = 0; t <= 485; ++t) {
        w.append({capture::Kind::Frame, {(FramingStart+t*1000)*1'000'000, (t+1)*1'000'000LL},
                  1, frame(t == 0 || t == 180 || t == 300)});
        w.flush();
        if (t == 0) first = w.currentPath().toStdString();
        if (t == 75 && splitAfter75) w.sealSegment();
    }
    w.close(); return first;
}
void framingCase(const std::string& kind, bool unsealed = false) {
    QTemporaryDir temp; ASSERT_TRUE(temp.isValid()); const fs::path root = temp.path().toStdString();
    const auto p = framingFixture(root/"raw", kind == "index");
    const auto scan = capture::scan(qpath(p)); ASSERT_GT(scan.index.size(), 75);
    const auto original = bytes(p);
    const auto block = kind == "last-header" ? scan.index.back() : scan.index[75]; const auto& last = scan.index.back();
    const auto footer = last.offset + 48 + last.compressedBytes;
    if (unsealed) save(p, original.substr(0, footer));
    RollOptions o{root/"raw", root/"clean", "BTC-USD", FramingStart, FramingStart+480'000}; roll(o);
    auto b = original;
    if (kind == "header" || kind == "last-header") b[block.offset+44] ^= 1;
    else if (kind == "magic") b[block.offset] ^= 1;
    else b.back() ^= 1;
    if (unsealed) b.resize(footer);
    save(p, b);
    metrics::MetricsRegistry registry; registerJournalMetrics(registry, "BTC-USD"); const auto before = counter(registry);
    o.outputRoot = root/"damaged";
    const auto report = roll(o);
    ASSERT_EQ(report["days"].size(), 2);
    EXPECT_EQ(report["days"][0]["committedThroughMs"], Epoch+Day);
    EXPECT_EQ(report["days"][1]["committedThroughMs"], o.toMs);
    EXPECT_EQ(counter(registry), before+1);
    const auto gapFrom = kind == "last-header" ? 180'000 : Minute;
    const auto gapTo = kind == "last-header" ? 300'000 : 180'000;
    const auto rows = recording::Hmc2Store::readRange(o.outputRoot, "BTC-USD", "near", Minute,
                                                    FramingStart+gapFrom, FramingStart+gapTo);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].observedMs, kind == "last-header" ? 58'000 : kind == "index" ? 15'000 : 14'000);
    if (kind != "last-header") equalRange(root/"clean", o.outputRoot, FramingStart+180'000, Epoch+Day);
    equalRange(root/"clean", o.outputRoot, FramingStart+360'000, o.toMs);
    EXPECT_THROW(capture::scan(qpath(p)), std::runtime_error);
    const auto audit = capture::verify(qpath(p)); EXPECT_FALSE(audit.ok);
    EXPECT_EQ(bytes(p), b);
}
}
TEST(CorruptFraming, HeaderDamageCompletesDay) { framingCase("header"); framingCase("header", true); }
TEST(CorruptFraming, MagicDamageCompletesDay) { framingCase("magic"); framingCase("magic", true); }
TEST(CorruptFraming, IndexDamageCompletesDay) { framingCase("index"); }
TEST(CorruptFraming, LastHeaderDamageCompletesDay) { framingCase("last-header"); framingCase("last-header", true); }

TEST(CorruptFraming, DamagedIndexFormsAndMissingIndexAreUnsealed) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw"); const auto scan = capture::scan(qpath(p)); const auto original = bytes(p);
    const auto& last = scan.index.back(); const auto footer = last.offset+48+last.compressedBytes;
    for (const auto at : {footer, footer+4, footer+12, uint64_t(original.size()-1)}) {
        auto b = original; b[at] ^= 0x40;
        if (at == footer+12) put32(b, b.size()-4, hmcol::crc32(b.data()+footer+8, b.size()-footer-12));
        save(p, b);
        unsigned skips = 0, records = 0;
        capture::RecordReader reader(qpath(p), true, {}, [&](const auto& block, const char*) { ++skips; EXPECT_EQ(block.offset, footer); });
        capture::Record r; EXPECT_NO_THROW(while (reader.next(r)) ++records);
        EXPECT_EQ(records, 366); EXPECT_EQ(skips, 1); EXPECT_FALSE(reader.result().indexed);
        EXPECT_EQ(reader.result().validBytes, b.size());
        EXPECT_FALSE(capture::verify(qpath(p)).ok);
    }
    save(p, original.substr(0, footer));
    unsigned skips = 0, records = 0;
    capture::RecordReader reader(qpath(p), true, {}, [&](const auto&, const char*) { ++skips; }); capture::Record r;
    while (reader.next(r)) ++records;
    EXPECT_EQ(records, 366); EXPECT_EQ(skips, 0); EXPECT_FALSE(reader.result().indexed);
}
TEST(CorruptFraming, SuccessorRequiresCrcLimitsAndExactOrdinal) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw"); const auto scan = capture::scan(qpath(p)); const auto& target = scan.index[75];
    auto b = bytes(p); const auto& last = scan.index.back(); b.resize(last.offset+48+last.compressedBytes);
    b[target.offset] = 'X';
    // Embed a CRC-valid wrong-ordinal header before the real successor. Its
    // length also claims a legal, complete block; only exact ordinal rejects it.
    auto fake = b.substr(scan.index[74].offset, 48);
    b.replace(target.offset+48, fake.size(), fake); save(p, b);
    unsigned skips = 0; capture::RecordReader reader(qpath(p), false, {}, [&](const auto&, const char*) { ++skips; });
    capture::Record r; unsigned count = 0;
    while (reader.next(r)) {
        EXPECT_NE(reader.result().recordOrdinal, target.ordinal);
        if (count == 75) EXPECT_EQ(reader.result().recordOrdinal, target.ordinal+1);
        ++count;
    }
    EXPECT_EQ(count, 365); EXPECT_EQ(skips, 1);
    // CRC-bad plausible magic at the true successor is not accepted either.
    b[scan.index[76].offset+44] ^= 1; save(p, b);
    capture::RecordReader abandoned(qpath(p), true, {}, [](const auto&, const char*) {});
    count = 0; while (abandoned.next(r)) ++count;
    EXPECT_EQ(count, 75); EXPECT_TRUE(abandoned.result().pendingTail);
    EXPECT_EQ(abandoned.result().validBytes, b.size());
}
TEST(CorruptFraming, SkippedPayloadAdvancesValidBytes) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto p = fixture(root/"raw"); const auto scan = capture::scan(qpath(p)); const auto block = scan.index.back();
    corrupt(p, block, false); auto b = bytes(p); b.resize(block.offset+48+block.compressedBytes); save(p, b);
    capture::RecordReader reader(qpath(p), true, {}, [](const auto&, const char*) {}); capture::Record r;
    while (reader.next(r)) {}
    EXPECT_EQ(reader.result().validBytes, b.size());
}
