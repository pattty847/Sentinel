#include "../../apps/sentinel-server/SentinelServerApp.hpp"
#include "ConfigLoader.hpp"
#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "roller/ShadowRoller.hpp"
#include "servermodel/ChunkService.hpp"
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
  static void checkProgress(SentinelServerApp &app, int64_t nowMs) {
    app.m_serverModel->checkRecorderProgress(nowMs);
  }
  static bool hasPrimaryRecorder(SentinelServerApp &app) {
    return app.m_serverModel->m_recorder != nullptr;
  }
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
// One-level change; offer side keeps the book uncrossed above the 100001 ask.
std::string offer(const std::string &price, const std::string &size,
                  const std::string &product = Product) {
  return json({{"channel", "l2_data"},
               {"events",
                json::array(
                    {{{"type", "update"},
                      {"product_id", product},
                      {"updates", json::array({{{"side", "offer"},
                                                {"price_level", price},
                                                {"new_quantity", size}}})}}})}})
      .dump();
}
// PEPE-like: native prices far below $0.005 on the 1e-8 quote grid.
json pepeMeta(const std::string &p = "PEPE-USD") {
  return {{"product_metadata",
           {{"product_id", p},
            {"quote_increment", "0.00000001"},
            {"base_increment", "1"}}}};
}
std::string pepeSnapshot(const std::string &product = "PEPE-USD") {
  return json({{"channel", "l2_data"},
               {"events",
                json::array(
                    {{{"type", "snapshot"},
                      {"product_id", product},
                      {"updates",
                       json::array({{{"side", "bid"},
                                     {"price_level", "0.00000999"},
                                     {"new_quantity", "500000000"}},
                                    {{"side", "bid"},
                                     {"price_level", "0.00000980"},
                                     {"new_quantity", "70000000"}},
                                    {{"side", "offer"},
                                     {"price_level", "0.00001001"},
                                     {"new_quantity", "400000000"}}})}}})}})
      .dump();
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
  // Serving path: the sink stands in for LiveService::publish. The lead's
  // wall clock stays at liveNow (Epoch: ticks never move its integration clock,
  // so its finished minutes must equal history's committed ones exactly).
  std::mutex liveMutex;
  std::vector<std::shared_ptr<const recording::Hmc2Record>> live;
  std::atomic<int64_t> liveNow{Epoch};
  void serve() {
    cfg.liveNowForTest = [this] { return liveNow.load(); };
    cfg.publisher = [this](std::shared_ptr<const recording::Hmc2Record> r) {
      std::lock_guard lock(liveMutex);
      live.push_back(std::move(r));
    };
  }
  std::vector<std::shared_ptr<const recording::Hmc2Record>> published() {
    std::lock_guard lock(liveMutex);
    return live;
  }
  static bool provisional(const recording::Hmc2Record &r) {
    return r.flags & recording::kProvisional;
  }
  static bool hasRow(const recording::Hmc2Record &r, int64_t row, bool ask) {
    return std::any_of(r.entries.begin(), r.entries.end(), [&](const auto &e) {
      return e.row == row && e.isAsk == ask && e.peakCode > 0;
    });
  }
  // The withdrawn/provisional test level: ask 100500 x 7, near row 100500.
  bool livePublishedRow(int64_t row, size_t from = 0) {
    const auto all = published();
    for (size_t i = from; i < all.size(); ++i)
      if (provisional(*all[i]) && all[i]->header.layer == "near" &&
          hasRow(*all[i], row, true))
        return true;
    return false;
  }
  bool liveBucket(int64_t bucket, size_t from = 0) {
    const auto all = published();
    for (size_t i = from; i < all.size(); ++i)
      if (provisional(*all[i]) && all[i]->bucketStartMs == bucket)
        return true;
    return false;
  }
  double counter(const std::string &series) {
    const auto text = metrics->render();
    const auto at = text.find(series + " ");
    return at == std::string::npos
               ? -1
               : std::stod(text.substr(at + series.size() + 1));
  }
  double forks(const std::string &product = Product) {
    return counter("sentinel_roller_shadow_live_lead_forks_total{product=\"" +
                   product + "\"}");
  }
  // No duplicate or out-of-order live record: per series the forming minute
  // never moves back, and each committed minute is handed over once, in order.
  void expectOrderedLive() {
    std::map<std::pair<std::string, std::string>, int64_t> forming, finals;
    for (const auto &r : published()) {
      const auto key = std::pair(r->header.symbol, r->header.layer);
      if (provisional(*r)) {
        EXPECT_GE(r->bucketStartMs, forming[key])
            << key.first << " " << key.second;
        forming[key] = std::max(forming[key], r->bucketStartMs);
      } else {
        if (finals.contains(key))
          EXPECT_GT(r->bucketStartMs, finals[key])
              << key.first << " " << key.second;
        finals[key] = r->bucketStartMs;
        EXPECT_EQ(r->committedThroughMs, r->bucketStartMs + 60000);
      }
    }
  }
  static void expectSameColumn(const recording::Hmc2Record &a,
                               const recording::Hmc2Record &b) {
    EXPECT_EQ(a.bucketStartMs, b.bucketStartMs);
    EXPECT_EQ(a.observedMs, b.observedMs);
    EXPECT_EQ(a.flags & ~recording::kProvisional,
              b.flags & ~recording::kProvisional);
    EXPECT_EQ(std::tie(a.bidRowLo, a.bidRowHi, a.askRowLo, a.askRowHi),
              std::tie(b.bidRowLo, b.bidRowHi, b.askRowLo, b.askRowHi));
    EXPECT_EQ(std::tie(a.midOpen, a.midClose, a.midMin, a.midMax),
              std::tie(b.midOpen, b.midClose, b.midMin, b.midMax));
    ASSERT_EQ(a.entries.size(), b.entries.size()) << a.bucketStartMs;
    for (size_t i = 0; i < a.entries.size(); ++i)
      EXPECT_EQ(std::tie(a.entries[i].row, a.entries[i].isAsk,
                         a.entries[i].twapCode, a.entries[i].peakCode),
                std::tie(b.entries[i].row, b.entries[i].isAsk,
                         b.entries[i].twapCode, b.entries[i].peakCode));
  }
  // Every committed publication is the batch column of its bucket.
  size_t expectFinalsMatch(const fs::path &batch) {
    size_t n = 0;
    for (const auto &r : published()) {
      if (provisional(*r))
        continue;
      const auto rows = recording::Hmc2Store::readRange(
          batch, r->header.symbol, r->header.layer, 60000, r->bucketStartMs,
          r->bucketStartMs + 60000);
      if (rows.empty())
        continue; // committed after the batch range
      expectSameColumn(*r, rows.front());
      ++n;
    }
    return n;
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
  serve(); // reconnect with the live publisher attached
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
  expectOrderedLive();
  EXPECT_GT(expectFinalsMatch(root / "batch"), 0u);
}
TEST_F(ShadowTest, RingMissCatchesUpToDurableTip) {
  serve();
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
  ASSERT_TRUE(eventually([&] { return liveBucket(Epoch + 180000); }));
  shadow.reset();
  writer->close();
  parity(180000);
  expectOrderedLive();
  EXPECT_GT(expectFinalsMatch(root / "batch"), 0u);
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
  serve();
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
  ASSERT_TRUE(eventually([&] { return forks() == 2; }));
  shadow.reset();
  writer->close();
  parity(120000);
  expectOrderedLive();
  EXPECT_GT(expectFinalsMatch(root / "batch"), 0u);
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
  serve();
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
  ASSERT_TRUE(eventually([&] { return liveBucket(Epoch + 120000); }));
  shadow.reset();
  writer->close();
  parity(120000);
  expectOrderedLive();
  EXPECT_GT(expectFinalsMatch(root / "batch"), 0u);
}
TEST_F(ShadowTest, ThirtySecondOutageNeverEntersCooldown) {
  serve();
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
  // Live publication resumes at the durable tip, with no missing minute.
  ASSERT_TRUE(eventually([&] { return liveBucket(Epoch + 120000); }));
  shadow.reset();
  writer->close();
  parity(120000);
  expectOrderedLive();
  EXPECT_EQ(expectFinalsMatch(root / "batch"),
            2u * 2u); // minutes 0 and 1, both layers
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
// A10: option C. The forming minute reflects a fan-out record before (here:
// without) its durable marker; history never applies it.
TEST_F(ShadowTest, LeadPublishesProvisionalRecordBeforeItsDurableMarker) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  EXPECT_FALSE(livePublishedRow(100500));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] { return livePublishedRow(100500); }));
  EXPECT_EQ(count(), 64); // durable-only history
  shadow.reset();
  writer->close();
  parity(60000);
  expectOrderedLive();
  // The forming minute carries no committed claim; history's finals do.
  for (const auto &r : published())
    if (provisional(*r))
      EXPECT_EQ(r->committedThroughMs, 0);
}
// A quiet product's forming minute still advances: wall-clock ticks every
// 250 ms, like the primary recorder's timer (live age, owner gate).
TEST_F(ShadowTest, LeadWallTicksAdvanceQuietFormingMinute) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  liveNow = Epoch + 75000; // no record after 63 s
  ASSERT_TRUE(eventually([&] {
    for (const auto &r : published())
      if (provisional(*r) && r->bucketStartMs == Epoch + 60000 &&
          r->observedMs >= 15000)
        return true;
    return false;
  }));
  EXPECT_EQ(count(), 64);
  shadow.reset();
  writer->close();
  parity(60000);
}
// The lead is an exact fork: without wall ticks its finished minutes are the
// committed history columns, byte for byte in content.
TEST_F(ShadowTest, LeadForkFinishesMinutesExactlyLikeHistory) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  for (int t = 64; t <= 185; ++t)
    writer->append(record(t * 1000, t % 5 ? "{\"channel\":\"heartbeats\"}"
                                          : offer(std::to_string(100100 + t),
                                                  std::to_string(t % 7))));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  shadow.reset();
  writer->close();
  parity(180000);
  size_t compared = 0;
  const auto all = published();
  for (const auto &final : all) {
    if (provisional(*final) || final->bucketStartMs < Epoch + 60000)
      continue;
    // The lead's finished (held) copy: full observation, published before
    // commit.
    const recording::Hmc2Record *held = nullptr;
    for (const auto &r : all)
      if (provisional(*r) && r->header.layer == final->header.layer &&
          r->bucketStartMs == final->bucketStartMs && r->observedMs == 60000)
        held = r.get();
    ASSERT_NE(held, nullptr) << final->bucketStartMs;
    expectSameColumn(*held, *final);
    ++compared;
  }
  EXPECT_EQ(compared, 4u); // minutes 1 and 2, both layers
  expectOrderedLive();
}
// A11: retract. The withdrawn record reached the forming minute; recovery
// drops the lead, forks again from durable history and rebuilds that minute.
TEST_F(ShadowTest, RetractRebuildsLiveMinuteFromDurableState) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] { return livePublishedRow(100500); }));
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] { return forks() == 2; }));
  EXPECT_EQ(count(), 64);
  const auto mark = published().size();
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually(
      [&] { return count() == 126 && liveBucket(Epoch + 120000, mark); }));
  shadow.reset();
  writer->close();
  // The forming minute 1 republished from durable state, never the withdrawn
  // level.
  EXPECT_TRUE(liveBucket(Epoch + 60000, mark));
  EXPECT_FALSE(livePublishedRow(100500, mark));
  expectOrderedLive();
  parity(120000);
  EXPECT_EQ(expectFinalsMatch(root / "batch"), 2u * 2u);
}
// A11, withdrawn suffix crossing a minute: the rebuilt lead never shows an
// older forming minute than the discarded one did (no out-of-order record).
TEST_F(ShadowTest, RetractAcrossMinuteNeverMovesLiveBack) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 1, true, frame(record(121000))});
  ASSERT_TRUE(eventually([&] { return liveBucket(Epoch + 120000); }));
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] { return forks() == 2; }));
  const auto mark = published().size();
  for (int t = 64; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually(
      [&] { return count() == 186 && liveBucket(Epoch + 180000, mark); }));
  shadow.reset();
  writer->close();
  EXPECT_FALSE(livePublishedRow(100500, mark));
  expectOrderedLive();
  parity(180000);
  EXPECT_EQ(expectFinalsMatch(root / "batch"), 3u * 2u);
}
// A11: socket disconnect (capture restart) with a provisional suffix applied
// live.
TEST_F(ShadowTest, DisconnectRebuildsLiveFromDurableState) {
  serve();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 64 && forks() == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] { return livePublishedRow(100500); }));
  fanout.reset(); // EOF
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  startFanout();
  writer->append(record(126000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 127 && forks() == 2; }));
  const auto mark = published().size();
  for (int t = 127; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually(
      [&] { return count() == 186 && liveBucket(Epoch + 180000, mark); }));
  shadow.reset();
  writer->close();
  EXPECT_FALSE(livePublishedRow(100500, mark));
  expectOrderedLive();
  parity(180000);
  EXPECT_EQ(expectFinalsMatch(root / "batch"), 3u * 2u);
}
// Restart 20 s into a minute: the straddled minute commits once,
// batch-identical.
TEST_F(ShadowTest, RestartMidMinuteCommitsStraddledMinuteOnce) {
  serve();
  initial(80);
  start();
  ASSERT_TRUE(eventually([&] { return count() == 81 && forks() == 1; }));
  shadow.reset();
  {
    std::lock_guard lock(mutex);
    applied.clear();
  }
  start();
  ASSERT_TRUE(eventually([&] { return count() == 81 && forks() == 1; }));
  for (int t = 81; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  shadow.reset();
  writer->close();
  parity(180000);
  expectOrderedLive();
  EXPECT_EQ(expectFinalsMatch(root / "batch"),
            3u * 2u); // minute 1 once, after the restart
}
// A1 and owner decision 2: a listed product without a journal is refused.
TEST_F(ShadowTest, ListedProductWithoutJournalIsRefusedWithoutWorker) {
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
      cfg, std::vector<std::string>{Product, "ETH-USD", "ADA-USD"},
      root / "primary", *metrics);
  ASSERT_TRUE(eventually([&] {
    return has("sentinel_roller_shadow_running{product=\"BTC-USD\"} 1") &&
           has("sentinel_roller_shadow_running{product=\"ETH-USD\"} 1");
  }));
  std::this_thread::sleep_for(
      300ms); // a retrying worker would count many failures
  EXPECT_TRUE(has("sentinel_roller_shadow_running{product=\"ADA-USD\"} 0"));
  EXPECT_TRUE(has(
      "sentinel_roller_shadow_setup_failures_total{product=\"ADA-USD\"} 1"));
  EXPECT_TRUE(
      has("sentinel_roller_shadow_lag_seconds{product=\"ADA-USD\"} -1"));
  EXPECT_FALSE(fs::exists(root / "shadow" / "ADA-USD"));
  EXPECT_FALSE(shadow->running("ADA-USD"));
  EXPECT_TRUE(shadow->running("ETH-USD"));
  EXPECT_EQ(shadow->watermarks("ADA-USD", "near").minuteThroughMs, 0);
  shadow.reset();
  eth.close();
}
// Day boundary, two products, live across midnight: both days checkpointed, no
// missing 23:59/00:00 column, history batch-identical, live forks once per day.
TEST_F(ShadowTest, MidnightRotationKeepsHistoryAndLiveForTwoProducts) {
  serve();
  fanout.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(
      fc, std::vector<std::string>{Product, "ETH-USD"}, *fanoutMetrics,
      [](const auto &) {});
  writer.reset();
  capture::WriterConfig bc, ec;
  bc.root = ec.root = QString::fromStdString(cfg.journalRoot);
  ec.symbol = "ETH-USD";
  bc.fsyncBlocks = ec.fsyncBlocks = 1;
  bc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(0, e); };
  ec.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(1, e); };
  capture::Writer btc(bc, meta()), eth(ec, meta("ETH-USD"));
  const int64_t Day = 86400000, from = Day - 120000;
  // Day-2 anchor replay re-applies 23:58..: served watermarks must not regress.
  std::atomic<int64_t> newest{0}, servedDuringReplay{INT64_MAX};
  std::atomic<int> replayed{0};
  std::atomic<ShadowRoller *> roller{nullptr};
  cfg.observeForTest = [&](const JournalRecord &r, bool didApply) {
    if (!didApply || r.pos.product != Product)
      return;
    const auto t = r.record.time.systemNs / 1000000;
    if (t < newest.load() && roller.load()) {
      ++replayed;
      servedDuringReplay =
          std::min(servedDuringReplay.load(),
                   roller.load()->watermarks(Product, "near").minuteThroughMs);
    }
    newest = std::max(newest.load(), t);
    std::lock_guard lock(mutex);
    applied.push_back(r.pos);
  };
  const auto append = [&](int64_t t0, int64_t t1) {
    for (auto t = t0; t <= t1; t += 1000) {
      btc.append(
          record(t, t == from ? snapshot() : "{\"channel\":\"heartbeats\"}"));
      eth.append(record(t, t == from ? snapshot("ETH-USD")
                                     : "{\"channel\":\"heartbeats\"}"));
    }
    btc.flush();
    eth.flush();
  };
  append(from, Day - 30000);
  shadow = std::make_unique<ShadowRoller>(
      cfg, std::vector<std::string>{Product, "ETH-USD"}, root / "primary",
      *metrics);
  roller = shadow.get();
  ASSERT_TRUE(
      eventually([&] { return forks() == 1 && forks("ETH-USD") == 1; }));
  append(Day - 29000, Day + 179000); // live across midnight
  ASSERT_TRUE(eventually([&] {
    for (const auto &p : {Product, std::string("ETH-USD")})
      for (const auto *layer : {"near", "deep"})
        if (shadow->watermarks(p, layer).minuteThroughMs < Epoch + Day + 120000)
          return false;
    return forks() == 2 && forks("ETH-USD") == 2 &&
           liveBucket(Epoch + Day + 120000);
  }));
  EXPECT_GT(replayed.load(), 0);
  EXPECT_GE(servedDuringReplay.load(), Epoch + Day);
  roller = nullptr;
  shadow.reset();
  btc.close();
  eth.close();
  for (const auto &p : {Product, std::string("ETH-USD")}) {
    roll({cfg.journalRoot, root / "batch", p, Epoch, Epoch + Day + 120000});
    const auto days = load(root / "shadow" / p / "roller.json").at("days");
    EXPECT_TRUE(days.contains(std::to_string(Epoch)));
    EXPECT_TRUE(days.contains(std::to_string(Epoch + Day)));
    for (const auto *layer : {"near", "deep"})
      EXPECT_EQ(recording::Hmc2Store::readRange(root / "shadow", p, layer,
                                                60000, Epoch + Day - 60000,
                                                Epoch + Day + 60000)
                    .size(),
                2u)
          << p << " " << layer;
  }
  EXPECT_EQ(contents(cfg.outputRoot), contents(root / "batch"));
  expectOrderedLive();
  EXPECT_EQ(expectFinalsMatch(root / "batch"), 2u * 2u * 4u); // 23:58..00:01
}
TEST_F(ShadowTest, LowPriceProductLiveAndHistoryRowsBelowHalfACent) {
  serve();
  fanout.reset();
  writer.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(
      fc, std::vector<std::string>{"PEPE-USD"}, *fanoutMetrics,
      [](const auto &) {});
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.symbol = "PEPE-USD";
  wc.fsyncBlocks = 1;
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(0, e); };
  capture::Writer pepe(wc, pepeMeta());
  pepe.append(record(0, pepeSnapshot()));
  for (int t = 1; t <= 63; ++t)
    pepe.append(record(t * 1000));
  pepe.flush();
  shadow = std::make_unique<ShadowRoller>(
      cfg, std::vector<std::string>{"PEPE-USD"}, root / "primary", *metrics);
  ASSERT_TRUE(eventually([&] { return forks("PEPE-USD") == 1; }));
  for (int t = 64; t <= 125; ++t)
    pepe.append(record(t * 1000));
  pepe.flush();
  ASSERT_TRUE(eventually([&] {
    return liveBucket(Epoch + 120000) &&
           shadow->watermarks("PEPE-USD", "deep").minuteThroughMs >=
               Epoch + 120000 &&
           shadow->watermarks("PEPE-USD", "near").minuteThroughMs >=
               Epoch + 120000;
  }));
  shadow.reset();
  pepe.close();
  const auto price = [](const recording::Hmc2Record &r,
                        const recording::Hmc2Entry &e) {
    return double(e.row) * double(r.header.rowTickUnits) / r.header.priceScale;
  };
  bool liveLow = false, historyLow = false;
  for (const auto &r : published())
    for (const auto &e : r->entries)
      liveLow = liveLow || (provisional(*r) && e.peakCode &&
                            price(*r, e) < 0.005 && price(*r, e) > 0);
  for (const auto &r : recording::Hmc2Store::readRange(
           root / "shadow", "PEPE-USD", "near", 60000, Epoch, Epoch + 120000))
    for (const auto &e : r.entries)
      historyLow = historyLow || (price(r, e) < 0.005 && price(r, e) > 0);
  EXPECT_TRUE(liveLow);
  EXPECT_TRUE(historyLow);
  roll({cfg.journalRoot, root / "batch", "PEPE-USD", Epoch, Epoch + 120000});
  EXPECT_EQ(contents(cfg.outputRoot), contents(root / "batch"));
  expectOrderedLive();
}
// A2-A5: recording.source: roller in the server ownership graph. No primary
// recorder; the model serves the roller root (output root == served root),
// live from the roller for a non-default product, watermarks from the roller.
TEST_F(ShadowTest, ServingRollerReplacesPrimaryRecorderForNonDefaultProduct) {
  fanout.reset();
  writer.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(
      fc, std::vector<std::string>{"PEPE-USD"}, *fanoutMetrics,
      [](const auto &) {});
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.symbol = "PEPE-USD";
  wc.fsyncBlocks = 1;
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(0, e); };
  capture::Writer pepe(wc, pepeMeta());
  pepe.append(record(0, pepeSnapshot()));
  for (int t = 1; t <= 63; ++t)
    pepe.append(record(t * 1000));
  pepe.flush();
  struct Cwd {
    QString old = QDir::currentPath();
    ~Cwd() { QDir::setCurrent(old); }
  } cwd;
  ASSERT_TRUE(QDir::setCurrent(temp.path()));
  ServerConfig config;
  config.defaultSymbols = {Product};
  config.recording.enabled = true;
  config.recording.source = "roller";
  config.recording.dir = (root / "primary").string();
  config.recording.fallbackDir.clear();
  config.heatmap.persistenceEnabled = false;
  config.rollerShadow = cfg;
  config.rollerShadow.products = {"PEPE-USD"};
  config.rollerShadow.liveNowForTest = [this] { return liveNow.load(); };
  SentinelServerApp app(config);
  ShadowServerTestAccess::prepare(app, root);
  auto &model = ShadowServerTestAccess::model(app);
  ASSERT_TRUE(model.recordingDir());
  EXPECT_EQ(*model.recordingDir(),
            fs::path(cfg.outputRoot)); // served == roller output
  ASSERT_NE(model.recordingLive(), nullptr);
  EXPECT_FALSE(ShadowServerTestAccess::hasPrimaryRecorder(app));
  EXPECT_FALSE(model.recordingAvailable()); // until the roller attaches
  std::mutex framesMutex;
  std::vector<heatmap::ChunkFrame> frames;
  auto raw = model.recordingLive()->subscribeRaw(
      {"PEPE-USD", {"hmc2.near"}, 1, 0},
      [&](const auto &, const std::string &, const recording::RawTailFrame &f) {
        if (f.bytes) {
          std::lock_guard lock(framesMutex);
          frames.push_back(heatmap::decodeChunk(*f.bytes));
        }
        return true;
      });
  ASSERT_TRUE(raw);
  ShadowServerTestAccess::start(app);
  EXPECT_TRUE(model.recordingAvailable()); // A4: no primary recorder
  EXPECT_FALSE(ShadowServerTestAccess::hasPrimaryRecorder(app));
  const auto text = [&] { return ShadowServerTestAccess::metrics(app); };
  // A5: no self-alias refusal of the served root.
  ASSERT_TRUE(eventually([&] {
    return text().find(
               "sentinel_roller_shadow_running{product=\"PEPE-USD\"} 1\n") !=
           std::string::npos;
  }));
  EXPECT_NE(text().find("sentinel_roller_shadow_setup_failures_total{product="
                        "\"PEPE-USD\"} 0\n"),
            std::string::npos);
  EXPECT_NE(text().find("sentinel_recorder_running 1\n"), std::string::npos);
  // Stall monitoring follows the roller's own worker state, not the engine
  // feed.
  ShadowServerTestAccess::checkProgress(app, Epoch + 63000);
  EXPECT_NE(text().find("sentinel_recorder_column_overdue_seconds{product="
                        "\"PEPE-USD\",layer=\"near\"}"),
            std::string::npos);
  EXPECT_EQ(text().find("sentinel_recorder_columns_written_total"),
            std::string::npos);
  // A3: watermarks and availability from the roller's hmc2 root.
  ASSERT_TRUE(eventually([&] {
    return model.recordingWatermarks("PEPE-USD", "near").minuteThroughMs >=
           Epoch + 60000;
  }));
  recording::ChunkService chunks(
      *model.recordingDir(),
      [&model](const std::string &s, const std::string &l) {
        return model.recordingWatermarks(s, l);
      },
      [] { return Epoch + 63000; });
  const auto sources = chunks.availability("PEPE-USD");
  ASSERT_FALSE(sources.empty());
  bool minuteLevel = false;
  for (const auto &level : sources.front().levels)
    if (level.levelMs == 60000) {
      minuteLevel = true;
      EXPECT_GE(level.committedThroughMs, Epoch + 60000);
      EXPECT_TRUE(level.latestMs.has_value());
    }
  EXPECT_TRUE(minuteLevel);
  // A2: a LiveService subscriber receives the roller's forming minute, with
  // native rows far below $0.005.
  for (int t = 64; t <= 70; ++t)
    pepe.append(record(t * 1000));
  pepe.flush();
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(framesMutex);
    for (const auto &f : frames)
      for (const auto &c : f.columns.columns)
        if (c.bucketStartMs == Epoch + 60000 &&
            (c.flags & recording::kProvisional))
          for (const auto &n : c.native)
            for (const auto &e : n.entries) {
              const auto price = double(n.baseRow + e.row()) *
                                 n.grid.rowTickUnits / n.grid.priceScale;
              if (price > 0 && price < 0.005)
                return true;
            }
    return false;
  }));
  EXPECT_FALSE(fs::exists(root / "primary" / "PEPE-USD"));
  EXPECT_FALSE(fs::exists(root / "primary" / Product));
}
TEST(ShadowConfig, ProductsAndRecordingSourceKeys) {
  QTemporaryDir tmp;
  const auto p = fs::path(tmp.path().toStdString()) / "config.yaml";
  {
    std::ofstream out(p);
    out << "default_symbols: [BTC-USD]\nrecording:\n  source: "
           "roller\nroller_shadow:\n"
           "  products: [btc-usd, PEPE-USD, PEPE-USD]\n";
  }
  ServerConfig c;
  EXPECT_EQ(c.recording.source, "primary");
  EXPECT_EQ(rollerProducts(c), std::vector<std::string>{"BTC-USD"});
  ASSERT_TRUE(ConfigLoader::loadServerConfig(p.string(), &c));
  EXPECT_EQ(c.recording.source, "roller");
  EXPECT_EQ(c.rollerShadow.products,
            (std::vector<std::string>{"BTC-USD", "PEPE-USD"}));
  EXPECT_EQ(rollerProducts(c),
            (std::vector<std::string>{"BTC-USD", "PEPE-USD"}));
}
// A8: --require-recording checks the root the configured source serves.
TEST(ShadowConfig, RequireRecordingChecksTheServedRoot) {
  QTemporaryDir tmp;
  const fs::path root = tmp.path().toStdString();
  fs::create_directories(root / "hmc2");
  fs::create_directories(root / "primary");
  ServerConfig c;
  c.recording.enabled = true;
  c.recording.source = "roller";
  c.recording.dir = (root / "absent-primary").string();
  c.recording.fallbackDir = (root / "primary").string();
  c.rollerShadow.enabled = true;
  c.rollerShadow.outputRoot = (root / "hmc2").string();
  auto copy = c;
  EXPECT_EQ(SentinelServerApp::requiredRecordingProblem(copy), "");
  EXPECT_TRUE(copy.recording.fallbackDir.empty()); // never the system disk
  copy = c;
  copy.rollerShadow.outputRoot = (root / "absent-hmc2").string();
  EXPECT_NE(SentinelServerApp::requiredRecordingProblem(copy), "");
  copy = c;
  copy.rollerShadow.enabled = false;
  EXPECT_NE(SentinelServerApp::requiredRecordingProblem(copy), "");
  copy = c;
  copy.recording.source = "primary";
  EXPECT_NE(SentinelServerApp::requiredRecordingProblem(copy), "");
  copy.recording.dir = (root / "primary").string();
  EXPECT_EQ(SentinelServerApp::requiredRecordingProblem(copy), "");
  copy.recording.source = "rolling";
  EXPECT_NE(SentinelServerApp::requiredRecordingProblem(copy), "");
}
} // namespace
