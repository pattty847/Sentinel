#include "../../apps/sentinel-server/SentinelServerApp.hpp"
#include "ConfigLoader.hpp"
#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "roller/ShadowRoller.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QTemporaryDir>
#include <fstream>
#include <future>
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
// Assemble the production process ownership graph without sockets, credentials,
// upstream feeds, metrics HTTP listener, or a bare server executable.
struct ShadowServerTestAccess {
  static void prepare(SentinelServerApp &app, const fs::path &temp) {
    app.m_serverModel = std::make_unique<ServerDataModel>(app.m_serverConfig);
    app.m_serverModel->registerMetrics(app.m_metrics);
    app.m_authenticator =
        std::make_unique<Authenticator>((temp / "no-credentials").string());
    app.m_server = std::make_unique<SentinelStreamServer>(
        *app.m_serverModel, *app.m_authenticator, app.m_serverConfig, 0);
    app.m_serverModel->m_recorderTimer
        .stop(); // explicit deterministic ticks below
  }
  static void start(SentinelServerApp &app) { app.startShadow({"BTC-USD"}); }
  static ServerDataModel &model(SentinelServerApp &app) {
    return *app.m_serverModel;
  }
  static void tick(SentinelServerApp &app, int64_t ms) {
    app.m_serverModel->m_recorder->onTick(ms);
  }
  static std::string metrics(SentinelServerApp &app) {
    return app.m_metrics.render();
  }
};
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
           count() == 127;
  }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(count(), 127);
  shadow.reset();
  writer->close();
  parity(120000);
}
TEST_F(ShadowTest, StalledAndFailingShadowDoesNotDelayServerPrimary) {
  initial(125);
  // TickBinaryLogger has a relative default; contain it in this temporary cwd.
  struct Cwd {
    QString old = QDir::currentPath();
    ~Cwd() { QDir::setCurrent(old); }
  } cwd;
  ASSERT_TRUE(QDir::setCurrent(temp.path()));
  std::atomic<bool> entered{false}, stalled{false};
  cfg.observeForTest = [&](const auto &, bool applied) {
    if (applied && !entered.exchange(true)) {
      stalled = true;
      std::this_thread::sleep_for(1500ms);
      stalled = false;
    }
  };
  ServerConfig config;
  config.defaultSymbols = {Product};
  config.recording.enabled = true;
  config.recording.dir = (root / "primary").string();
  config.recording.fallbackDir.clear();
  config.heatmap.persistenceEnabled = false;
  config.rollerShadow = cfg;
  SentinelServerApp app(config);
  ShadowServerTestAccess::prepare(app, root);
  auto &model = ShadowServerTestAccess::model(app);
  ASSERT_TRUE(model.recordingAvailable());
  std::atomic<int> deliveries{0};
  auto subscription = model.recordingLive()->subscribe(
      {Product, "near", 60000, {99990, 1, 20}, 1},
      [&](const auto &, const auto &) {
        ++deliveries;
        return true;
      });
  ASSERT_TRUE(subscription);
  const auto start = std::chrono::steady_clock::now();
  ShadowServerTestAccess::start(app);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  ASSERT_TRUE(eventually([&] { return entered.load(); }));
  ASSERT_TRUE(stalled.load());
  const auto now = QDateTime::currentMSecsSinceEpoch();
  model.onLiveOrderBookInitialized("BTC-USD", {{99999, 2}}, {{100001, 3}}, now);
  const auto boundary = now / 60000 * 60000 + 120000;
  const auto append = [&](int64_t through) {
    const auto began = std::chrono::steady_clock::now();
    ShadowServerTestAccess::tick(app, through + 3000);
    EXPECT_TRUE(eventually([&] {
      return model.recordingWatermarks(Product, "near").minuteThroughMs >=
             through;
    }));
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
    EXPECT_FALSE(recording::Hmc2Store::readRange(root / "primary", Product,
                                                 "near", 60000, through - 60000,
                                                 through)
                     .empty());
  };
  append(boundary);
  EXPECT_TRUE(stalled.load());
  EXPECT_TRUE(eventually([&] { return deliveries.load() > 0; }));
  // A checkpoint write fault in the same running shadow and ownership graph.
  // The first snapshot already created provenance before the instrumentation.
  const auto checkpoint = root / "shadow" / Product / "roller.json";
  ASSERT_TRUE(fs::remove(checkpoint));
  fs::create_directory(checkpoint); // QSaveFile cannot rename over a directory
  {
    std::ofstream blocker(checkpoint / "blocker");
    blocker << "blocked";
  }
  ASSERT_TRUE(eventually([&] {
    return ShadowServerTestAccess::metrics(app).find(
               "sentinel_roller_shadow_setup_failures_total{product=\"BTC-"
               "USD\"} 0\n") == std::string::npos;
  }));
  append(boundary + 60000);
}
TEST_F(ShadowTest, MalformedRecordInvalidatesAndContinuesLikeBatch) {
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  writer->append(record(64000, "not JSON"));
  writer->append(record(
      65000,
      R"({"channel":"l2_data","events":[{"type":"snapshot","product_id":"BTC-USD","updates":"bad"}]})"));
  writer->append(record(66000, snapshot()));
  for (int t = 67; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0"));
  shadow.reset();
  writer->close();
  parity(180000);
  EXPECT_FALSE(fs::exists(root / "primary"));
}
TEST_F(ShadowTest, StopWakesCheckerAcrossPredicateWaitTransition) {
  std::atomic<bool> entered{false}, stopping{false};
  cfg.compareInterval = 2s; // a lost wake exceeds the one-second stop budget
  cfg.beforeCompareWaitForTest = [&] {
    entered = true;
    while (!stopping.load())
      std::this_thread::yield();
    // stop is now contending for the wait mutex. The broken version sets the
    // predicate and notifies here, before wait_for has actually gone to sleep.
    std::this_thread::sleep_for(100ms);
  };
  shadow = std::make_unique<ShadowRoller>(cfg, std::vector<std::string>{},
                                          root / "primary", *metrics);
  ASSERT_TRUE(eventually([&] { return entered.load(); }));
  const auto began = std::chrono::steady_clock::now();
  auto done = std::async(std::launch::async, [&] {
    stopping = true;
    shadow->stop();
  });
  EXPECT_EQ(done.wait_for(1s), std::future_status::ready);
  done.get();
  EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
}
TEST_F(ShadowTest, PersistentWriteFaultEntersCooldownAndRecovers) {
  initial(125);
  cfg.failureMinDuration = 40ms;
  cfg.failureCooldown = 900ms;
  fs::create_directories(root / "shadow");
  {
    std::ofstream out(root / "shadow" / Product);
    out << "blocked";
  }
  start();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 1");
  }));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_fault_cooldowns_total{product=\"BTC-USD\"} 1"));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 3"));
  std::this_thread::sleep_for(250ms);
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 3"));
  EXPECT_EQ(count(), 0);
  fs::remove(root / "shadow" / Product);
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 0");
  }));
}
TEST_F(ShadowTest, JournalUnavailableResumesAppliedCursorWithoutReplay) {
  initial();
  cfg.failureMinDuration = 40ms;
  cfg.failureCooldown = 900ms;
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64; }));
  // Keep capture's durable tip ahead while its files are unavailable.
  publish = false;
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  fs::rename(root / "raw", root / "unmounted");
  fanout->publish(0, {capture::JournalEventKind::Durable,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 1");
  }));
  EXPECT_EQ(count(), 64); // no day-anchor reapplication on each transport retry
  std::this_thread::sleep_for(250ms);
  EXPECT_EQ(count(), 64);
  fs::rename(root / "unmounted", root / "raw");
  publish = true;
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
  EXPECT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 0");
  }));
  shadow.reset();
  writer->close();
  parity(120000);
}
TEST_F(ShadowTest, ThirtySecondOutageNeverEntersCooldown) {
  initial(125);
  cfg.failureCooldown = 900ms;
  std::atomic<int64_t> elapsedMs{0};
  const auto base = std::chrono::steady_clock::now();
  cfg.nowForTest = [&] { return base + std::chrono::milliseconds(elapsedMs.load()); };
  fs::create_directories(root / "shadow");
  { std::ofstream out(root / "shadow" / Product); out << "blocked"; }
  start();
  ASSERT_TRUE(eventually([&] { return has("sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 3"); }));
  elapsedMs = 30000;
  ASSERT_TRUE(eventually([&] { return has("sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 4"); }));
  EXPECT_TRUE(has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 0"));
  EXPECT_TRUE(has("sentinel_roller_shadow_fault_cooldowns_total{product=\"BTC-USD\"} 0"));
  fs::remove(root / "shadow" / Product);
  ASSERT_TRUE(eventually([&] { return count() == 126; }));
}
TEST_F(ShadowTest, PersistentFaultWaitsForMinimumElapsedTime) {
  initial(125);
  std::atomic<int64_t> elapsedMs{0};
  const auto base = std::chrono::steady_clock::now();
  cfg.nowForTest = [&] { return base + std::chrono::milliseconds(elapsedMs.load()); };
  cfg.failureCooldown = 900ms;
  fs::create_directories(root / "shadow");
  { std::ofstream out(root / "shadow" / Product); out << "blocked"; }
  start();
  ASSERT_TRUE(eventually([&] { return has("sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 3"); }));
  EXPECT_TRUE(has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 0"));
  elapsedMs = 120000;
  ASSERT_TRUE(eventually([&] { return has("sentinel_roller_shadow_fault_cooldown{product=\"BTC-USD\"} 1"); }));
  EXPECT_TRUE(has("sentinel_roller_shadow_fault_cooldowns_total{product=\"BTC-USD\"} 1"));
}
TEST_F(ShadowTest, ComparisonWatermarkAndMismatchTotalsSurviveRestart) {
  initial(3661);
  start();
  ASSERT_TRUE(eventually([&] { return count() == 3662; }));
  shadow.reset();
  auto rows = recording::Hmc2Store::readRange(root / "shadow", Product, "near",
                                              60000, Epoch, Epoch + 3600000);
  ASSERT_FALSE(rows.empty());
  ASSERT_FALSE(rows.front().entries.empty());
  ++rows.front().entries.front().twapCode;
  {
    recording::Hmc2Store divergent(root / "shadow");
    divergent.append(rows.front());
  }
  cfg.compareInterval = 50ms;
  start();
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_mismatch_total{product=\"BTC-USD\","
               "layer=\"near\"} 1");
  }));
  shadow.reset();
  const auto checkpointPath = root / "shadow" / Product / "comparison.json";
  const auto saved = load(checkpointPath);
  EXPECT_EQ(saved.at("comparedThroughMs"), Epoch + 3600000);
  auto waits = std::make_shared<std::atomic<unsigned>>(0);
  cfg.beforeCompareWaitForTest = [waits] { ++*waits; };
  const auto previous = count();
  start();
  ASSERT_TRUE(eventually([&] { return count() == previous + 3662; }));
  const auto pass = waits->load();
  // Observe completed checker passes after warmup, not a short sleep that
  // could cancel a wrongly restarted oracle before it publishes its result.
  ASSERT_TRUE(eventually([&] { return waits->load() >= pass + 4; }));
  const auto hidden = checkpointPath.string() + ".unavailable";
  fs::rename(checkpointPath, hidden);
  const auto beforeMissing = waits->load();
  ASSERT_TRUE(eventually([&] { return waits->load() >= beforeMissing + 4; }));
  EXPECT_FALSE(
      fs::exists(checkpointPath)); // no fresh audit/rewrite of old hours
  EXPECT_TRUE(has("sentinel_roller_shadow_mismatch_total{product=\"BTC-USD\","
                  "layer=\"near\"} 1"));
  EXPECT_FALSE(has("sentinel_roller_shadow_comparison_failures_total{product="
                   "\"BTC-USD\"} 0"));
  fs::rename(hidden, checkpointPath);
  const auto afterRestore = waits->load();
  ASSERT_TRUE(eventually([&] { return waits->load() >= afterRestore + 4; }));
  shadow.reset();
  EXPECT_EQ(load(checkpointPath),
            saved); // not even the completion time rewrites
  EXPECT_TRUE(has("sentinel_roller_shadow_mismatch_total{product=\"BTC-USD\","
                  "layer=\"near\"} 1"));
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
  EXPECT_EQ(ServerConfig{}.rollerShadow.failureMinDuration, 120000ms);
  QTemporaryDir tmp;
  const auto p = fs::path(tmp.path().toStdString()) / "config.yaml";
  {
    std::ofstream out(p);
    out << "roller_shadow:\n  enabled: true\n  journal_dir: /tmp/raw\n  dir: "
           "/tmp/shadow\n  socket: /tmp/feed.sock\n  from: "
           "'2026-10-01T00:00:00Z'\n  fault_min_duration_ms: 45000\n";
  }
  ServerConfig c;
  ASSERT_TRUE(ConfigLoader::loadServerConfig(p.string(), &c));
  EXPECT_TRUE(c.rollerShadow.enabled);
  EXPECT_EQ(c.rollerShadow.outputRoot, "/tmp/shadow");
  EXPECT_EQ(c.rollerShadow.journalRoot, "/tmp/raw");
  EXPECT_EQ(c.rollerShadow.socketPath, "/tmp/feed.sock");
  EXPECT_EQ(c.rollerShadow.failureMinDuration, 45000ms);
}
} // namespace
