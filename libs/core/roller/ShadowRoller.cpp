#include "ShadowRoller.hpp"
#include "SentinelLogging.hpp"
#include "capture/CaptureFanout.hpp"
#include <QDir>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <thread>

namespace sentinel::roller {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr int64_t Hour = 3600000, Day = 86400000;
struct ShadowStopped {};
int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
JournalPos position(const json &j, const std::string &product) {
  const auto p = capture::parsePosition(j);
  if (p.product != product)
    throw std::runtime_error("fanout product mismatch");
  return {p.product, p.runId, p.block, p.record};
}
bool beforeOrEqual(const JournalPos &a, const JournalPos &b) {
  return a.run == b.run &&
         std::pair(a.block, a.record) <= std::pair(b.block, b.record);
}
uint64_t little(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i)
    v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}
// Per product, across lead forks: provisional bucket starts already handed to
// the sink. A rebuilt lead never republishes an older minute than one already
// shown (a discarded lead may have reached the next minute on withdrawn data).
struct LeadGuard {
  std::map<std::string, int64_t>
      newestBucket; // by layer; lead workers only, one at a time
};
// Option C live path (owner 2026-10-06), roller worker thread only. The lead is
// a non-persisting fork of the day's history recorder (Publication::Lead) that
// also applies provisional fan-out records as they arrive. History still
// applies only durable prefixes (INV-114) and publishes committed minutes. Any
// provisional discard (retract, disconnect, EOF, socket failure) drops the
// lead; the next fork starts again from the durable history state.
class LiveLead {
  const ShadowConfig &cfg;
  const std::string product;
  std::shared_ptr<LeadGuard> guard;
  metrics::Counter &forks;
  recording::BookRecorder *history = nullptr;
  const JournalFeed *historyFeed = nullptr;
  std::unique_ptr<JournalFeed> feed;
  std::unique_ptr<recording::BookRecorder> lead;
  int64_t lastTickMs = 0, nextFailureLogMs = 0;
  int64_t now() const {
    return cfg.liveNowForTest ? cfg.liveNowForTest() : nowMs();
  }
  void failed(const char *where, const std::exception &e) {
    discard();
    const auto t = nowMs();
    if (t >= nextFailureLogMs) {
      nextFailureLogMs = t + 10000;
      sLog_Warning("Roller live lead dropped product="
                   << product << " at=" << where << " error=" << e.what());
    }
  }
  void tick() {
    const auto t = now();
    if (t - lastTickMs < 250)
      return;
    lastTickMs = t;
    lead->onTick(t);
  }

public:
  LiveLead(const ShadowConfig &c, std::string p, std::shared_ptr<LeadGuard> g,
           metrics::Counter &f)
      : cfg(c), product(std::move(p)), guard(std::move(g)), forks(f) {}
  ~LiveLead() { discard(); }
  bool enabled() const { return bool(cfg.publisher); }
  void attach(recording::BookRecorder *h, const JournalFeed *f) {
    history = h;
    historyFeed = f;
    if (!h)
      discard();
  }
  // Joins the lead worker: no publication of withdrawn input after return.
  void discard() {
    lead.reset();
    feed.reset();
  }
  // At the socket, after every returned record reached history: fork from the
  // durable state, then apply the provisional suffix received so far.
  void ensure(const std::deque<JournalRecord> &pending) {
    if (!enabled() || lead || !history || !historyFeed)
      return;
    try {
      auto publish = [sink = cfg.publisher, guard = guard, name = product](
                         std::shared_ptr<const recording::Hmc2Record> r) {
        auto &newest = guard->newestBucket[r->header.layer];
        if (r->bucketStartMs < newest)
          return;
        newest = r->bucketStartMs;
        sLog_Probe("roller.live",
                   "product=" << name << " layer=" << r->header.layer
                              << " bucket=" << r->bucketStartMs
                              << " observed=" << r->observedMs
                              << " entries=" << r->entries.size() << " ageMs="
                              << nowMs() - (r->bucketStartMs + r->observedMs));
        sink(std::move(r));
      };
      lead = history->forkLead(std::move(publish), cfg.livePublishMs);
      feed = std::make_unique<JournalFeed>(*historyFeed);
      feed->onSnapshot = [this](int64_t e, int64_t l,
                                std::vector<recording::Level> v) {
        lead->onSnapshotAt(product, e, l, std::move(v));
      };
      feed->onUpdates = [this](int64_t e, int64_t l,
                               std::vector<recording::Level> v) {
        lead->onUpdatesAt(product, e, l, std::move(v));
      };
      feed->onInvalid = [this](int64_t t, const std::string &reason) {
        lead->onInvalid(product, t, reason);
      };
      feed->onTick = [this](int64_t t) { lead->onTick(t); };
      feed->onTrade = {};
      feed->onConnection = {};
      forks.inc();
      sLog_Data("Roller live lead forked product=" << product << " provisional="
                                                   << pending.size());
      for (const auto &r : pending)
        feed->apply(r);
      lastTickMs = 0;
      tick();
    } catch (const std::exception &e) {
      failed("fork", e);
    }
  }
  void apply(const JournalRecord &r) {
    if (!lead)
      return;
    try {
      feed->apply(r);
      tick();
    } catch (const std::exception &e) {
      failed("apply", e);
    }
  }
  // Socket waits: wall-clock ticks advance the forming minute like the primary
  // recorder's 250 ms timer.
  void idle() {
    if (!lead)
      return;
    try {
      tick();
    } catch (const std::exception &e) {
      failed("tick", e);
    }
  }
};
// A bounded synchronous socket, owned by this product's worker only. Timeouts
// periodically check cancellation; no Qt event loop or main-thread invocation.
class LiveSource {
  const ShadowConfig &cfg;
  const std::string product;
  const std::atomic<bool> &stopping;
  QLocalSocket socket;
  QByteArray bytes;
  std::deque<JournalRecord> pending;
  size_t pendingSize = 0;
  std::optional<JournalPos> ceiling, diskTarget, applied, received;
  bool diskDone = false, initialDetached = true, recovering = false;
  std::function<void(const std::string &)> retry;
  LiveLead *lead;
  std::unique_ptr<JournalReader> catchup;
  std::map<std::string, json> metadata;
  std::map<std::string, size_t> runOrder;
  void inventory() {
    JournalReader r(cfg.journalRoot, product);
    runOrder.clear();
    for (const auto &f : r.files()) {
      const std::string run = f.header.at("run_id");
      if (!runOrder.contains(run))
        runOrder[run] = runOrder.size();
      metadata[run] = f.header.at("product_metadata");
    }
  }
  bool older(const JournalPos &a, const JournalPos &b) {
    if (a.run == b.run)
      return beforeOrEqual(a, b);
    if (!runOrder.contains(a.run) || !runOrder.contains(b.run))
      inventory();
    return runOrder.at(a.run) < runOrder.at(b.run);
  }
  std::pair<json, std::string> packet() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!stopping) {
      // Bound both Qt's buffer and ours, even for a peer that never frames.
      if (bytes.size() >= 4) {
        const auto n = little(bytes.data(), 4);
        if (n < 4 || n > capture::MaxRecordBytes + 4096)
          throw std::runtime_error("fanout packet length");
        if (uint64_t(bytes.size()) >= n + 4) {
          const auto jn = little(bytes.data() + 4, 4);
          if (jn > n - 4 || jn > 4096)
            throw std::runtime_error("fanout JSON length");
          auto j = json::parse(bytes.data() + 8, bytes.data() + 8 + jn);
          std::string raw(bytes.data() + 8 + jn, n - 4 - jn);
          bytes.remove(0, n + 4);
          return {std::move(j), std::move(raw)};
        }
      }
      if (socket.state() == QLocalSocket::UnconnectedState)
        throw std::runtime_error("fanout EOF; resume journal");
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("fanout idle timeout");
      if (lead)
        lead->idle();
      if (socket.bytesAvailable() == 0)
        socket.waitForReadyRead(50);
      bytes += socket.read(capture::MaxRecordBytes + 4100 - bytes.size());
    }
    throw ShadowStopped{};
  }
  void controlOrRecord() {
    auto [j, raw] = packet();
    const auto type = j.at("type").get<std::string>();
    if (type == "record") {
      const auto p = position(j.at("pos"), product);
      if (!j.at("provisional").get<bool>() || raw.size() < 32 ||
          raw.size() > capture::MaxRecordBytes + 4 ||
          little(raw.data(), 4) != raw.size() - 4)
        throw std::runtime_error("malformed fanout RAWL2 record");
      const auto kind = little(raw.data() + 4, 4);
      if (kind < 1 || kind > 9)
        throw std::runtime_error("fanout record kind");
      JournalRecord r{{capture::Kind(kind),
                       {int64_t(little(raw.data() + 8, 8)),
                        int64_t(little(raw.data() + 16, 8))},
                       little(raw.data() + 24, 8),
                       raw.substr(32)},
                      p,
                      {},
                      false,
                      1};
      const auto local = r.record.time.systemNs / 1000000;
      if (local < recording::kHmc2MinMs || local >= recording::kHmc2EndMs ||
          r.record.time.steadyNs < 0)
        throw std::runtime_error("fanout receive clock out of range");
      if (cfg.observeForTest)
        cfg.observeForTest(r, false);
      // Handshake replay overlaps the file prefix. Skip only a proven
      // applied prefix, never positions newer than the durable ceiling.
      if (applied && older(p, *applied))
        return;
      if (received &&
          (p.run != received->run ||
           !((p.block == received->block && p.record == received->record + 1) ||
             (p.block == received->block + 1 && p.record == 0))))
        throw std::runtime_error("fanout discontinuity; resume journal");
      received = p;
      pendingSize += raw.size();
      if (pendingSize > cfg.pendingBytes)
        throw std::runtime_error("shadow pending queue full");
      pending.push_back(std::move(r));
      if (lead)
        lead->apply(pending.back());
    } else {
      if (!raw.empty())
        throw std::runtime_error("fanout control with raw suffix");
      if (type == "durable") {
        auto p = position(j.at("through"), product);
        if (ceiling && p.run == ceiling->run && !beforeOrEqual(*ceiling, p))
          throw std::runtime_error("durability regression");
        ceiling = p;
      } else if (type == "retract" || type == "disconnect") {
        // Recovery discards provisional state and retains the fully applied
        // durable prefix in the existing JournalFeed/BookRecorder.
        throw std::runtime_error("fanout " + type + "; resume journal");
      } else
        throw std::runtime_error("unexpected fanout control: " + type);
    }
  }
  void discardProvisional() {
    pending.clear();
    pendingSize = 0;
    if (lead)
      lead->discard();
  }
  void handshake(const std::optional<JournalPos> &checkpoint) {
    socket.abort();
    bytes.clear();
    discardProvisional();
    pendingSize = 0;
    ceiling.reset();
    received = applied;
    socket.setReadBufferSize(capture::MaxRecordBytes + 4100);
    const auto path =
        cfg.socketPath.empty()
            ? QDir::homePath() + "/Sentinel-runtime/run/capture.sock"
            : QString::fromStdString(cfg.socketPath);
    socket.connectToServer(path);
    if (!socket.waitForConnected(1000))
      throw std::runtime_error("capture socket unavailable: " +
                               socket.errorString().toStdString());
    json hello = {{"type", "hello"}, {"version", 1}, {"product", product}};
    if (checkpoint)
      hello["pos"] = *checkpoint;
    const auto command = hello.dump() + "\n";
    if (socket.write(command.data(), command.size()) !=
            qint64(command.size()) ||
        !socket.waitForBytesWritten(1000))
      throw std::runtime_error("fanout handshake write failed");
    auto [tip, raw] = packet();
    if (tip.at("type") == "gap")
      std::tie(tip, raw) = packet();
    if (!raw.empty() || tip.at("type") != "tip" || tip.at("version") != 1 ||
        tip.at("product") != product)
      throw std::runtime_error("invalid fanout handshake");
    if (!tip.at("durable").is_null())
      ceiling = position(tip.at("durable"), product);
    // With no durability yet, wait for the first watermark. Never infer
    // catch-up completeness from a provisional tip or a temporary file EOF.
    while (!ceiling)
      controlOrRecord();
    diskTarget = ceiling;
    inventory();
  }

