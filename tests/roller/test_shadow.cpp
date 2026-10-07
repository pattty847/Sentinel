#include "../../apps/sentinel-server/SentinelServerApp.hpp"
#include "ConfigLoader.hpp"
#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "heatmap/LiveEdge.hpp"
#include "roller/ShadowRoller.hpp"
#include "servermodel/ChunkService.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QTcpServer>
#include <QTemporaryDir>
#include <fstream>
#include <sys/resource.h>
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
  static void stopCandleTimer(ServerDataModel &m) { m.m_candleTimer.stop(); }
  static void tickCandles(ServerDataModel &m, int64_t ms) {
    m.m_aggregator->tick(ms);
  }
  static bool hasEngine(SentinelServerApp &app) {
    return app.m_marketDataCore != nullptr;
  }
  static SentinelStreamServer &server(SentinelServerApp &app) {
    return *app.m_server;
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
  // Fan-out "resnapshot" requests (capture reconnects); the roller sends none.
  std::atomic<int> resnapshots{0};
  void startFanout(size_t ringBytes = 32 * 1024 * 1024,
                   size_t clientBytes = 16 * 1024 * 1024) {
    capture::FanoutConfig c;
    c.socketPath = QString::fromStdString(cfg.socketPath);
    c.ringBytes = ringBytes;
    c.clientBytes = clientBytes;
    fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
    fanout = std::make_unique<capture::CaptureFanout>(
        c, std::vector{Product}, *fanoutMetrics,
        [this](const auto &) { ++resnapshots; });
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
    cfg.retractLive = [this](const std::string &) {
      std::lock_guard lock(liveMutex);
      retracts.push_back(live.size());
    };
  }
  // Production wiring into a real LiveService (as SentinelServerApp does).
  void serveTo(std::shared_ptr<recording::LiveService> service) {
    cfg.liveNowForTest = [this] { return liveNow.load(); };
    cfg.publisher = [service](std::shared_ptr<const recording::Hmc2Record> r) {
      service->publish(std::move(r));
    };
    cfg.retractLive = [service](const std::string &p) {
      service->retractProvisional(p);
    };
    cfg.ensureLiveFinal =
        [service](const std::string &p, const std::string &l,
                  std::shared_ptr<const recording::Hmc2Record> r) {
          return service->ensureFinal(p, l, std::move(r));
        };
  }
  std::vector<size_t> retracts; // live.size() at each withdrawal
  size_t retractCount() {
    std::lock_guard lock(liveMutex);
    return retracts.size();
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
  // No duplicate or out-of-order live record: within one lead (between
  // withdrawals) the forming minute never moves back, and each committed
  // minute is handed over once, in order.
  void expectOrderedLive() {
    std::map<std::pair<std::string, std::string>, int64_t> forming, finals;
    const auto all = published();
    std::set<size_t> cuts;
    {
      std::lock_guard lock(liveMutex);
      cuts.insert(retracts.begin(), retracts.end());
    }
    for (size_t i = 0; i < all.size(); ++i) {
      if (cuts.contains(i))
        forming.clear();
      const auto &r = all[i];
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
  // recording.live_feed: journal (slice D-b1). The sink stands in for the
  // queued hand-offs into ServerDataModel; each event notes how many durable
  // records history had applied when the worker handed it over.
  struct ModelEvent {
    std::string kind, product, reason;
    std::vector<OrderBookLevel> bids, asks;
    std::vector<BookLevelUpdate> updates;
    Trade trade{};
    bool connected = false;
    json metadata;
    size_t applied = 0;
  };
  std::mutex modelMutex;
  std::vector<ModelEvent> modelEvents;
  void tapModel() {
    const auto push = [this](ModelEvent e) {
      e.applied = count();
      std::lock_guard lock(modelMutex);
      modelEvents.push_back(std::move(e));
    };
    cfg.model.snapshot = [push](const std::string &p,
                                std::vector<OrderBookLevel> bids,
                                std::vector<OrderBookLevel> asks, int64_t) {
      ModelEvent e;
      e.kind = "snapshot";
      e.product = p;
      e.bids = std::move(bids);
      e.asks = std::move(asks);
      push(std::move(e));
    };
    cfg.model.updates = [push](const std::string &p,
                               std::vector<BookLevelUpdate> updates, int64_t) {
      ModelEvent e;
      e.kind = "updates";
      e.product = p;
      e.updates = std::move(updates);
      push(std::move(e));
    };
    cfg.model.invalidate = [push](const std::string &p,
                                  const std::string &reason) {
      ModelEvent e;
      e.kind = "invalidate";
      e.product = p;
      e.reason = reason;
      push(std::move(e));
    };
    cfg.model.trade = [push](const Trade &t) {
      ModelEvent e;
      e.kind = "trade";
      e.product = t.product_id;
      e.trade = t;
      push(std::move(e));
    };
    cfg.model.connection = [push](const std::string &p, bool connected) {
      ModelEvent e;
      e.kind = "connection";
      e.product = p;
      e.connected = connected;
      push(std::move(e));
    };
    cfg.model.watermark = [push](const std::string &p, int64_t ms) {
      ModelEvent e;
      e.kind = "watermark";
      e.product = p;
      e.trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
      push(std::move(e));
    };
    cfg.model.metadata = [push](const std::string &p, const json &m) {
      ModelEvent e;
      e.kind = "metadata";
      e.product = p;
      e.metadata = m;
      push(std::move(e));
    };
  }
  std::vector<ModelEvent> modelLog(const std::string &kind = {},
                                   const std::string &product = Product) {
    std::lock_guard lock(modelMutex);
    std::vector<ModelEvent> out;
    for (const auto &e : modelEvents)
      if ((kind.empty() || e.kind == kind) && e.product == product)
        out.push_back(e);
    return out;
  }
  size_t modelCount(const std::string &kind,
                    const std::string &product = Product) {
    return modelLog(kind, product).size();
  }
  // Position in the product's event log of the first update setting
  // price to qty, or npos.
  size_t updateAt(double price, double qty,
                  const std::string &product = Product) {
    const auto log = modelLog({}, product);
    for (size_t i = 0; i < log.size(); ++i)
      for (const auto &u : log[i].updates)
        if (log[i].kind == "updates" && u.price == price && u.quantity == qty)
          return i;
    return std::string::npos;
  }
  using RawBook = std::pair<std::map<double, double>, std::map<double, double>>;
  // Batch reference: every journal record on disk through a fresh JournalFeed.
  RawBook batchBook(const std::string &product = Product) {
    RawBook b;
    JournalFeed f(product);
    const auto apply = [&b](const std::vector<recording::Level> &v) {
      for (const auto &l : v) {
        auto &side = l.isBid ? b.first : b.second;
        if (l.size > 0)
          side[l.price] = l.size;
        else
          side.erase(l.price);
      }
    };
    f.onSnapshot = [&](int64_t, int64_t, std::vector<recording::Level> v) {
      b = {};
      apply(v);
    };
    f.onUpdates = [&](int64_t, int64_t, std::vector<recording::Level> v) {
      apply(v);
    };
    f.onInvalid = [&](int64_t, const std::string &) { b = {}; };
    JournalReader reader(cfg.journalRoot, product);
    JournalRecord r;
    while (reader.next(r))
      f.apply(r);
    return b;
  }
  static RawBook eventBook(const ModelEvent &e) {
    RawBook b;
    for (const auto &l : e.bids)
      b.first[l.price] = l.size;
    for (const auto &l : e.asks)
      b.second[l.price] = l.size;
    return b;
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
// A11, withdrawn suffix crossing a minute: the dropped lead's publications are
// withdrawn before the rebuilt lead publishes, and the rebuilt lead may then
// correct the earlier minute.
TEST_F(ShadowTest, RetractAcrossMinuteWithdrawsBeforeRebuilding) {
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
  ASSERT_EQ(retractCount(), 1u);
  size_t mark = 0; // first publication after the withdrawal
  {
    std::lock_guard lock(liveMutex);
    mark = retracts.front();
  }
  for (int t = 64; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually(
      [&] { return count() == 186 && liveBucket(Epoch + 180000, mark); }));
  shadow.reset();
  writer->close();
  EXPECT_TRUE(
      liveBucket(Epoch + 60000, mark)); // the earlier minute is corrected
  EXPECT_FALSE(livePublishedRow(100500, mark));
  expectOrderedLive();
  parity(180000);
  EXPECT_EQ(expectFinalsMatch(root / "batch"), 3u * 2u);
}
// A raw-tail client: every frame decoded into the client's own LiveEdge.
struct LiveClient {
  std::mutex mutex;
  heatmap::ChunkStore store;
  heatmap::LiveEdge edge{Product, "hmc2.near"};
  std::vector<std::shared_ptr<const heatmap::ChunkFrame>> frames;
};
using Client = LiveClient;
std::shared_ptr<recording::LiveService::RawSubscription>
subscribeClient(recording::LiveService &service,
                std::shared_ptr<LiveClient> client) {
  return service.subscribeRaw({Product, {"hmc2.near"}, 1, 0},
                              [client](const auto &, const std::string &,
                                       const recording::RawTailFrame &f) {
                                auto frame =
                                    std::make_shared<const heatmap::ChunkFrame>(
                                        heatmap::decodeChunk(*f.bytes));
                                std::lock_guard lock(client->mutex);
                                client->edge.accept(frame, client->store);
                                client->frames.push_back(std::move(frame));
                                return true;
                              });
}
// Withdrawn input in these tests: the recovery snapshot's ask 100500 and every
// column from minute 2 on (the durable book is unobserved there).
bool withdrawnColumn(const heatmap::SparseColumn &c) {
  if (c.bucketStartMs >= Epoch + 120000)
    return true;
  for (const auto &n : c.native)
    for (const auto &e : n.entries)
      if (e.isAsk() && (n.baseRow + int64_t(e.row())) * n.grid.rowTickUnits /
                               n.grid.priceScale ==
                           100500)
        return true;
  return false;
}
bool frameShows(const heatmap::ChunkFrame &f) {
  return std::any_of(f.columns.columns.begin(), f.columns.columns.end(),
                     withdrawnColumn);
}
bool edgeShows(LiveClient &c) {
  std::lock_guard lock(c.mutex);
  for (const auto &[bucket, column] : c.edge.snapshot()->minutes)
    if (withdrawnColumn(*column))
      return true;
  return false;
}
// Clean baseline frames may come first; after the first frame carrying the
// withdrawn data, a clearing frame follows and nothing withdrawn reappears.
void expectShownThenCleared(LiveClient &c) {
  std::lock_guard lock(c.mutex);
  const auto shown = std::find_if(c.frames.begin(), c.frames.end(),
                                  [](const auto &f) { return frameShows(*f); });
  ASSERT_NE(shown, c.frames.end());
  const auto cleared = std::find_if(
      shown, c.frames.end(), [](const auto &f) { return !frameShows(*f); });
  ASSERT_NE(cleared, c.frames.end());
  EXPECT_TRUE(std::none_of(cleared, c.frames.end(),
                           [](const auto &f) { return frameShows(*f); }));
}
// Finding 1 (review r1): a withdrawn provisional recovery snapshot, across a
// minute boundary, on an invalid durable book that no replacement snapshot
// revalidates. Real LiveService raw subscribers decode every frame into the
// client's LiveEdge: the existing subscriber drops the withdrawn minutes, a
// subscriber that connects after the withdrawal never receives them.
TEST_F(ShadowTest, WithdrawnRecoverySnapshotLeavesLiveServiceSubscribers) {
  auto service = std::make_shared<recording::LiveService>(cfg.outputRoot);
  serveTo(service);
  const auto subscribe = [&](std::shared_ptr<Client> client) {
    return subscribeClient(*service, client);
  };
  auto existing = std::make_shared<Client>();
  auto existingSub = subscribe(existing);
  ASSERT_TRUE(existingSub);
  // Durable: valid book, invalid from 64 s, heartbeats into minute 2.
  writer->append(record(0, snapshot()));
  for (int t = 1; t <= 125; ++t)
    writer->append(record(t * 1000, t == 64 ? "not JSON"
                                            : "{\"channel\":\"heartbeats\"}"));
  writer->flush();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 126 && forks() == 1; }));
  // Provisional recovery snapshot in minute 2, then a record in minute 3.
  auto recovery = json::parse(snapshot());
  recovery["events"][0]["updates"].push_back(
      {{"side", "offer"}, {"price_level", "100500"}, {"new_quantity", "7"}});
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(170000, recovery.dump()))});
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 1, true, frame(record(181000))});
  ASSERT_TRUE(eventually(
      [&] { return edgeShows(*existing); })); // option C: shown early
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] { return forks() == 2; }));
  // No replacement snapshot: only durable heartbeats follow.
  for (int t = 126; t <= 200; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 201; }));
  auto late = std::make_shared<Client>();
  auto lateSub = subscribe(late);
  ASSERT_TRUE(lateSub);
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(late->mutex);
    return !late->frames.empty();
  }));
  ASSERT_TRUE(eventually([&] { return !edgeShows(*existing); }));
  std::this_thread::sleep_for(500ms); // further worker turns change nothing
  shadow.reset();
  EXPECT_FALSE(edgeShows(*existing));
  EXPECT_FALSE(edgeShows(*late));
  expectShownThenCleared(*existing); // once withdrawn, never shown again
  {
    std::lock_guard lock(existing->mutex);
    // The committed minute 1 stays (final, durable observation only).
    EXPECT_TRUE(existing->edge.snapshot()->minutes.contains(Epoch + 60000));
  }
  {
    std::lock_guard lock(late->mutex);
    for (const auto &f : late->frames)
      EXPECT_FALSE(frameShows(*f));
  }
  writer->close();
  parity(180000);
}
// Review r1 finding 2: the deploy marker follows every product's history writer
// (leases and checkpoint policy), never just the "Roller started" line.
namespace logcapture {
std::mutex mutex;
std::vector<std::string> lines;
QtMessageHandler previous = nullptr;
void handler(QtMsgType type, const QMessageLogContext &context,
             const QString &message) {
  {
    std::lock_guard lock(mutex);
    lines.push_back(message.toStdString());
  }
  if (previous)
    previous(type, context, message);
}
bool seen(const std::string &text) {
  std::lock_guard lock(mutex);
  return std::any_of(lines.begin(), lines.end(), [&](const auto &l) {
    return l.find(text) != std::string::npos;
  });
}
} // namespace logcapture
TEST_F(ShadowTest, ServingReadinessWaitsForEveryProductWriter) {
  {
    std::lock_guard lock(logcapture::mutex);
    logcapture::lines.clear();
  }
  logcapture::previous = qInstallMessageHandler(logcapture::handler);
  struct Restore {
    ~Restore() { qInstallMessageHandler(logcapture::previous); }
  } restore;
  serve();
  initial(125); // a checkpoint follows the first commit at the next minute
  // Another writer holds the product lease (for example a repair roll).
  auto holder =
      std::make_unique<recording::Hmc2Store>(root / "shadow", true, Product);
  start();
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  EXPECT_TRUE(logcapture::seen("Roller started product=BTC-USD mode=live"));
  EXPECT_FALSE(logcapture::seen("Roller writer open product=BTC-USD"));
  EXPECT_FALSE(logcapture::seen("Roller serving ready"));
  holder.reset();
  ASSERT_TRUE(eventually(
      [&] { return logcapture::seen("Roller serving ready products=1"); }));
  EXPECT_TRUE(logcapture::seen("Roller writer open product=BTC-USD"));
  shadow.reset();
}
// Review r2 finding 1: zero cached finals. After a restart history replays
// below its commit floor and republishes no final, so the live cache is empty.
// An existing subscriber receives a provisional minute that is then withdrawn
// (invalid durable book, no replacement snapshot): it must drop it. The lead
// first seeds the newest persisted minute, so the withdrawal frame can resend
// it.
TEST_F(ShadowTest, RestartWithdrawalWithZeroCachedFinalsReachesSubscriber) {
  writer->append(record(0, snapshot()));
  for (int t = 1; t <= 185; ++t)
    writer->append(record(t * 1000, t == 64 ? "not JSON"
                                            : "{\"channel\":\"heartbeats\"}"));
  writer->flush();
  serveTo(std::make_shared<recording::LiveService>(cfg.outputRoot));
  start();
  ASSERT_TRUE(eventually([&] {
    return fs::exists(root / "shadow" / Product / "roller.json") &&
           load(root / "shadow" / Product / "roller.json")
                   .value("committedThroughMs", int64_t{0}) >= Epoch + 120000;
  }));
  shadow.reset();
  // Restarted process: a new, empty live cache with an existing subscriber.
  auto service = std::make_shared<recording::LiveService>(cfg.outputRoot);
  serveTo(service);
  auto existing = std::make_shared<LiveClient>();
  auto sub = subscribeClient(*service, existing);
  ASSERT_TRUE(sub);
  start();
  ASSERT_TRUE(eventually([&] { return forks() == 1; }));
  auto recovery = json::parse(snapshot());
  recovery["events"][0]["updates"].push_back(
      {{"side", "offer"}, {"price_level", "100500"}, {"new_quantity", "7"}});
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(190000, recovery.dump()))});
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 1, true, frame(record(195000))});
  ASSERT_TRUE(eventually([&] { return edgeShows(*existing); }));
  fanout->publish(0, {capture::JournalEventKind::Retract,
                      durable.runId,
                      durable.block,
                      durable.record,
                      true,
                      {}});
  ASSERT_TRUE(eventually([&] { return forks() == 2; }));
  ASSERT_TRUE(eventually([&] { return !edgeShows(*existing); }));
  shadow.reset();
  expectShownThenCleared(*existing);
}
// Review r2 finding 1: on a root with no committed minute yet the lead does not
// publish (nothing could carry a withdrawal) until history commits one.
TEST_F(ShadowTest, LeadWaitsForACommittedMinuteOnANewRoot) {
  auto service = std::make_shared<recording::LiveService>(cfg.outputRoot);
  serveTo(service);
  auto client = std::make_shared<LiveClient>();
  auto sub = subscribeClient(*service, client);
  ASSERT_TRUE(sub);
  writer->append(record(0, snapshot()));
  for (int t = 1; t <= 30; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  start();
  ASSERT_TRUE(eventually([&] { return count() == 31; }));
  std::this_thread::sleep_for(1500ms); // past the 1 s gate retry
  EXPECT_EQ(forks(), 0);
  {
    std::lock_guard lock(client->mutex);
    EXPECT_TRUE(client->frames.empty());
  }
  for (int t = 31; t <= 70; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return forks() == 1; }));
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(client->mutex);
    return std::any_of(
        client->frames.begin(), client->frames.end(), [](const auto &f) {
          return std::any_of(
              f->columns.columns.begin(), f->columns.columns.end(),
              [](const auto &c) { return c.flags & recording::kProvisional; });
        });
  }));
  shadow.reset();
  std::lock_guard lock(client->mutex);
  // The first frame already carries a committed minute.
  const auto &first = client->frames.front()->columns.columns;
  EXPECT_TRUE(std::any_of(first.begin(), first.end(), [](const auto &c) {
    return !(c.flags & recording::kProvisional);
  }));
}
// Review r2 finding 2: readiness is current durable health of every product at
// once. BTC opens, checkpoints, then its checkpoint fails; ETH opens only
// after that: no ready line until BTC recovers.
TEST_F(ShadowTest, ServingReadinessNeedsEveryProductHealthyAtOnce) {
  {
    std::lock_guard lock(logcapture::mutex);
    logcapture::lines.clear();
  }
  logcapture::previous = qInstallMessageHandler(logcapture::handler);
  struct Restore {
    ~Restore() { qInstallMessageHandler(logcapture::previous); }
  } restore;
  serve();
  fanout.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(
      fc, std::vector<std::string>{Product, "ETH-USD"}, *fanoutMetrics,
      [](const auto &) {});
  initial(125);
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.symbol = "ETH-USD";
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(1, e); };
  capture::Writer eth(wc, meta("ETH-USD"));
  eth.append(record(0, snapshot("ETH-USD")));
  for (int t = 1; t <= 125; ++t)
    eth.append(record(t * 1000));
  eth.flush();
  auto ethLease =
      std::make_unique<recording::Hmc2Store>(root / "shadow", true, "ETH-USD");
  shadow = std::make_unique<ShadowRoller>(
      cfg, std::vector<std::string>{Product, "ETH-USD"}, root / "primary",
      *metrics);
  ASSERT_TRUE(eventually([&] {
    return logcapture::seen("Roller writer healthy product=BTC-USD");
  }));
  // BTC's next durable checkpoint fails (persistence failure after open).
  const auto checkpoint = root / "shadow" / Product / "roller.json";
  const auto saved = [&] {
    std::ifstream in(checkpoint);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();
  ASSERT_TRUE(fs::remove(checkpoint));
  fs::create_directory(checkpoint);
  {
    std::ofstream(checkpoint / "blocker") << "blocked";
  }
  for (int t = 126; t <= 185; ++t)
    writer->append(record(t * 1000));
  writer->flush();
  ASSERT_TRUE(eventually([&] {
    return logcapture::seen("Roller serving not ready product=BTC-USD");
  }));
  ethLease.reset();
  ASSERT_TRUE(eventually([&] {
    return logcapture::seen("Roller writer healthy product=ETH-USD");
  }));
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(logcapture::seen("Roller serving ready"));
  fs::remove_all(checkpoint);
  {
    std::ofstream(checkpoint) << saved;
  }
  ASSERT_TRUE(eventually(
      [&] { return logcapture::seen("Roller serving ready products=2"); }));
  shadow.reset();
  eth.close();
}
// Review r2 finding 2: a writer that opens but cannot persist its first
// checkpoint is not healthy (fresh root, read-only product directory).
TEST_F(ShadowTest, ServingReadinessNeedsADurableCheckpointAfterOpen) {
  {
    std::lock_guard lock(logcapture::mutex);
    logcapture::lines.clear();
  }
  logcapture::previous = qInstallMessageHandler(logcapture::handler);
  struct Restore {
    ~Restore() { qInstallMessageHandler(logcapture::previous); }
  } restore;
  serve();
  initial(125);
  const auto dir = root / "shadow" / Product;
  fs::create_directories(dir);
  fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
  start();
  ASSERT_TRUE(eventually(
      [&] { return logcapture::seen("Roller writer open product=BTC-USD"); }));
  ASSERT_TRUE(eventually([&] {
    return !has(
        "sentinel_roller_shadow_setup_failures_total{product=\"BTC-USD\"} 0");
  }));
  EXPECT_FALSE(logcapture::seen("Roller writer healthy product=BTC-USD"));
  EXPECT_FALSE(logcapture::seen("Roller serving ready"));
  fs::permissions(dir, fs::perms::owner_all);
  ASSERT_TRUE(eventually(
      [&] { return logcapture::seen("Roller serving ready products=1"); }));
  shadow.reset();
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

// ---------------------------------------------------------------------------
// Slice D-b1: recording.live_feed: journal. The roller workers feed the
// server's live model (ModelSink) from the journal and fan-out.
// ---------------------------------------------------------------------------
std::string trades(const std::string &side, const std::string &id,
                   const std::string &product = Product,
                   const std::string &price = "100000") {
  return json({{"channel", "market_trades"},
               {"events",
                json::array({{{"type", "update"},
                              {"trades", json::array({{{"trade_id", id},
                                                       {"product_id", product},
                                                       {"price", price},
                                                       {"size", "0.5"},
                                                       {"side", side},
                                                       {"time",
                                                        "2026-10-01T00:01:"
                                                        "05Z"}}})}}})}})
      .dump();
}
capture::Record lifecycle(int64_t ms, capture::Kind kind) {
  return {kind,
          {Epoch * 1000000 + ms * 1000000, capture::Stamp::now().steadyNs},
          1,
          "{}"};
}
std::string heartbeat() { return "{\"channel\":\"heartbeats\"}"; }
// A1: the first tip hands over one synthesized snapshot equal to a batch
// replay of the durable journal (anchor plus durable updates), then every
// provisional record's updates on arrival, before its durable marker.
TEST_F(ShadowTest, JournalTapSeedsOneSnapshotEqualToBatchThenProvisionalUpdates) {
  tapModel();
  writer->append(record(0, snapshot()));
  for (int t = 1; t <= 63; ++t)
    writer->append(record(t * 1000, t == 10   ? offer("100002", "4")
                                    : t == 20 ? offer("100001", "0")
                                              : heartbeat()));
  writer->flush();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  const auto seed = modelLog("snapshot").front();
  EXPECT_EQ(seed.applied, 64u); // after catch-up, at the durable tip
  const auto batch = batchBook();
  EXPECT_EQ(eventBook(seed), batch);
  EXPECT_TRUE(batch.second.contains(100002));
  EXPECT_FALSE(batch.second.contains(100001));
  EXPECT_EQ(modelCount("metadata"), 1u);
  ASSERT_EQ(modelCount("connection"), 1u);
  EXPECT_TRUE(modelLog("connection").front().connected);
  // Provisional (published, not yet durable): handed over on arrival.
  writer->append(record(64000, offer("100500", "7")));
  writer->append(record(65000, offer("100500", "0")));
  ASSERT_TRUE(eventually([&] {
    return updateAt(100500, 0) != std::string::npos;
  }));
  EXPECT_EQ(count(), 64u); // neither is durable yet
  EXPECT_LT(updateAt(100500, 7), updateAt(100500, 0)); // arrival order
  for (const auto &e : modelLog("updates"))
    EXPECT_EQ(e.applied, 64u);
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 66; }));
  std::this_thread::sleep_for(100ms);
  // The durable marker adds nothing: one seed, two updates, no invalidation.
  EXPECT_EQ(modelCount("snapshot"), 1u);
  EXPECT_EQ(modelCount("updates"), 2u);
  EXPECT_EQ(modelCount("invalidate"), 0u);
  EXPECT_EQ(resnapshots.load(), 0);
  shadow.reset();
  writer->close();
}
// A2 shared check: the provisional level reached the model, then recovery
// invalidated the book and re-seeded it from durable state only.
void expectReseededFromDurable(ShadowTest &t, size_t updateIndex) {
  const auto log = t.modelLog();
  size_t invalid = std::string::npos, reseed = std::string::npos;
  for (size_t i = updateIndex + 1; i < log.size(); ++i) {
    if (invalid == std::string::npos && log[i].kind == "invalidate")
      invalid = i;
    if (invalid != std::string::npos && log[i].kind == "snapshot") {
      reseed = i;
      break;
    }
  }
  ASSERT_NE(invalid, std::string::npos);
  ASSERT_NE(reseed, std::string::npos);
  EXPECT_EQ(t.eventBook(log[reseed]), t.batchBook());
  EXPECT_FALSE(t.eventBook(log[reseed]).second.contains(100500));
  EXPECT_EQ(t.resnapshots.load(), 0); // never asks capture to reconnect
}
// Scenario tail: after the re-seed the book is live again (a new provisional
// level arrives before its durable marker) and history is batch-identical.
void expectLiveAgainWithHistoryUnchanged(ShadowTest &t, int64_t fromSecond) {
  for (int s = int(fromSecond); s <= 125; ++s)
    t.writer->append(record(s * 1000, s == 100 ? offer("100004", "2") : heartbeat()));
  ASSERT_TRUE(eventually([&] { return t.updateAt(100004, 2) != std::string::npos; }));
  EXPECT_LT(t.modelLog("updates").back().applied, 126u);
  t.writer->flush();
  ASSERT_TRUE(eventually([&] { return t.count() == 126; }));
  t.shadow.reset();
  t.writer->close();
  t.parity(120000);
}
TEST_F(ShadowTest, JournalTapRetractInvalidatesThenReseedsDurableState) {
  tapModel();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] {
    return updateAt(100500, 7) != std::string::npos;
  }));
  const auto update = updateAt(100500, 7);
  fanout->publish(0, {capture::JournalEventKind::Retract, durable.runId,
                      durable.block, durable.record, true, {}});
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 2; }));
  expectReseededFromDurable(*this, update);
  EXPECT_EQ(count(), 64u);
  expectLiveAgainWithHistoryUnchanged(*this, 64);
}
TEST_F(ShadowTest, JournalTapDisconnectInvalidatesThenReseedsDurableState) {
  tapModel();
  fanout.reset();
  startFanout(32 * 1024 * 1024, 64 * 1024);
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] {
    return updateAt(100500, 7) != std::string::npos;
  }));
  const auto update = updateAt(100500, 7);
  // Larger than the client budget: the fan-out sends "disconnect" and closes.
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 1, true,
                      frame(record(64001, std::string(128 * 1024, ' ')))});
  // Withdraw both from the ring, so the reconnect resumes at the durable tip.
  fanout->publish(0, {capture::JournalEventKind::Retract, durable.runId,
                      durable.block, durable.record, true, {}});
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") >= 2; }));
  EXPECT_EQ(fanoutMetrics->render().find(
                "sentinel_fanout_disconnects_total{reason=\"slow_client\"} 0\n"),
            std::string::npos);
  expectReseededFromDurable(*this, update);
  expectLiveAgainWithHistoryUnchanged(*this, 64);
}
TEST_F(ShadowTest, JournalTapEofInvalidatesThenReseedsDurableState) {
  serve(); // with the live heatmap lead, as served
  tapModel();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  fanout->publish(0, {capture::JournalEventKind::Record, durable.runId,
                      durable.block + 1, 0, true,
                      frame(record(64000, offer("100500", "7")))});
  ASSERT_TRUE(eventually([&] {
    return updateAt(100500, 7) != std::string::npos;
  }));
  const auto update = updateAt(100500, 7);
  fanout.reset(); // EOF
  ASSERT_TRUE(eventually([&] { return modelCount("invalidate") == 1; }));
  for (int t = 64; t <= 125; ++t)
    writer->append(record(t * 1000, t == 70 ? offer("100003", "5") : heartbeat()));
  writer->flush();
  startFanout(); // empty ring: file catch-up to the first new durable marker
  writer->append(record(126000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 2; }));
  EXPECT_EQ(modelLog("snapshot").back().applied, 127u);
  EXPECT_TRUE(eventBook(modelLog("snapshot").back()).second.contains(100003));
  expectReseededFromDurable(*this, update);
  // The new fan-out held no resume cursor: journal catch-up (ring miss).
  EXPECT_EQ(fanoutMetrics->render().find(
                "sentinel_fanout_resume_misses_total{product=\"BTC-USD\"} 0\n"),
            std::string::npos);
  for (int s = 127; s <= 185; ++s)
    writer->append(record(s * 1000, s == 130 ? offer("100004", "2") : heartbeat()));
  ASSERT_TRUE(eventually([&] { return updateAt(100004, 2) != std::string::npos; }));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 186; }));
  shadow.reset();
  writer->close();
  parity(180000);
  expectOrderedLive();
}
// A capture freeze at the tip (FM-127) on the fake wall clock: 30 s without a
// socket record invalidates the book and reports disconnected; when records
// return the book re-seeds and is live, and history is untouched.
TEST_F(ShadowTest, JournalTapThirtySecondTipSilenceInvalidatesThenReseeds) {
  serve();
  tapModel();
  initial();
  start();
  ASSERT_TRUE(eventually([&] {
    return modelCount("snapshot") == 1 && modelCount("connection") == 1;
  }));
  liveNow = Epoch + 29000;
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(modelCount("invalidate"), 0u); // 29 s is not silence
  liveNow = Epoch + 30000;
  ASSERT_TRUE(eventually([&] { return modelCount("invalidate") == 1; }));
  ASSERT_TRUE(eventually([&] { return modelCount("connection") == 2; }));
  EXPECT_FALSE(modelLog("connection").back().connected);
  EXPECT_EQ(modelLog("invalidate").front().reason, "journal tip silent");
  writer->append(record(64000)); // capture is back
  ASSERT_TRUE(eventually([&] {
    return modelCount("snapshot") == 2 && modelCount("connection") == 3;
  }));
  EXPECT_TRUE(modelLog("connection").back().connected);
  EXPECT_EQ(eventBook(modelLog("snapshot").back()), batchBook());
  expectLiveAgainWithHistoryUnchanged(*this, 65);
  expectOrderedLive();
}
// A7: after a restart the model gets nothing from the day's catch-up replay;
// the seed arrives at the durable tip, equal to batch.
TEST_F(ShadowTest, JournalTapRestartSeedsOnlyAfterCatchup) {
  serve();
  tapModel();
  writer->append(record(0, snapshot()));
  for (int t = 1; t <= 80; ++t)
    writer->append(record(t * 1000, t % 11 == 0 ? trades("BUY", "h" + std::to_string(t))
                                    : t % 7      ? heartbeat()
                                                 : offer(std::to_string(100001 + t),
                                                         std::to_string(t % 5))));
  writer->flush();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(modelCount("trade"), 0u); // a fresh process never replays the day
  shadow.reset();
  {
    std::lock_guard lock(modelMutex);
    modelEvents.clear();
  }
  {
    std::lock_guard lock(mutex);
    applied.clear();
  }
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  const auto log = modelLog();
  ASSERT_FALSE(log.empty());
  for (const auto &e : log)
    if (e.kind != "connection")
      EXPECT_EQ(e.applied, 81u) << e.kind;
  EXPECT_EQ(eventBook(modelLog("snapshot").front()), batchBook());
  EXPECT_EQ(modelCount("trade"), 0u);
  shadow.reset();
  writer->close();
}
// A8: the day rotation replays the new day from its anchor without telling
// the model: no invalidation, one re-seed at the new tip.
TEST_F(ShadowTest, JournalTapMidnightReseedsOnceWithoutInvalidation) {
  serve();
  tapModel();
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
  const auto append = [&](int64_t t0, int64_t t1) {
    for (auto t = t0; t <= t1; t += 1000) {
      const bool level = t % 13000 == 0;
      btc.append(record(t, t == from ? snapshot()
                           : level   ? offer(std::to_string(100100 + t / 1000 % 50), "1")
                                     : heartbeat()));
      eth.append(record(t, t == from ? snapshot("ETH-USD")
                           : level   ? offer(std::to_string(100100 + t / 1000 % 50), "2", "ETH-USD")
                                     : heartbeat()));
    }
    btc.flush();
    eth.flush();
  };
  append(from, Day - 30000);
  shadow = std::make_unique<ShadowRoller>(
      cfg, std::vector<std::string>{Product, "ETH-USD"}, root / "primary",
      *metrics);
  ASSERT_TRUE(eventually([&] {
    return modelCount("snapshot") == 1 && modelCount("snapshot", "ETH-USD") == 1;
  }));
  append(Day - 29000, Day + 179000);
  ASSERT_TRUE(eventually([&] {
    for (const auto &p : {Product, std::string("ETH-USD")})
      if (shadow->watermarks(p, "near").minuteThroughMs < Epoch + Day + 120000)
        return false;
    return modelCount("snapshot") == 2 && modelCount("snapshot", "ETH-USD") == 2;
  }));
  std::this_thread::sleep_for(100ms);
  for (const auto &p : {Product, std::string("ETH-USD")}) {
    EXPECT_EQ(modelCount("invalidate", p), 0u) << p;
    EXPECT_EQ(modelCount("snapshot", p), 2u) << p; // the seed and one re-seed
    ASSERT_EQ(modelCount("connection", p), 1u) << p;
    EXPECT_EQ(eventBook(modelLog("snapshot", p).back()), batchBook(p)) << p;
  }
  shadow.reset();
  btc.close();
  eth.close();
}
// The model's "await the next upstream snapshot" paths become a re-seed
// request: the worker answers with its current live book (provisional too).
TEST_F(ShadowTest, JournalTapReseedRequestSendsTheCurrentLiveBook) {
  tapModel();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  writer->append(record(64000, offer("100500", "7"))); // provisional
  ASSERT_TRUE(eventually([&] {
    return updateAt(100500, 7) != std::string::npos;
  }));
  shadow->requestReseed(Product);
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 2; }));
  auto expected = batchBook();
  expected.second[100500] = 7;
  EXPECT_EQ(eventBook(modelLog("snapshot").back()), expected);
  EXPECT_EQ(modelCount("invalidate"), 0u);
  shadow.reset();
  writer->close();
}
// Trades reach the model once per journal position: on arrival, never again
// when recovery reads the same record from the journal, and from durable
// catch-up only to fill a gap after the model was live.
TEST_F(ShadowTest, JournalTapDeliversEachTradeOnceAcrossRecovery) {
  tapModel();
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  writer->append(record(64000, trades("BUY", "t1"))); // provisional
  ASSERT_TRUE(eventually([&] { return modelCount("trade") == 1; }));
  // EOF before t1 is durable; capture keeps journaling (t1, then the gap t2).
  fanout.reset();
  ASSERT_TRUE(eventually([&] { return modelCount("invalidate") == 1; }));
  writer->append(record(65000));
  writer->flush();
  writer->append(record(66000, trades("SELL", "t2")));
  writer->flush();
  startFanout();
  writer->append(record(67000));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 2; }));
  EXPECT_EQ(count(), 68u);
  writer->append(record(68000, trades("BUY", "t3"))); // provisional again
  ASSERT_TRUE(eventually([&] { return modelCount("trade") == 3; }));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return count() == 69; }));
  std::this_thread::sleep_for(150ms);
  std::vector<std::string> ids;
  for (const auto &e : modelLog("trade"))
    ids.push_back(e.trade.trade_id);
  EXPECT_EQ(ids, (std::vector<std::string>{"t1", "t2", "t3"}));
  // The candle watermark follows journal time: strictly increasing, at most
  // one per journal second, and the gap record carrying t2 (66 s) advances it
  // after that trade.
  const auto log = modelLog();
  int64_t lastSecond = INT64_MIN;
  size_t gap = std::string::npos, gapMark = std::string::npos;
  for (size_t i = 0; i < log.size(); ++i) {
    if (log[i].kind == "trade" && log[i].trade.trade_id == "t2")
      gap = i;
    if (log[i].kind != "watermark")
      continue;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        log[i].trade.timestamp.time_since_epoch()).count();
    EXPECT_GT(ms / 1000, lastSecond) << i;
    lastSecond = ms / 1000;
    if (ms == Epoch + 66000)
      gapMark = i;
  }
  ASSERT_NE(gap, std::string::npos);
  ASSERT_NE(gapMark, std::string::npos);
  EXPECT_LT(gap, gapMark);
  // Coinbase reports the maker; the model gets the aggressor (A4).
  EXPECT_EQ(modelLog("trade")[0].trade.side, AggressorSide::Sell);
  EXPECT_EQ(modelLog("trade")[1].trade.side, AggressorSide::Buy);
  EXPECT_EQ(resnapshots.load(), 0);
  shadow.reset();
  writer->close();
}
// A reconnect that resumes from the fan-out ring replays provisional records
// the model already has: their trades are not delivered again.
TEST_F(ShadowTest, JournalTapRingReplayNeverRedeliversATrade) {
  tapModel();
  cfg.pendingBytes = 600; // two provisional records overflow: ring resume
  initial();
  start();
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") == 1; }));
  writer->append(record(64000, trades("BUY", "t1"))); // provisional
  ASSERT_TRUE(eventually([&] { return modelCount("trade") == 1; }));
  writer->append(record(65000, json({{"channel", "heartbeats"},
                                     {"pad", std::string(400, 'x')}})
                                   .dump()));
  // Each resume replays t1 and the padding from the ring and overflows again.
  ASSERT_TRUE(eventually([&] { return modelCount("snapshot") >= 3; }));
  writer->flush(); // durable: the resume catches up from the journal instead
  ASSERT_TRUE(eventually([&] { return count() == 66; }));
  writer->append(record(66000, trades("SELL", "t2")));
  writer->flush();
  ASSERT_TRUE(eventually([&] { return modelCount("trade") == 2; }));
  std::this_thread::sleep_for(150ms);
  std::vector<std::string> ids;
  for (const auto &e : modelLog("trade"))
    ids.push_back(e.trade.trade_id);
  EXPECT_EQ(ids, (std::vector<std::string>{"t1", "t2"}));
  shadow.reset();
  writer->close();
}
// Hot-path evidence (AGENTS 5): capture publish -> model hand-off latency for
// provisional records at the tip, and the per-record worker CPU of the tap.
TEST_F(ShadowTest, JournalTapHandoffLatencyBenchmark) {
  writer.reset();
  capture::WriterConfig wc;
  wc.root = QString::fromStdString(cfg.journalRoot);
  wc.onJournal = [&](const capture::JournalEvent &e) { fanout->publish(0, e); };
  capture::Session session(wc, meta());
  ASSERT_TRUE(session.submit(record(0, snapshot())));
  for (int t = 1; t <= 63; ++t)
    ASSERT_TRUE(session.submit(record(t * 1000)));
  std::mutex samplesMutex;
  std::vector<double> handed;
  std::atomic<bool> seeded{false};
  cfg.model.snapshot = [&](const std::string &, std::vector<OrderBookLevel>,
                           std::vector<OrderBookLevel>, int64_t) { seeded = true; };
  cfg.model.updates = [&](const std::string &, std::vector<BookLevelUpdate> u,
                          int64_t) {
    // The level size carries the submit time (steady us).
    const auto us = capture::Stamp::now().steadyNs / 1000. - u.front().quantity;
    std::lock_guard lock(samplesMutex);
    handed.push_back(us);
  };
  cfg.model.invalidate = [](const std::string &, const std::string &) {};
  cfg.model.trade = [](const Trade &) {};
  cfg.model.connection = [](const std::string &, bool) {};
  cfg.model.metadata = [](const std::string &, const json &) {};
  start();
  ASSERT_TRUE(eventually([&] { return seeded.load(); }));
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    const auto stamp = std::to_string(capture::Stamp::now().steadyNs / 1000);
    ASSERT_TRUE(session.submit(record(64000 + i, offer(std::to_string(100100 + i % 50), stamp))));
    std::this_thread::sleep_until(begin + (i + 1) * 10ms);
  }
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(samplesMutex);
    return handed.size() == 200;
  }));
  shadow.reset();
  session.close();
  std::sort(handed.begin(), handed.end());
  std::cout << "JOURNAL_TAP_HANDOFF samples=200 submit_to_model_us p50="
            << handed[99] << " p95=" << handed[189] << '\n';
}
// A7 cost evidence (run with --gtest_also_run_disabled_tests): process CPU for
// the day catch-up of 7 synthetic products, with and without the model tap.
TEST_F(ShadowTest, DISABLED_JournalTapCatchupCostSevenProducts) {
  const std::vector<std::string> products = {"BTC-USD", "ETH-USD", "SOL-USD", "FARTCOIN-USD",
                                             "PEPE-USD", "DOGE-USD", "AVAX-USD"};
  const int records = std::getenv("TAP_BENCH_RECORDS") ? std::atoi(std::getenv("TAP_BENCH_RECORDS")) : 20000;
  fanout.reset();
  writer.reset();
  fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
  capture::FanoutConfig fc;
  fc.socketPath = QString::fromStdString(cfg.socketPath);
  fanout = std::make_unique<capture::CaptureFanout>(fc, products, *fanoutMetrics, [](const auto &) {});
  std::vector<std::unique_ptr<capture::Writer>> writers;
  for (size_t i = 0; i < products.size(); ++i) {
    capture::WriterConfig wc;
    wc.root = QString::fromStdString(cfg.journalRoot);
    wc.symbol = products[i];
    wc.fsyncBlocks = 0;
    wc.onJournal = [this, i](const capture::JournalEvent &e) { fanout->publish(i, e); };
    writers.push_back(std::make_unique<capture::Writer>(wc, meta(products[i])));
    auto &w = *writers.back();
    w.append(record(0, snapshot(products[i])));
    for (int r = 1; r < records; ++r) {
      const auto ms = int64_t(r) * 3000000 / records * 1; // ~50 minutes
      if (r % 10 == 0)
        w.append(record(ms, trades(r % 20 ? "BUY" : "SELL", std::to_string(r), products[i])));
      else
        w.append(record(ms, json({{"channel", "l2_data"},
                                  {"events", json::array({{{"type", "update"},
                                    {"product_id", products[i]},
                                    {"updates", json::array({
                                      {{"side", "offer"}, {"price_level", std::to_string(100002 + r % 40)}, {"new_quantity", std::to_string(r % 9)}},
                                      {{"side", "bid"}, {"price_level", std::to_string(99998 - r % 40)}, {"new_quantity", std::to_string(r % 7)}},
                                      {{"side", "offer"}, {"price_level", std::to_string(100050 + r % 13)}, {"new_quantity", "1.5"}}})}}})}})
                                .dump()));
    }
    w.flush();
  }
  const auto cpu = [] {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
  };
  for (int round = 0; round < 3; ++round)
    for (const bool tapped : {false, true}) {
      cfg.outputRoot = (root / ("out-" + std::to_string(round) + (tapped ? "-tap" : "-plain"))).string();
      cfg.model = {};
      std::atomic<int> seeds{0};
      if (tapped) {
        cfg.model.snapshot = [&](const std::string &, std::vector<OrderBookLevel>,
                                 std::vector<OrderBookLevel>, int64_t) { ++seeds; };
        cfg.model.updates = [](const std::string &, std::vector<BookLevelUpdate>, int64_t) {};
        cfg.model.invalidate = [](const std::string &, const std::string &) {};
        cfg.model.trade = [](const Trade &) {};
        cfg.model.connection = [](const std::string &, bool) {};
        cfg.model.metadata = [](const std::string &, const json &) {};
      }
      {
        std::lock_guard lock(mutex);
        applied.clear();
      }
      const auto c0 = cpu();
      const auto w0 = std::chrono::steady_clock::now();
      metrics = std::make_unique<metrics::MetricsRegistry>();
      shadow = std::make_unique<ShadowRoller>(cfg, products, root / "primary", *metrics);
      const auto until = std::chrono::steady_clock::now() + 120s;
      while ((count() < products.size() * records ||
              (tapped && seeds.load() < int(products.size()))) &&
             std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(2ms);
      ASSERT_EQ(count(), products.size() * records);
      const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
      const auto used = cpu() - c0;
      shadow.reset();
      std::cout << "JOURNAL_TAP_CATCHUP round=" << round << " tap=" << tapped
                << " products=" << products.size() << " records=" << count()
                << " cpuS=" << used << " wallS=" << wall << '\n';
    }
  for (auto &w : writers)
    w->close();
}
// With the journal feed the model never asks REST for product metadata, even
// when a feed connects before its journal header arrived.
TEST(JournalLiveFeedModel, NeverRequestsRestMetadata) {
  static int argc = 1;
  static char name[] = "test_shadow";
  static char *argv[] = {name, nullptr};
  if (!QCoreApplication::instance())
    static QCoreApplication app(argc, argv);
  QTemporaryDir temp;
  struct Cwd {
    QString old = QDir::currentPath();
    ~Cwd() { QDir::setCurrent(old); }
  } cwd;
  ASSERT_TRUE(QDir::setCurrent(temp.path()));
  for (const auto *feed : {"engine", "journal"}) {
    ServerConfig config;
    config.defaultSymbols = {"PEPE-USD"};
    config.rollerShadow.products = {"PEPE-USD"};
    config.heatmap.persistenceEnabled = false;
    config.recording.liveFeed = feed;
    ServerDataModel model(config);
    int requests = 0;
    QObject::connect(&model, &ServerDataModel::productMetadataRequested,
                     [&](const QString &, uint64_t) { ++requests; });
    model.onMarketDataConnectionChanged("PEPE-USD", true);
    EXPECT_EQ(requests, std::string(feed) == "engine" ? 1 : 0) << feed;
  }
}
// ---- Slice D-b1 through the production server graph: initialize() with
// recording.live_feed: journal, PEPE-USD captured, a real queued hand-off into
// ServerDataModel on this (main) thread.
bool eventuallyQt(auto f) {
  const auto until = std::chrono::steady_clock::now() + 8s;
  do {
    QCoreApplication::processEvents();
    if (f())
      return true;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < until);
  QCoreApplication::processEvents();
  return f();
}
std::string pepeLevel(const std::string &side, const std::string &price,
                      const std::string &size) {
  return json({{"channel", "l2_data"},
               {"events",
                json::array({{{"type", "update"},
                              {"product_id", "PEPE-USD"},
                              {"updates", json::array({{{"side", side},
                                                        {"price_level", price},
                                                        {"new_quantity",
                                                         size}}})}}})}})
      .dump();
}
struct JournalServer {
  ShadowTest &t;
  std::unique_ptr<capture::Writer> pepe;
  struct Cwd {
    QString old = QDir::currentPath();
    ~Cwd() { QDir::setCurrent(old); }
  } cwd;
  ServerConfig config;
  std::unique_ptr<SentinelServerApp> app;
  struct Book {
    std::string product, status;
    double tick;
    size_t bids, asks;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
  };
  std::vector<Book> books;
  int metadataRequests = 0, bookUpdates = 0, bars = 0;
  std::vector<Trade> trades;
  explicit JournalServer(ShadowTest &test) : t(test) {
    t.fanout.reset();
    t.writer.reset();
    t.fanoutMetrics = std::make_unique<metrics::MetricsRegistry>();
    capture::FanoutConfig fc;
    fc.socketPath = QString::fromStdString(t.cfg.socketPath);
    t.fanout = std::make_unique<capture::CaptureFanout>(
        fc, std::vector<std::string>{"PEPE-USD"}, *t.fanoutMetrics,
        [this](const auto &) { ++t.resnapshots; });
    capture::WriterConfig wc;
    wc.root = QString::fromStdString(t.cfg.journalRoot);
    wc.symbol = "PEPE-USD";
    wc.fsyncBlocks = 1;
    wc.onJournal = [this](const capture::JournalEvent &e) {
      if (t.fanout)
        t.fanout->publish(0, e);
    };
    pepe = std::make_unique<capture::Writer>(wc, pepeMeta());
    pepe->append(record(0, pepeSnapshot()));
    for (int s = 1; s <= 63; ++s)
      pepe->append(record(s * 1000));
    pepe->flush();
    EXPECT_TRUE(QDir::setCurrent(t.temp.path())); // relative tick logs
    config.defaultSymbols = {Product};
    config.streamPort = 0;
    config.recording.enabled = true;
    config.recording.source = "roller";
    config.recording.liveFeed = "journal";
    config.recording.dir = (t.root / "primary").string();
    config.recording.fallbackDir.clear();
    config.heatmap.persistenceEnabled = false;
    config.rollerShadow = t.cfg;
    config.rollerShadow.products = {"PEPE-USD"};
    config.rollerShadow.liveNowForTest = [&test] { return test.liveNow.load(); };
    QTcpServer probe; // a free loopback port for the metrics endpoint
    EXPECT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    const auto port = probe.serverPort();
    probe.close();
    qputenv("SENTINEL_HEALTH_PORT", QByteArray::number(port));
  }
  bool initialize() {
    app = std::make_unique<SentinelServerApp>(config);
    const bool ok = app->initialize();
    if (!ok)
      return false;
    auto &m = model();
    QObject::connect(&m, &ServerDataModel::bookSnapshotBroadcast,
                     [this](const QString &id,
                            const std::vector<OrderBookLevel> &bids,
                            const std::vector<OrderBookLevel> &asks, double tick,
                            const QString &status, uint64_t) {
                       books.push_back({id.toStdString(), status.toStdString(),
                                        tick, bids.size(), asks.size()});
                     });
    QObject::connect(&m, &ServerDataModel::bookUpdateBroadcast,
                     [this](const QString &, const std::vector<BookDelta> &,
                            double, double, uint64_t) { ++bookUpdates; });
    QObject::connect(&m, &ServerDataModel::productMetadataRequested,
                     [this](const QString &, uint64_t) { ++metadataRequests; });
    QObject::connect(&m, &ServerDataModel::barUpdated,
                     [this](const QString &, int64_t, const OHLCVBar &) {
                       ++bars;
                     });
    QObject::connect(&m, &ServerDataModel::tradeBroadcast,
                     [this](const Trade &trade) { trades.push_back(trade); });
    return true;
  }
  ~JournalServer() {
    app.reset();
    pepe->close();
    qunsetenv("SENTINEL_HEALTH_PORT");
  }
  ServerDataModel &model() { return ShadowServerTestAccess::model(*app); }
  std::string metrics() { return ShadowServerTestAccess::metrics(*app); }
  bool connected(bool up) {
    return metrics().find(std::string("sentinel_mdc_connected{product=\"PEPE-"
                                      "USD\",pinned=\"1\"} ") +
                          (up ? "1" : "0") + "\n") != std::string::npos;
  }
  // Index of the first PEPE book status `status` at or after `from`.
  size_t status(const std::string &status, size_t from = 0) {
    for (size_t i = from; i < books.size(); ++i)
      if (books[i].product == "PEPE-USD" && books[i].status == status)
        return i;
    return std::string::npos;
  }
};
// A5 and A3: no MarketDataFeeds; captured products only; PEPE's tick comes
// from the journal header (no REST) and a far move re-seeds instead of
// waiting for an upstream snapshot.
TEST_F(ShadowTest, JournalLiveFeedServerHasNoEngineAndServesCapturedProducts) {
  JournalServer s(*this);
  ASSERT_TRUE(s.initialize());
  EXPECT_FALSE(ShadowServerTestAccess::hasEngine(*s.app));
  auto &server = ShadowServerTestAccess::server(*s.app);
  using Admission = SentinelStreamServer::FeedAdmission;
  EXPECT_EQ(server.notifyClientSubscribed("PEPE-USD"), Admission::Accepted);
  EXPECT_EQ(server.notifyClientSubscribed("XRP-USD"), Admission::InvalidProduct);
  EXPECT_EQ(server.notifyClientSubscribed(Product), Admission::InvalidProduct);
  ASSERT_TRUE(eventuallyQt([&] {
    return s.status("ready") != std::string::npos && s.connected(true);
  }));
  const auto &ready = s.books[s.status("ready")];
  EXPECT_EQ(ready.tick,
            deriveGrid(pepeMeta()["product_metadata"], 0.00001).nearTick);
  EXPECT_LT(ready.tick, 0.005);
  EXPECT_EQ(ready.bids, 2u);
  EXPECT_EQ(ready.asks, 1u);
  EXPECT_EQ(s.metadataRequests, 0); // never REST
  EXPECT_EQ(s.metrics().find("sentinel_mdc_ws_latency_ms"), std::string::npos);
  // Provisional updates below $0.005 reach the live book.
  s.pepe->append(record(64000, pepeLevel("offer", "0.00001002", "123")));
  ASSERT_TRUE(eventuallyQt([&] { return s.bookUpdates > 0; }));
  // A 50 % move leaves the model's bounded raw band: re-seed, not "await".
  s.pepe->append(record(
      65000,
      json({{"channel", "l2_data"},
            {"events",
             json::array({{{"type", "update"},
                           {"product_id", "PEPE-USD"},
                           {"updates",
                            json::array({{{"side", "bid"},
                                          {"price_level", "0.00000999"},
                                          {"new_quantity", "0"}},
                                         {{"side", "bid"},
                                          {"price_level", "0.00000980"},
                                          {"new_quantity", "0"}},
                                         {{"side", "offer"},
                                          {"price_level", "0.00001001"},
                                          {"new_quantity", "0"}},
                                         {{"side", "offer"},
                                          {"price_level", "0.00001002"},
                                          {"new_quantity", "0"}},
                                         {{"side", "bid"},
                                          {"price_level", "0.00001500"},
                                          {"new_quantity", "10"}},
                                         {{"side", "offer"},
                                          {"price_level", "0.00001510"},
                                          {"new_quantity", "10"}}})}}})}})
          .dump()));
  ASSERT_TRUE(eventuallyQt([&] {
    const auto lost = s.status("aggregation_unavailable");
    return lost != std::string::npos &&
           s.status("ready", lost + 1) != std::string::npos;
  }));
  const auto &moved = s.books[s.status("ready", s.status("aggregation_unavailable") + 1)];
  EXPECT_EQ(moved.bids, 1u);
  EXPECT_EQ(moved.asks, 1u);
  EXPECT_EQ(resnapshots.load(), 0);
}
// A4: a journal market_trades BUY (Coinbase maker side) reaches the model as
// the aggressor Sell; the tape, footprint and candles follow it.
TEST_F(ShadowTest, JournalLiveFeedServerTradesCarryTheAggressorSide) {
  JournalServer s(*this);
  ASSERT_TRUE(s.initialize());
  ASSERT_TRUE(eventuallyQt([&] { return s.status("ready") != std::string::npos; }));
  s.pepe->append(record(65000, trades("BUY", "p1", "PEPE-USD", "0.00001")));
  ASSERT_TRUE(eventuallyQt([&] { return !s.trades.empty() && s.bars > 0; }));
  EXPECT_EQ(s.trades.front().trade_id, "p1");
  EXPECT_EQ(s.trades.front().side, AggressorSide::Sell);
  std::vector<ServerDataModel::FootprintTradeSample> footprint;
  ASSERT_TRUE(s.model().collectFootprintTrades("PEPE-USD", Epoch + 64000,
                                               Epoch + 66000, footprint));
  ASSERT_EQ(footprint.size(), 1u);
  EXPECT_EQ(footprint.front().side, AggressorSide::Sell);
}
// A6: TransportDown invalidates; 30 s without a socket record reports the
// product disconnected (and invalid); TransportUp plus a snapshot is ready.
TEST_F(ShadowTest, JournalLiveFeedServerConnectionFollowsTransportAndTipAge) {
  JournalServer s(*this);
  ASSERT_TRUE(s.initialize());
  ASSERT_TRUE(eventuallyQt([&] {
    return s.status("ready") != std::string::npos && s.connected(true);
  }));
  s.pepe->append(lifecycle(64000, capture::Kind::TransportDown));
  ASSERT_TRUE(eventuallyQt([&] {
    return s.status("invalidated") != std::string::npos && s.connected(false);
  }));
  const auto down = s.books.size();
  s.pepe->append(lifecycle(65000, capture::Kind::TransportUp));
  ASSERT_TRUE(eventuallyQt([&] { return s.connected(true); }));
  EXPECT_EQ(s.status("ready", down), std::string::npos); // no book yet
  s.pepe->append(record(66000, pepeSnapshot()));
  ASSERT_TRUE(eventuallyQt([&] { return s.status("ready", down) != std::string::npos; }));
  // Tip-age guard: a frozen capture (FM-127) shows as invalid and down.
  const auto quiet = s.books.size();
  liveNow = Epoch + 66000 + 30000;
  ASSERT_TRUE(eventuallyQt([&] {
    return s.connected(false) && s.status("invalidated", quiet) != std::string::npos;
  }));
  // Records again: re-seed from durable plus the provisional suffix.
  const auto silent = s.books.size();
  s.pepe->append(record(67000));
  ASSERT_TRUE(eventuallyQt([&] {
    return s.connected(true) && s.status("ready", silent) != std::string::npos;
  }));
  EXPECT_EQ(s.books.back().status, "ready");
}
// A5: the journal feed needs the roller-served recording; anything else is
// refused at startup with the reason logged.
TEST_F(ShadowTest, JournalLiveFeedRefusedWithoutTheRollerSource) {
  JournalServer s(*this);
  s.config.recording.source = "primary";
  EXPECT_FALSE(s.initialize());
  EXPECT_NE(liveFeedProblem(s.config).find("requires recording.source=roller"),
            std::string::npos);
  s.config.recording.source = "roller";
  s.config.recording.liveFeed = "websocket";
  EXPECT_NE(liveFeedProblem(s.config), "");
  EXPECT_FALSE(s.initialize());
  s.config.recording.liveFeed = "journal";
  s.config.rollerShadow.enabled = false;
  EXPECT_NE(liveFeedProblem(s.config), "");
  EXPECT_FALSE(s.initialize());
  s.app.reset();
}
TEST(ShadowConfig, LiveFeedKeyDefaultsToEngine) {
  QTemporaryDir tmp;
  const auto p = fs::path(tmp.path().toStdString()) / "config.yaml";
  {
    std::ofstream out(p);
    out << "recording:\n  source: roller\n  live_feed: journal\n"
           "roller_shadow:\n  enabled: true\n";
  }
  ServerConfig c;
  EXPECT_EQ(c.recording.liveFeed, "engine");
  EXPECT_EQ(liveFeedProblem(c), "");
  EXPECT_FALSE(journalLiveFeed(c));
  ASSERT_TRUE(ConfigLoader::loadServerConfig(p.string(), &c));
  EXPECT_EQ(c.recording.liveFeed, "journal");
  EXPECT_TRUE(journalLiveFeed(c));
  EXPECT_EQ(liveFeedProblem(c), "");
}

// Fix round 1 (finding 1): two invalidation/recovery cycles inside the 1 s
// re-seed throttle. The second request is coalesced and delivered at the
// deadline, so ordinary deltas find a ready book again (no upstream snapshot).
TEST_F(ShadowTest, JournalLiveFeedServerCoalescesReseedsInsideTheThrottle) {
  JournalServer s(*this);
  ASSERT_TRUE(s.initialize());
  ASSERT_TRUE(eventuallyQt([&] { return s.status("ready") != std::string::npos; }));
  const auto move = [&](int64_t ms, std::vector<std::array<std::string, 3>> levels) {
    json updates = json::array();
    for (const auto &[side, price, size] : levels)
      updates.push_back({{"side", side}, {"price_level", price}, {"new_quantity", size}});
    s.pepe->append(record(ms, json({{"channel", "l2_data"},
                                    {"events", json::array({{{"type", "update"},
                                                             {"product_id", "PEPE-USD"},
                                                             {"updates", updates}}})}})
                                  .dump()));
  };
  move(64000, {{"bid", "0.00000999", "0"}, {"bid", "0.00000980", "0"},
               {"offer", "0.00001001", "0"}, {"bid", "0.00001500", "10"},
               {"offer", "0.00001510", "10"}});
  ASSERT_TRUE(eventuallyQt([&] {
    const auto lost = s.status("aggregation_unavailable");
    return lost != std::string::npos && s.status("ready", lost + 1) != std::string::npos;
  }));
  const auto first = s.status("aggregation_unavailable");
  const auto recovered = s.status("ready", first + 1);
  move(65000, {{"bid", "0.00001500", "0"}, {"offer", "0.00001510", "0"},
               {"bid", "0.00002500", "10"}, {"offer", "0.00002510", "10"}});
  ASSERT_TRUE(eventuallyQt([&] {
    return s.status("aggregation_unavailable", recovered + 1) != std::string::npos;
  }));
  const auto second = s.status("aggregation_unavailable", recovered + 1);
  // Inside the throttle window of the first request.
  EXPECT_LT(s.books[second].at - s.books[first].at, 900ms);
  ASSERT_TRUE(eventuallyQt([&] { return s.status("ready", second + 1) != std::string::npos; }));
  const auto again = s.status("ready", second + 1);
  EXPECT_EQ(s.books[again].bids, 1u);
  EXPECT_EQ(s.books[again].asks, 1u);
  const auto updates = s.bookUpdates;
  s.pepe->append(record(66000, pepeLevel("offer", "0.00002520", "5")));
  ASSERT_TRUE(eventuallyQt([&] { return s.bookUpdates > updates; }));
  EXPECT_EQ(resnapshots.load(), 0);
}
// Fix round 1 (finding 3): with the journal feed every captured product is
// exempt from mdc.max_connections and anything else is an invalid product,
// never a connection-cap refusal.
TEST_F(ShadowTest, JournalLiveFeedAdmissionIgnoresTheEngineCap) {
  JournalServer s(*this);
  s.config.mdc.maxConnections = 1;
  s.config.rollerShadow.products = {"PEPE-USD", "ETH-USD", "SOL-USD"};
  ASSERT_TRUE(s.initialize());
  auto &server = ShadowServerTestAccess::server(*s.app);
  using Admission = SentinelStreamServer::FeedAdmission;
  EXPECT_EQ(server.notifyClientSubscribed("ETH-USD"), Admission::Accepted);
  EXPECT_EQ(server.notifyClientSubscribed("SOL-USD"), Admission::Accepted);
  EXPECT_EQ(server.notifyClientSubscribed("PEPE-USD"), Admission::Accepted);
  EXPECT_EQ(server.notifyClientSubscribed("XRP-USD"), Admission::InvalidProduct);
  EXPECT_EQ(server.notifyClientSubscribed(Product), Admission::InvalidProduct);
  EXPECT_NE(s.metrics().find("sentinel_mdc_refused_total{product=\"XRP-USD\"} 1"),
            std::string::npos);
}
// Fix round 2 (finding 2): candles of a journal product close on its journal
// time only. Two production models run the 250 ms wall-clock candle timer; one
// gets every record on time, the other loses the fan-out at +50 s without
// noticing (capture journals trades at +55 s and +65 s; the wall clock passes
// +60 s) and recovers the journal gap at +80 s. Closed 1 s / 1 m / 5 m bars are
// unique, ordered and identical.
struct JournalCandles {
  ServerConfig config;
  QTemporaryDir temp;
  struct Cwd {
    QString old = QDir::currentPath();
    ~Cwd() { QDir::setCurrent(old); }
  } cwd;
  std::unique_ptr<ServerDataModel> steady, outage;
  static constexpr int64_t T0 = 1'800'000'000'000 / 300'000 * 300'000;
  struct Rec { int64_t receiveMs; std::optional<Trade> trade; };
  std::vector<Rec> journal;
  JournalCandles() {
    static int argc = 1;
    static char name[] = "test_shadow";
    static char *argv[] = {name, nullptr};
    if (!QCoreApplication::instance())
      static QCoreApplication app(argc, argv);
    EXPECT_TRUE(QDir::setCurrent(temp.path()));
    config.defaultSymbols = {"PEPE-USD"};
    config.rollerShadow.products = {"PEPE-USD"};
    config.heatmap.persistenceEnabled = false;
    config.heatmap.timeframesMs = {1000, 60000, 300000};
    config.recording.liveFeed = "journal";
    steady = std::make_unique<ServerDataModel>(config);
    outage = std::make_unique<ServerDataModel>(config);
  }
  // Capture journals a heartbeat every second and each trade 80 ms after its
  // exchange time; the tap hands over the watermark once per journal second.
  void record(std::vector<std::pair<int64_t, double>> trades, int64_t until) {
    for (int64_t s = 0; s <= until; s += 1000)
      journal.push_back({T0 + s + 120, std::nullopt});
    for (const auto &[ms, price] : trades)
      journal.push_back({T0 + ms + 80, Trade{std::chrono::system_clock::time_point(
                                                 std::chrono::milliseconds(T0 + ms)),
                                             "PEPE-USD", std::to_string(ms),
                                             AggressorSide::Buy, price, 1.0}});
    std::stable_sort(journal.begin(), journal.end(),
                     [](const Rec &a, const Rec &b) { return a.receiveMs < b.receiveMs; });
  }
  static void deliver(ServerDataModel &m, const Rec &r, int64_t &sentSecond) {
    if (r.trade)
      m.onTrade(*r.trade);
    if (r.receiveMs / 1000 > sentSecond) {
      sentSecond = r.receiveMs / 1000;
      m.onFeedWatermark("PEPE-USD", r.receiveMs);
    }
  }
  // Outage: records received in (stallFrom, recoverAt) reach `outage` only at
  // recoverAt (detection plus journal catch-up), in order.
  void run(int64_t stallFrom, int64_t recoverAt, int64_t end) {
    for (auto *m : {steady.get(), outage.get()})
      ShadowServerTestAccess::stopCandleTimer(*m);
    size_t a = 0, b = 0;
    int64_t sentA = INT64_MIN, sentB = INT64_MIN;
    for (int64_t now = T0; now <= T0 + end; now += 250) {
      for (; a < journal.size() && journal[a].receiveMs <= now; ++a)
        deliver(*steady, journal[a], sentA);
      const bool stalled = now > T0 + stallFrom && now < T0 + recoverAt;
      for (; !stalled && b < journal.size() && journal[b].receiveMs <= now; ++b)
        deliver(*outage, journal[b], sentB);
      ShadowServerTestAccess::tickCandles(*steady, now); // the wall-clock timer
      ShadowServerTestAccess::tickCandles(*outage, now);
    }
  }
  void expectIdentical() {
    for (const int64_t tf : {1000, 60000, 300000}) {
      const auto x = steady->getHistory("PEPE-USD", tf, 100000);
      const auto y = outage->getHistory("PEPE-USD", tf, 100000);
      ASSERT_FALSE(x.empty()) << tf;
      for (size_t i = 1; i < y.size(); ++i)
        EXPECT_LT(y[i - 1].timestamp_ms, y[i].timestamp_ms) << tf << " at " << i;
      ASSERT_EQ(x.size(), y.size()) << tf;
      for (size_t i = 0; i < x.size(); ++i)
        EXPECT_EQ(std::tie(x[i].timestamp_ms, x[i].open, x[i].high, x[i].low, x[i].close,
                           x[i].volume, x[i].count),
                  std::tie(y[i].timestamp_ms, y[i].open, y[i].high, y[i].low, y[i].close,
                           y[i].volume, y[i].count))
            << tf << " at " << i;
    }
  }
};
TEST(JournalLiveFeedModel, DelayedOutageDetectionKeepsCandlesIdentical) {
  JournalCandles c;
  c.record({{10'000, 1.0}, {40'000, 2.0}, {55'000, 3.0}, {65'000, 2.5},
            {150'000, 4.0}, {200'000, 5.0}, {301'000, 6.0}},
           420'000);
  c.run(50'000, 80'000, 420'000);
  c.expectIdentical();
  // The minute the stall straddled holds both of its trades, once.
  const auto minutes = c.outage->getHistory("PEPE-USD", 60000, 100);
  ASSERT_GE(minutes.size(), 2u);
  EXPECT_EQ(minutes[0].timestamp_ms, JournalCandles::T0);
  EXPECT_EQ(minutes[0].count, 3u); // 10 s, 40 s, 55 s
  EXPECT_EQ(minutes[1].count, 1u); // 65 s
}
// Heartbeats alone advance journal time: a product with no trades still
// closes its bars on time (one watermark lag), not on this process's clock.
TEST(JournalLiveFeedModel, HeartbeatsCloseBarsWithoutTrades) {
  JournalCandles c;
  c.record({{10'000, 1.0}}, 130'000);
  for (auto *m : {c.steady.get(), c.outage.get()})
    ShadowServerTestAccess::stopCandleTimer(*m);
  int64_t sent = INT64_MIN;
  for (const auto &r : c.journal)
    JournalCandles::deliver(*c.steady, r, sent);
  // The wall clock is far ahead and closes nothing for a journal product.
  ShadowServerTestAccess::tickCandles(*c.steady, JournalCandles::T0 + 3'600'000);
  const auto minutes = c.steady->getHistory("PEPE-USD", 60000, 100);
  ASSERT_EQ(minutes.size(), 2u); // 0..1 m closed at 129.12 s; 2 m is open
  EXPECT_EQ(minutes[0].count, 1u);
  EXPECT_EQ(minutes[1].count, 0u); // carried quiet minute
  EXPECT_EQ(minutes[1].close, 1.0);
  const auto seconds = c.steady->getHistory("PEPE-USD", 1000, 1000);
  ASSERT_FALSE(seconds.empty());
  EXPECT_EQ(seconds.front().timestamp_ms, JournalCandles::T0 + 10'000);
  // Last journal record 130.12 s, lag 1 s: seconds through 128 are closed.
  EXPECT_EQ(seconds.back().timestamp_ms, JournalCandles::T0 + 128'000);
  // A late trade for a bucket journal time already closed never reopens it.
  c.steady->onTrade(Trade{std::chrono::system_clock::time_point(
                              std::chrono::milliseconds(JournalCandles::T0 + 50'000)),
                          "PEPE-USD", "late", AggressorSide::Buy, 9.0, 1.0});
  c.steady->onFeedWatermark("PEPE-USD", JournalCandles::T0 + 250'000);
  const auto after = c.steady->getHistory("PEPE-USD", 60000, 100);
  ASSERT_EQ(after.size(), 4u);
  for (size_t i = 0; i < after.size(); ++i) {
    EXPECT_EQ(after[i].timestamp_ms, JournalCandles::T0 + int64_t(i) * 60'000) << i;
    EXPECT_NE(after[i].high, 9.0) << i;
  }
}
} // namespace
