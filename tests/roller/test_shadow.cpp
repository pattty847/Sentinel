#include "ConfigLoader.hpp"
#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "roller/ShadowRoller.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
using namespace sentinel;
using namespace sentinel::roller;
using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
const int64_t Epoch = parseTime("2026-10-01T00:00:00Z");
const std::string Product = "BTC-USD";
bool eventually(auto f) {
  const auto until = std::chrono::steady_clock::now() + 8s;
  do {
    if (f())
      return true;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < until);
  return f();
}
json meta(const std::string &p = Product) {
  return {{"product_metadata",
           {{"product_id", p},
            {"quote_increment", "0.01"},
            {"base_increment", "0.00000001"}}}};
}
std::string snapshot(const std::string &product = Product) {
  return json({{"channel", "l2_data"},
               {"events",
                json::array(
                    {{{"type", "snapshot"},
                      {"product_id", product},
                      {"updates", json::array({{{"side", "bid"},
                                                {"price_level", "99999"},
                                                {"new_quantity", "2"}},
                                               {{"side", "offer"},
                                                {"price_level", "100001"},
                                                {"new_quantity", "3"}}})}}})}})
      .dump();
}
capture::Record record(int64_t ms,
                       std::string payload = "{\"channel\":\"heartbeats\"}") {
  return {capture::Kind::Frame,
          {Epoch * 1000000 + ms * 1000000, capture::Stamp::now().steadyNs},
          1,
          std::move(payload)};
}
std::string frame(const capture::Record &r) {
  std::string s;
  const auto put = [&](uint64_t v, int n) {
    for (int i = 0; i < n; ++i)
      s += char(v >> (8 * i));
  };
  put(28 + r.payload.size(), 4);
  put(uint32_t(r.kind), 4);
  put(r.time.systemNs, 8);
  put(r.time.steadyNs, 8);
  put(r.connection, 8);
  return s + r.payload;
}
json load(const fs::path &p) {
  json j;
  std::ifstream in(p);
  in >> j;
  return j;
}
std::map<std::string, std::string> contents(const fs::path &root) {
  std::map<std::string, std::string> result;
  if (fs::exists(root))
    for (const auto &e : fs::recursive_directory_iterator(root))
      if (e.path().extension() == ".hmc2") {
        std::ifstream in(e.path(), std::ios::binary);
        result[fs::relative(e.path(), root).string()] = {
            std::istreambuf_iterator<char>(in), {}};
      }
  return result;
}
struct ShadowTest : testing::Test {
  QTemporaryDir temp{"/tmp/sr-XXXXXX"};
  fs::path root = temp.path().toStdString();
  std::unique_ptr<metrics::MetricsRegistry> metrics =
      std::make_unique<metrics::MetricsRegistry>();
  std::unique_ptr<metrics::MetricsRegistry> fanoutMetrics;
  std::unique_ptr<capture::CaptureFanout> fanout;
  std::unique_ptr<capture::Writer> writer;
  std::unique_ptr<ShadowRoller> shadow;
  ShadowConfig cfg;
  std::mutex mutex;
  std::vector<JournalPos> applied;
  capture::JournalPosition durable;
  bool publish = true;
  void SetUp() override {
    static int argc = 1;
    static char name[] = "test_shadow";
    static char *argv[] = {name, nullptr};
    if (!QCoreApplication::instance()) {
      static QCoreApplication app(argc, argv);
    }
    cfg.enabled = true;
    cfg.journalRoot = (root / "raw").string();
    cfg.outputRoot = (root / "shadow").string();
    cfg.socketPath = (root / "capture.sock").string();
    cfg.from = "2026-10-01T00:00:00Z";
    cfg.retryMin = 20ms;
    cfg.retryMax = 100ms;
    cfg.observeForTest = [this](const auto &r, bool didApply) {
      if (didApply) {
        std::lock_guard lock(mutex);
        applied.push_back(r.pos);
      }
    };
    startFanout();
    startWriter();
  }
  void TearDown() override {
    shadow.reset();
    writer.reset();
    fanout.reset();
  }
  void startFanout(size_t ringBytes = 32 * 1024 * 1024) {
    capture::FanoutConfig c;
    c.socketPath = QString::fromStdString(cfg.socketPath);
    c.ringBytes = ringBytes;
    fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
    fanout = std::make_unique<capture::CaptureFanout>(
        c, std::vector{Product}, *fanoutMetrics, [](const auto &) {});
  }
  void startWriter() {
    capture::WriterConfig c;
    c.root = QString::fromStdString(cfg.journalRoot);
    c.fsyncBlocks = 1;
    c.onJournal = [this](const capture::JournalEvent &e) {
      if (e.kind == capture::JournalEventKind::Durable)
        durable = {Product, std::string(e.runId), e.block, e.record};
      if (fanout && publish)
        fanout->publish(0, e);
    };
    writer = std::make_unique<capture::Writer>(c, meta());
  }
  void initial(int seconds = 63) {
    writer->append(record(0, snapshot()));
    for (int t = 1; t <= seconds; ++t)
      writer->append(record(t * 1000));
    writer->flush();
  }
  void start() {
    metrics = std::make_unique<metrics::MetricsRegistry>();
    shadow = std::make_unique<ShadowRoller>(cfg, std::vector{Product},
                                            root / "primary", *metrics);
  }
  bool has(const std::string &line) {
    return metrics->render().find(line + "\n") != std::string::npos;
  }
  size_t count() {
    std::lock_guard lock(mutex);
    return applied.size();
  }
  void parity(int64_t end) {
    roll({cfg.journalRoot, root / "batch", Product, Epoch, Epoch + end});
    EXPECT_EQ(contents(cfg.outputRoot), contents(root / "batch"));
  }
};
TEST_F(ShadowTest, CatchupLiveResumeNoGapsOrDuplicates) {
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  {
    std::lock_guard lock(mutex);
    std::set<std::string> unique;
    for (const auto &p : applied)
      unique.insert(json(p).dump());
    EXPECT_EQ(unique.size(), applied.size());
  }
  shadow.reset();
  const auto checkpoint = load(root / "shadow" / Product / "roller.json");
  EXPECT_EQ(
      checkpoint.at("pos").get<JournalPos>(),
      (JournalPos{Product, durable.runId, durable.block, durable.record}));
  {
    std::lock_guard lock(mutex);
    applied.clear();
  }
  start();
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  for (int t = 126; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  shadow.reset();
  writer->close();
  parity(180000);
}
TEST_F(ShadowTest, NeverCrossesHandshakeDurableCeilingWhenAnchorIsNewer) {
  writer->append(record(-1000, snapshot()));
  writer->flush();
  publish = false;
  writer->append(record(0, snapshot()));
  writer->append(record(63000));
  writer->flush();
  start();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  EXPECT_EQ(count(), 0);
  EXPECT_TRUE(contents(cfg.outputRoot).empty());
  publish = true;
  writer->append(record(64000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() >= 3; }));
}
TEST_F(ShadowTest, CatchupJoinsSocketOverlapExactlyOnce) {
  initial();
  bool injected = false;
  cfg.observeForTest = [&](const JournalRecord &r, bool apply) {
    if (!apply)
      return;
    if (!injected) {
      injected = true;
      // Arrive after the first durable-tip handshake, during day replay.
      for (int t = 64; t <= 125; ++t)
        writer->append(record(t * 1000));
      writer->flush();
    }
    std::lock_guard lock(mutex);
    applied.push_back(r.pos);
  };
  start();
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  writer->append(record(126000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 127; }));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0"));
  shadow.reset();
  writer->close();
  std::set<std::string> unique;
  for (const auto &p : applied)
    unique.insert(json(p).dump());
  EXPECT_EQ(unique.size(), 127);
  parity(120000);
}
TEST_F(ShadowTest, RingMissCatchesUpToDurableTip) {
  fanout.reset();
  startFanout(256);
  initial(125);
  start();
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  shadow.reset();
  for (int t = 126; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  {
    std::lock_guard lock(mutex);
    applied.clear();
  }
  start();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  shadow.reset();
  writer->close();
  parity(180000);
}
TEST_F(ShadowTest, RetractNeverAppliesOrCheckpointsProvisionalSuffix) {
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  const auto withdrawn = frame(record(125000, snapshot()));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true, withdrawn});
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(count(), 64);
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] {
    return metrics->render().find("sentinel_roller_shadow_setup_failures_total{"
                                  "product=\"BTC-USD\"} 0\n") ==
           std::string::npos;
  }));
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1");
  }));
  std::this_thread::sleep_for(150ms);
  shadow.reset();
  writer->close();
  parity(120000);
  const auto cp = load(root / "shadow" / Product / "roller.json");
  EXPECT_EQ(
      cp["pos"].get<JournalPos>(),
      (JournalPos{Product, durable.runId, durable.block, durable.record}));
}
TEST_F(ShadowTest, NullRetractBeforeAnyDurabilityLeavesNoOutput) {
  const auto raw = frame(record(0, snapshot()));
  fanout->publish(
      0, {capture::JournalEventKind::Record, "withdrawn-run", 0, 0, true, raw});
  start();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(count(), 0);
  EXPECT_FALSE(fs::exists(root / "shadow" / Product / "roller.json"));
  fanout->publish(
      0,
      {capture::JournalEventKind::Retract, "withdrawn-run", 0, 0, false, {}});
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  initial();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  shadow.reset();
  writer->close();
  parity(60000);
}
TEST_F(ShadowTest, IndependentProductsShareRootWithTheirOwnGrids) {
  fanout.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(
      fc, std::vector<std::string>{Product, "ETH-USD"}, *fanoutMetrics,
      [](const auto &) {});
  initial();
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.symbol = "ETH-USD";
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(1, e); };
  capture::Writer eth(wc, meta("ETH-USD"));
  eth.append(record(0, snapshot("ETH-USD")));
  for (int t = 1; t <= 63; ++t)
    eth.append(record(t * 1000));
  eth.flush();
  shadow = std::make_unique<ShadowRoller>(
      cfg, std::vector<std::string>{Product, "ETH-USD"}, root / "primary",
      *metrics);
  ASSERT_TRUE(eventually([&] { return count() == 128; }));
  shadow.reset();
  writer->close();
  eth.close();
  roll({cfg.journalRoot, root / "batch", Product, Epoch, Epoch + 60000});
  roll({cfg.journalRoot, root / "batch", "ETH-USD", Epoch, Epoch + 60000});
  EXPECT_EQ(contents(cfg.outputRoot), contents(root / "batch"));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0"));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"ETH-USD\"} 0"));
}
TEST_F(ShadowTest, EofDiscardsProvisionalAndRecoversJournal) {
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  const auto withdrawn = frame(record(200000, snapshot()));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true, withdrawn});
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(count(), 64);
  fanout.reset();
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  startFanout(); // empty ring: first new durable watermark supplies the ceiling
  writer->append(record(126000));
  writer->flush();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1") &&
           count() > 64;
  }));
  std::this_thread::sleep_for(100ms);
  shadow.reset();
  writer->close();
  parity(120000);
}
TEST_F(ShadowTest, SocketAndWriteFailureDoNotBlockPrimaryRecorder) {
  initial();
  cfg.socketPath = (root / "absent.sock").string();
  start();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  auto c =
      deriveGrid(meta()["product_metadata"], 100000).config(root / "primary");
  recording::BookRecorder primary(c);
  primary.onSnapshotAt(Product, Epoch, Epoch,
                       {{true, 99999, 2}, {false, 100001, 3}});
  primary.onTick(Epoch + 63000);
  primary.drain();
  EXPECT_EQ(primary.stats().columnsWritten, 2);
  shadow.reset();
  cfg.socketPath = (root / "capture.sock").string();
  fs::create_directories(root / "shadow");
  {
    std::ofstream out(root / "shadow" / Product);
    out << "blocked";
  }
  start();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  primary.onTick(Epoch + 125000);
  primary.drain();
  EXPECT_EQ(primary.stats().columnsWritten, 4);
  EXPECT_EQ(primary.stats().diskErrors, 0);
  shadow.reset();
  fs::remove(root / "shadow" / Product);
  start();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1");
  }));
}
TEST_F(ShadowTest, MalformedRecordRetriesAndNeverAffectsPrimaryRoot) {
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  writer->append(record(64000, "not JSON"));
  writer->flush();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  shadow.reset();
  EXPECT_FALSE(fs::exists(root / "primary"));
}
TEST_F(ShadowTest, RefusesAliasedPrimaryRoot) {
  initial();
  fs::create_directories(root / "primary");
  fs::create_directory_symlink(root / "primary", root / "alias");
  cfg.outputRoot = (root / "alias").string();
  start();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  EXPECT_TRUE(fs::is_empty(root / "primary"));
}
TEST_F(ShadowTest, MismatchMetricIsStrictAndCrossConnectionInformational) {
  initial(125);
  writer->close();
  roll({cfg.journalRoot, root / "batch", Product, Epoch, Epoch + 120000});
  fs::copy(root / "batch", root / "shadow", fs::copy_options::recursive);
  fs::copy(root / "batch", root / "primary", fs::copy_options::recursive);
  auto &mismatch =
      metrics->counter("sentinel_roller_shadow_mismatch_total", "test",
                       {{"product", Product}, {"layer", "near"}});
  // A missing primary side never gates independent-connection acceptance.
  fs::remove_all(root / "primary" / Product / "near-60000");
  compareShadow(root / "shadow", root / "batch", root / "primary", Product,
                "near", Epoch, Epoch + 120000, mismatch);
  EXPECT_EQ(mismatch.value(), 0);
  // An actual decoded-content difference also increments the strict gate.
  auto rows = recording::Hmc2Store::readRange(root / "shadow", Product, "near",
                                              60000, Epoch, Epoch + 120000);
  ASSERT_EQ(rows.size(), 2);
  ASSERT_FALSE(rows.back().entries.empty());
  ++rows.back().entries.front().twapCode;
  {
    recording::Hmc2Store divergent(root / "shadow");
    divergent.append(rows.back());
  }
  compareShadow(root / "shadow", root / "batch", root / "primary", Product,
                "near", Epoch, Epoch + 120000, mismatch);
  EXPECT_EQ(mismatch.value(), 1);
  // Missing shadow buckets count, even though legacy diff excluded them.
  fs::remove_all(root / "shadow" / Product / "near-60000");
  compareShadow(root / "shadow", root / "batch", root / "primary", Product,
                "near", Epoch, Epoch + 120000, mismatch);
  EXPECT_EQ(mismatch.value(), 3);
}
TEST(ShadowStorage, ScopedWritersExcludeSameProductAndUnscopedOwners) {
  QTemporaryDir temp;
  fs::path root = temp.path().toStdString();
  recording::Hmc2Store btc(root, true, "BTC-USD");
  recording::Hmc2Store eth(root, true, "ETH-USD");
  EXPECT_THROW(recording::Hmc2Store(root, true, "BTC-USD"), std::runtime_error);
  EXPECT_THROW((void)recording::Hmc2Store(root), std::runtime_error);
  recording::Hmc2Record r;
  r.header.symbol = "ETH-USD";
  r.header.layer = "near";
  EXPECT_THROW(btc.append(r), std::runtime_error);
}
TEST_F(ShadowTest, HourlyWorkerChecksOnlyCompletedHours) {
  initial(3661);
  cfg.compareInterval = 50ms;
  start();
  ASSERT_TRUE(eventually([&] {
    return !has("sentinel_roller_shadow_last_comparison_timestamp_seconds{"
                "product=\"BTC-USD\"} 0");
  }));
  EXPECT_TRUE(has("sentinel_roller_shadow_mismatch_total{product=\"BTC-USD\","
                  "layer=\"near\"} 0"));
  EXPECT_TRUE(has("sentinel_roller_shadow_mismatch_total{product=\"BTC-USD\","
                  "layer=\"deep\"} 0"));
  EXPECT_TRUE(has("sentinel_roller_shadow_comparison_failures_total{product="
                  "\"BTC-USD\"} 0"));
}
TEST_F(ShadowTest, HandoffLatencyBenchmark) {
  // Timestamp at feed submission, before Session queue admission. Use the
  // production default 1 s block interval/fsync policy and real fan-out.
  writer.reset();
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(0, e); };
  capture::Session session(wc, meta());
  ASSERT_TRUE(session.submit(record(0, snapshot())));
  for (int t = 1; t <= 63; ++t)
    ASSERT_TRUE(session.submit(record(t * 1000)));
  std::mutex samplesMutex;
  std::vector<double> received, committed;
  cfg.observeForTest = [&](const auto &r, bool apply) {
    const auto ms = r.record.time.systemNs / 1000000;
    if (ms < Epoch + 64000 || ms >= Epoch + 64200)
      return;
    const auto us =
        (capture::Stamp::now().steadyNs - r.record.time.steadyNs) / 1000.;
    std::lock_guard lock(samplesMutex);
    (apply ? committed : received).push_back(us);
  };
  start();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1");
  }));
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(session.submit(record(64000 + i)));
    std::this_thread::sleep_until(begin + (i + 1) * 10ms);
  }
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(samplesMutex);
    return committed.size() == 200;
  }));
  shadow.reset();
  session.close();
  ASSERT_TRUE(session.error().empty());
  for (auto *v : {&received, &committed})
    std::sort(v->begin(), v->end());
  ASSERT_EQ(received.size(), 200);
  ASSERT_EQ(committed.size(), 200);
  std::cout << "SHADOW_HANDOFF samples=200 feed_to_receive_us p50="
            << received[99] << " p95=" << received[189]
            << " feed_to_durable_apply_us p50=" << committed[99]
            << " p95=" << committed[189] << '\n';
}
TEST(ShadowConfig, DefaultOffAndExplicitKeys) {
  EXPECT_FALSE(ServerConfig{}.rollerShadow.enabled);
  QTemporaryDir tmp;
  const auto p = fs::path(tmp.path().toStdString()) / "config.yaml";
  {
    std::ofstream out(p);
    out << "roller_shadow:\n  enabled: true\n  journal_dir: /tmp/raw\n  dir: "
           "/tmp/shadow\n  socket: /tmp/feed.sock\n  from: "
           "'2026-10-01T00:00:00Z'\n";
  }
  ServerConfig c;
  ASSERT_TRUE(ConfigLoader::loadServerConfig(p.string(), &c));
  EXPECT_TRUE(c.rollerShadow.enabled);
  EXPECT_EQ(c.rollerShadow.outputRoot, "/tmp/shadow");
  EXPECT_EQ(c.rollerShadow.journalRoot, "/tmp/raw");
  EXPECT_EQ(c.rollerShadow.socketPath, "/tmp/feed.sock");
}
} // namespace