public:
  LiveSource(const ShadowConfig &c, std::string p,
             const std::atomic<bool> &stop,
             const std::optional<JournalPos> &checkpoint,
             std::function<void(const std::string &)> onRetry,
             LiveLead *liveLead = nullptr)
      : cfg(c), product(std::move(p)), stopping(stop),
        retry(std::move(onRetry)), lead(liveLead) {
    handshake(checkpoint);
    // A day-anchor rebuild can exceed the bounded server backlog. Hold no
    // socket during that replay; reconnect at the fully applied durable tip.
    socket.abort();
    bytes.clear();
    discardProvisional();
    received.reset();
  }
  bool next(JournalReader &disk, JournalRecord &out) {
    while (!stopping) {
      try {
        if (recovering) {
          handshake(applied);
          if (!older(*applied, *ceiling))
            throw std::runtime_error("durable tip precedes applied cursor");
          catchup.reset();
          diskDone = *ceiling == *applied;
          if (!diskDone) {
            catchup = std::make_unique<JournalReader>(cfg.journalRoot, product,
                                                      applied);
            JournalRecord cursor;
            if (!catchup->next(cursor) || cursor.pos != *applied)
              throw std::runtime_error("resume cursor unavailable in journal");
          }
          initialDetached = false;
          recovering = false;
        }
        if (!nextImpl(disk, out))
          return false;
        // next() is called again only after JournalFeed applied this record.
        // Retaining that state allows transport recovery without anchor replay.
        applied = out.pos;
        return true;
      } catch (const ShadowStopped &) {
        return false;
      } catch (const std::exception &e) {
        if (!applied)
          throw;
        socket.abort();
        // Retract/disconnect/EOF: the lead held withdrawn input. History keeps
        // its applied prefix; the next fork republishes from durable state.
        discardProvisional();
        recovering = true;
        retry(e.what());
      }
    }
    return false;
  }

private:
  bool nextImpl(JournalReader &disk, JournalRecord &out) {
    if (stopping)
      return false;
    if (diskDone && initialDetached) {
      initialDetached = false;
      handshake(applied);
      if (*ceiling != *applied) {
        catchup =
            std::make_unique<JournalReader>(cfg.journalRoot, product, applied);
        JournalRecord cursor;
        if (!catchup->next(cursor) || cursor.pos != *applied)
          throw std::runtime_error("resume cursor unavailable in journal");
        diskDone = false;
      }
    }
    if (!diskDone) {
      if (!(catchup ? catchup->next(out) : disk.next(out)))
        throw std::runtime_error(
            "durable handshake prefix not visible in journal");
      if (!older(out.pos, *diskTarget))
        throw std::runtime_error(
            "journal replay crossed handshake durable ceiling");
      if (out.pos == *diskTarget) {
        diskDone = true;
        applied = out.pos;
        received = pending.empty() ? out.pos : pending.back().pos;
      }
      return true;
    }
    while (!stopping) {
      while (!pending.empty() && applied &&
             older(pending.front().pos, *applied)) {
        pendingSize -= pending.front().record.payload.size() + 32;
        pending.pop_front();
      }
      if (!pending.empty() && ceiling && older(pending.front().pos, *ceiling)) {
        out = std::move(pending.front());
        pending.pop_front();
        pendingSize -= out.record.payload.size() + 32;
        if (!metadata.contains(out.pos.run))
          inventory();
        out.metadata = metadata.at(out.pos.run);
        applied = out.pos;
        return true;
      }
      if (lead)
        lead->ensure(pending);
      controlOrRecord();
    }
    return false;
  }
};
// Unknown when the journal root itself is unavailable (volume not mounted):
// that is a retryable fault, not evidence that the product is not captured.
std::optional<bool> journalHasProduct(const std::string &root,
                                      const std::string &product) {
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    return std::nullopt;
  const auto dir = fs::path(root) / product;
  if (!fs::is_directory(dir, ec))
    return false;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file() || it->path().extension() != ".rawl2")
      continue;
    try {
      const auto h =
          capture::readHeader(QString::fromStdString(it->path().string()));
      if (h.at("product_metadata").at("product_id") == product)
        return true;
    } catch (const std::exception &) {
    }
  }
  return false;
}
bool overlaps(const fs::path &a, const fs::path &b) {
  auto x = fs::weakly_canonical(a), y = fs::weakly_canonical(b);
  auto prefix = [](const auto &p, const auto &q) {
    return std::mismatch(p.begin(), p.end(), q.begin(), q.end()).first ==
           p.end();
  };
  return prefix(x, y) || prefix(y, x);
}
} // namespace

struct ShadowRoller::Impl {
  struct Product {
    std::string name;
    metrics::Gauge *running, *lastComparison, *cooldown;
    metrics::Counter *records, *failures, *compareFailures, *cooldowns,
        *leadForks;
    bool refused = false;
    std::shared_ptr<LeadGuard> leadGuard = std::make_shared<LeadGuard>();
    // Served watermarks (chunk workers): the current day's history recorder
    // while it exists, merged into the newest values ever served.
    mutable std::mutex historyMutex;
    recording::BookRecorder *history = nullptr;
    int64_t historyEndMs = 0;
    mutable std::map<std::string, recording::BookRecorder::Watermarks> served;
    struct ComparisonMetrics {
      std::atomic<bool> ready{false};
      std::atomic<uint64_t> near{0}, deep{0};
    };
    std::shared_ptr<ComparisonMetrics> comparison =
        std::make_shared<ComparisonMetrics>();
    std::shared_ptr<std::atomic<int64_t>> last =
        std::make_shared<std::atomic<int64_t>>(0);
    std::atomic<int64_t> committed{0};
    int64_t compared = 0;
    bool comparisonCheckpointSeen = false; // checker thread only
    std::thread worker;
  };
  ShadowConfig cfg;
  fs::path primary;
  std::vector<std::unique_ptr<Product>> products;
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  std::condition_variable wake;
  std::thread checker;
  Impl(ShadowConfig c, std::vector<std::string> names, fs::path root,
       metrics::MetricsRegistry &registry)
      : cfg(std::move(c)), primary(std::move(root)) {
    for (auto &name : names) {
      auto p = std::make_unique<Product>();
      p->name = std::move(name);
      const metrics::Labels labels = {{"product", p->name}};
      p->running = &registry.gauge(
          "sentinel_roller_shadow_running",
          "Shadow product is consuming the durable journal/socket.", labels);
      p->lastComparison = &registry.gauge(
          "sentinel_roller_shadow_last_comparison_timestamp_seconds",
          "Completion time of the latest successful hourly parity report.",
          labels);
      p->records = &registry.counter(
          "sentinel_roller_shadow_records_applied_total",
          "Durable records applied, including deterministic restart warmup.",
          labels);
      p->failures = &registry.counter(
          "sentinel_roller_shadow_setup_failures_total",
          "Shadow setup/stream/write failures requiring retry.", labels);
      p->cooldown = &registry.gauge(
          "sentinel_roller_shadow_fault_cooldown",
          "Product is probing a persistent fault at slow cadence.", labels);
      p->cooldowns =
          &registry.counter("sentinel_roller_shadow_fault_cooldowns_total",
                            "Entries into persistent-fault cooldown.", labels);
      p->compareFailures = &registry.counter(
          "sentinel_roller_shadow_comparison_failures_total",
          "Hourly parity checks that could not complete.", labels);
      p->leadForks = &registry.counter(
          "sentinel_roller_shadow_live_lead_forks_total",
          "Live forming-minute leads forked from durable history state "
          "(serving path; one per start, day and provisional discard).",
          labels);
      for (const auto *layer : {"near", "deep"}) {
        registry.counterFn(
            "sentinel_roller_shadow_mismatch_total",
            "Strict same-journal mismatching buckets, persisted across "
            "restarts.",
            {{"product", p->name}, {"layer", layer}},
            [state = p->comparison, near = std::string_view(layer) ==
                                           "near"]() -> std::optional<double> {
              // Do not expose zero during restore and manufacture an increase.
              if (!state->ready.load())
                return std::nullopt;
              return near ? state->near.load() : state->deep.load();
            });
      }
      registry.gaugeFn(
          "sentinel_roller_shadow_lag_seconds",
          "Age of last applied durable record; -1 before first record.", labels,
          [last = p->last]() -> std::optional<double> {
            const auto t = last->load();
            return t ? std::max(0., (nowMs() - t) / 1000.) : -1.;
          });
      // Owner decision 2: a product the capture does not journal is refused.
      if (journalHasProduct(cfg.journalRoot, p->name) == false) {
        p->refused = true;
        p->failures->inc();
        sLog_Error("Roller refused product="
                   << p->name << " reason=no journal with product_id under "
                   << cfg.journalRoot);
      }
      products.push_back(std::move(p));
    }
    // Allocate/register first. A partial thread launch is joined safely.
    try {
      for (auto &p : products)
        if (!p->refused)
          p->worker = std::thread([this, p = p.get()] { run(*p); });
      checker = std::thread([this] { check(); });
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Impl() { stop(); }
  void stop() {
    {
      // The predicate and wait transition share this mutex: never lose a stop
      // between a false predicate and the condition variable releasing the
      // lock.
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    for (auto &p : products)
      if (p->worker.joinable())
        p->worker.join();
    if (checker.joinable())
      checker.join();
  }
  void wait(std::chrono::milliseconds delay, bool comparison = false) {
    std::unique_lock lock(mutex);
    wake.wait_for(lock, delay, [&] {
      const bool stopped = stopping.load();
      if (!stopped && comparison && cfg.beforeCompareWaitForTest)
        cfg.beforeCompareWaitForTest();
      return stopped;
    });
  }
  void validate() {
    if (cfg.outputRoot.empty() || primary.empty() || cfg.journalRoot.empty() ||
        overlaps(cfg.outputRoot, primary) ||
        overlaps(cfg.outputRoot, cfg.journalRoot))
      throw std::runtime_error(
          "shadow output must be disjoint from primary and journal roots");
    for (const auto &root : cfg.protectedRoots)
      if (!root.empty() && overlaps(cfg.outputRoot, root))
        throw std::runtime_error(
            "shadow output aliases a configured recorder root");
    if (cfg.retryMin.count() <= 0 || cfg.retryMax < cfg.retryMin ||
        cfg.compareInterval.count() <= 0 || cfg.failureThreshold == 0 ||
        cfg.failureMinDuration.count() <= 0 ||
        cfg.failureCooldown < cfg.retryMax)
      throw std::runtime_error("invalid shadow retry/comparison interval");
  }
  void run(Product &p) noexcept {
    auto backoff = cfg.retryMin;
    std::string lastFailure;
    unsigned consecutive = 0;
    std::optional<std::chrono::steady_clock::time_point> failureSince;
    bool coolingDown = false;
    int64_t furthestCommitted = 0;
    const auto retry = [&](const std::string &reason) {
      if (stopping)
        return;
      p.running->set(0);
      p.failures->inc();
      const auto now = cfg.nowForTest ? cfg.nowForTest()
                                      : std::chrono::steady_clock::now();
      if (!failureSince || reason != lastFailure) {
        lastFailure = reason;
        consecutive = 0;
        failureSince = now;
        coolingDown = false;
        p.cooldown->set(0);
      }
      ++consecutive;
      if (!coolingDown && consecutive >= cfg.failureThreshold &&
          now - *failureSince >= cfg.failureMinDuration) {
        coolingDown = true;
        p.cooldown->set(1);
        p.cooldowns->inc();
      }
      const auto delay = coolingDown ? cfg.failureCooldown : backoff;
      sLog_Warning("Shadow roller retry product="
                   << p.name << " error=" << reason << " consecutive="
                   << consecutive << " retryMs=" << delay.count());
      wait(delay);
      backoff = std::min(cfg.retryMax, backoff * 2);
    };
    while (!stopping) {
      try {
        validate();
        capture::validateSymbol(p.name);
        const auto first = parseTime(cfg.from);
        if (first % Day)
          throw std::runtime_error("shadow from must be UTC midnight");
        auto day = first;
        std::optional<JournalPos> checkpoint;
        const auto cpPath = fs::path(cfg.outputRoot) / p.name / "roller.json";
        if (fs::exists(cpPath)) {
          json cp;
          std::ifstream in(cpPath);
          in >> cp;
          const auto committed = cp.value("committedThroughMs", int64_t{0});
          p.committed = committed;
          furthestCommitted = std::max(furthestCommitted, committed);
          if (cp.contains("pos"))
            checkpoint = cp.at("pos").get<JournalPos>();
          while (cp.at("days").contains(std::to_string(day)) &&
                 cp["days"][std::to_string(day)]
                         .at("committedThroughMs")
                         .get<int64_t>() >= day + Day)
            day += Day;
        }
        {
          std::lock_guard lock(p.historyMutex);
          auto &near = p.served["near"];
          near.minuteThroughMs =
              std::max(near.minuteThroughMs, p.committed.load());
          auto &deep = p.served["deep"];
          deep.minuteThroughMs =
              std::max(deep.minuteThroughMs, p.committed.load());
        }
        LiveLead lead(cfg, p.name, p.leadGuard, *p.leadForks);
        LiveSource source(cfg, p.name, stopping, checkpoint, retry,
                          lead.enabled() ? &lead : nullptr);
        RollOptions o{cfg.journalRoot, cfg.outputRoot, p.name, day, day + Day};
        o.publisher = cfg.publisher;
        o.onRecorder = [&](recording::BookRecorder *h, const JournalFeed *f,
                           int64_t end) {
          {
            std::lock_guard lock(p.historyMutex);
            if (p.history)
              mergeServed(p); // keep the values of the recorder going away
            p.history = h;
            p.historyEndMs = end;
          }
          lead.attach(h, f);
        };
        o.cancelled = [&] { return stopping.load(); };
        o.productWriterLease = true;
        o.onInvalid = [&](const std::string &reason) {
          // Match batch: invalidate the book, then continue to the next
          // snapshot.
          sLog_Warning("Shadow journal invalidated product="
                       << p.name << " reason=" << reason);
        };
        o.nextRecord = [&](JournalReader &reader, JournalRecord &input) {
          if (!source.next(reader, input))
            return false;
          return true;
        };
        o.onApplied = [&](const JournalRecord &input) {
          p.last->store(input.record.time.systemNs / 1000000);
          p.running->set(1);
          p.records->inc();
          if (cfg.observeForTest)
            cfg.observeForTest(input, true);
        };
        o.onCommitted = [&](int64_t through) {
          p.committed = through;
          // Replaying the old checkpoint is not recovery from a persistent
          // fault.
          if (through > furthestCommitted) {
            furthestCommitted = through;
            backoff = cfg.retryMin;
            consecutive = 0;
            lastFailure.clear();
            failureSince.reset();
            coolingDown = false;
            p.cooldown->set(0);
          }
        };
        sLog_App("Roller started product="
                 << p.name << " mode=" << (cfg.publisher ? "live" : "shadow")
                 << " day=" << day << " root=" << cfg.outputRoot);
        roll(o);
        // Midnight rotates through the same anchor/replay path as batch.
      } catch (const std::exception &e) {
        retry(e.what());
      } catch (...) {
        retry("unknown shadow failure");
      }
      p.running->set(0);
    }
  }
  // Under historyMutex. Watermarks never regress: a new day's recorder starts
  // from its anchor, and the old one runs past its commit ceiling.
  static void mergeServed(const Product &p) {
    for (const auto *layer : {"near", "deep"}) {
      auto w = p.history->watermarks(p.name, layer);
      w.minuteThroughMs = std::min(w.minuteThroughMs, p.historyEndMs);
      w.hourThroughMs = std::min(w.hourThroughMs, p.historyEndMs);
      auto &s = p.served[layer];
      s.minuteThroughMs = std::max(s.minuteThroughMs, w.minuteThroughMs);
      s.hourThroughMs = std::max(s.hourThroughMs, w.hourThroughMs);
      s.lastColumnMs = std::max(s.lastColumnMs, w.lastColumnMs);
    }
  }
  const Product *find(const std::string &name) const {
    for (const auto &p : products)
      if (p->name == name)
        return p.get();
    return nullptr;
  }
  void check() noexcept {
    while (!stopping) {
      for (auto &p : products) {
        if (stopping)
          break;
        if (p->refused)
          continue;
        try {
          validate();
          const auto first = parseTime(cfg.from);
          const auto progressPath =
              fs::path(cfg.outputRoot) / p->name / "comparison.json";
          p->compared = first;
          uint64_t nearTotal = 0, deepTotal = 0;
          if (fs::exists(progressPath)) {
            json saved;
            std::ifstream in(progressPath);
            in >> saved;
            if (saved.at("version") != 1 || saved.at("product") != p->name ||
                saved.at("fromMs") != first)
              throw std::runtime_error(
                  "comparison checkpoint identity mismatch");
            p->compared = saved.at("comparedThroughMs").get<int64_t>();
            if (p->compared < first || p->compared % Hour)
              throw std::runtime_error("invalid comparison watermark");
            nearTotal = saved.at("nearMismatch").get<uint64_t>();
            deepTotal = saved.at("deepMismatch").get<uint64_t>();
            p->lastComparison->set(
                saved.at("completedAtSeconds").get<double>());
            p->comparisonCheckpointSeen = true;
          } else {
            if (p->comparisonCheckpointSeen)
              throw std::runtime_error("comparison checkpoint disappeared");
            // An unavailable volume is not evidence of a new output root.
            // Wait for recorder provenance before exposing initial zero totals.
            if (!fs::exists(progressPath.parent_path() / "roller.json"))
              continue;
          }
          // Reload also covers an interrupted publication after atomic rename.
          p->comparison->near = nearTotal;
          p->comparison->deep = deepTotal;
          p->comparison->ready = true;
          const auto end =
              std::min(p->committed.load(), nowMs() - 2000) / Hour * Hour;
          // One hour at a time, sequential across products; no work on
          // the source workers, primary path, or metrics scrape thread.
          while (p->compared < end && !stopping) {
            QTemporaryDir temp;
            if (!temp.isValid())
              throw std::runtime_error("shadow oracle temp directory");
            const auto begin = p->compared;
            const auto batchEnd = std::min(end, begin / Day * Day + Day);
            RollOptions o{cfg.journalRoot, temp.path().toStdString(), p->name,
                          begin / Day * Day, batchEnd};
            o.cancelled = [&] { return stopping.load(); };
            roll(o);
            if (stopping)
              break;
            while (p->compared < batchEnd && !stopping) {
              metrics::Counter nearDelta, deepDelta;
              for (const auto *layer : {"near", "deep"}) {
                auto report = compareShadow(
                    cfg.outputRoot, o.outputRoot, primary, p->name, layer,
                    p->compared, p->compared + Hour,
                    std::string_view(layer) == "near" ? nearDelta : deepDelta);
                sLog_Data("Shadow roller comparison " << report.dump());
              }
              const auto completed = nowMs() / 1000.;
              // Commit both layers and the exclusive hour watermark together,
              // before publishing metrics. Failed comparisons never count
              // twice.
              writeCheckpoint(progressPath,
                              {{"version", 1},
                               {"product", p->name},
                               {"fromMs", first},
                               {"comparedThroughMs", p->compared + Hour},
                               {"nearMismatch", nearTotal + nearDelta.value()},
                               {"deepMismatch", deepTotal + deepDelta.value()},
                               {"completedAtSeconds", completed}});
              p->comparisonCheckpointSeen = true;
              nearTotal += nearDelta.value();
              deepTotal += deepDelta.value();
              p->comparison->near = nearTotal;
              p->comparison->deep = deepTotal;
              p->compared += Hour;
              p->lastComparison->set(completed);
            }
          }
        } catch (const std::exception &e) {
          p->compareFailures->inc();
          sLog_Warning("Shadow comparison failed product="
                       << p->name << " error=" << e.what());
        } catch (...) {
          p->compareFailures->inc();
          sLog_Error("Shadow comparison unknown failure product=" << p->name);
        }
      }
      wait(cfg.compareInterval, true);
    }
  }
};
ShadowRoller::ShadowRoller(ShadowConfig c, std::vector<std::string> p,
                           fs::path root, metrics::MetricsRegistry &r) {
  if (c.enabled)
    m = std::make_unique<Impl>(std::move(c), std::move(p), std::move(root), r);
}
ShadowRoller::~ShadowRoller() = default;
recording::BookRecorder::Watermarks
ShadowRoller::watermarks(const std::string &product,
                         const std::string &layer) const {
  const auto *p = m ? m->find(product) : nullptr;
  if (!p)
    return {};
  std::lock_guard lock(p->historyMutex);
  if (p->history)
    Impl::mergeServed(*p);
  const auto it = p->served.find(layer);
  return it == p->served.end() ? recording::BookRecorder::Watermarks{}
                               : it->second;
}
bool ShadowRoller::running(const std::string &product) const {
  const auto *p = m ? m->find(product) : nullptr;
  return p && p->running->value() > 0;
}
void ShadowRoller::stop() {
  if (m)
    m->stop();
}
} // namespace sentinel::roller
