#include "ShadowRoller.hpp"
#include "SentinelLogging.hpp"
#include "capture/CaptureFanout.hpp"
#include <QDir>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
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
// Probe only (evaluated when roller.live is enabled): lowest native row price.
double lowestPrice(const recording::Hmc2Record &r) {
  int64_t low = INT64_MAX;
  for (const auto &e : r.entries)
    low = std::min(low, e.row);
  return r.entries.empty() ? 0.0
                           : double(low) * double(r.header.rowTickUnits) /
                                 r.header.priceScale;
}
// Option C live path (owner 2026-10-06), roller worker thread only. The lead is
// a non-persisting fork of the day's history recorder (Publication::Lead) that
// also applies provisional fan-out records as they arrive. History still
// applies only durable prefixes (INV-114) and publishes committed minutes. Any
// provisional discard (retract, disconnect, EOF, socket failure) and day end
// drops the lead and withdraws everything it published from the live cache
// (committed minutes stay); the next fork republishes from durable history.
class LiveLead {
  const ShadowConfig &cfg;
  const std::string product;
  metrics::Counter &forks;
  std::function<int64_t()> committedThrough;
  bool waitLogged = false;
  bool published = false; // lead worker writes; read after the worker joined
  recording::BookRecorder *history = nullptr;
  const JournalFeed *historyFeed = nullptr;
  std::unique_ptr<JournalFeed> feed;
  std::unique_ptr<recording::BookRecorder> lead;
  int64_t lastTickMs = 0, nextFailureLogMs = 0, nextForkMs = 0;
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
  // A withdrawal reaches a client only with a committed minute to resend.
  // After a restart history republishes none (replay below the commit floor):
  // seed the newest persisted minute of each layer, or wait for the first.
  bool finalsCached() {
    if (!cfg.ensureLiveFinal)
      return true;
    for (const auto *layer : {"near", "deep"}) {
      if (cfg.ensureLiveFinal(product, layer, nullptr))
        continue;
      const auto end = committedThrough();
      std::shared_ptr<recording::Hmc2Record> newest;
      for (const auto back : {Hour, Day}) {
        if (end <= recording::kHmc2MinMs)
          break;
        auto rows = recording::Hmc2Store::readRange(
            cfg.outputRoot, product, layer, 60000,
            std::max(recording::kHmc2MinMs, end - back), end);
        if (!rows.empty()) {
          newest =
              std::make_shared<recording::Hmc2Record>(std::move(rows.back()));
          newest->committedThroughMs = newest->bucketStartMs + 60000;
          break;
        }
      }
      if (!newest || !cfg.ensureLiveFinal(product, layer, newest))
        return false;
    }
    return true;
  }

public:
  LiveLead(const ShadowConfig &c, std::string p, metrics::Counter &f,
           std::function<int64_t()> committed)
      : cfg(c), product(std::move(p)), forks(f),
        committedThrough(std::move(committed)) {}
  ~LiveLead() { discard(); }
  bool enabled() const { return bool(cfg.publisher); }
  void attach(recording::BookRecorder *h, const JournalFeed *f) {
    history = h;
    historyFeed = f;
    if (!h)
      discard();
  }
  // Joins the lead worker first: no publication of withdrawn input follows
  // the withdrawal.
  void discard() {
    lead.reset();
    feed.reset();
    if (!std::exchange(published, false) || !cfg.retractLive)
      return;
    try {
      cfg.retractLive(product);
      sLog_Data("Roller live provisional withdrawn product=" << product);
    } catch (const std::exception &e) {
      sLog_Error("Roller live withdrawal failed product="
                 << product << " error=" << e.what());
    }
  }
  // At the socket, after every returned record reached history: fork from the
  // durable state, then apply the provisional suffix received so far.
  void ensure(const std::deque<JournalRecord> &pending) {
    if (!enabled() || lead || !history || !historyFeed || nowMs() < nextForkMs)
      return;
    try {
      if (!finalsCached()) {
        if (!std::exchange(waitLogged, true))
          sLog_Data("Roller live lead waits for a committed minute product="
                    << product);
        nextForkMs = nowMs() + 1000;
        return;
      }
      auto publish = [this, sink = cfg.publisher, name = product](
                         std::shared_ptr<const recording::Hmc2Record> r) {
        published = true;
        sLog_Probe("roller.live",
                   "product=" << name << " layer=" << r->header.layer
                              << " bucket=" << r->bucketStartMs
                              << " observed=" << r->observedMs
                              << " entries=" << r->entries.size() << " ageMs="
                              << nowMs() - (r->bucketStartMs + r->observedMs)
                              << " lowPrice=" << lowestPrice(*r));
        sink(std::move(r));
      };
      const auto began = std::chrono::steady_clock::now();
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
      sLog_Data("Roller live lead forked product="
                << product << " provisional=" << pending.size() << " forkUs="
                << std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - began)
                       .count());
      for (const auto &r : pending)
        feed->apply(r);
      lastTickMs = 0;
      tick();
    } catch (const std::exception &e) {
      failed("fork", e);
      nextForkMs = nowMs() + 1000; // never a fork per socket read
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
// recording.live_feed: journal (slice D-b1), roller worker thread only. The
// server's live model follows this product's journal through cfg.model:
// - `durable` follows the history feed (durable records, catch-up included;
//   RollOptions::onFeed chains its callbacks, so nothing is parsed twice);
// - at the socket tip `live` starts as a copy of it and applies every
//   provisional record on arrival through its own fork of the history feed.
// Nothing reaches the model before the tip. Each seed (first tip, recovery, day
// rotation, silence) hands over the header metadata and one synthesized
// snapshot, then one queued hand-off per provisional record. A failure, a
// provisional discard or tipSilenceMs without a socket record invalidates the
// model's book and reports connected=false; the next tip re-seeds from durable
// state. A day rotation re-seeds without invalidating. Trades reach the model
// once per journal position: provisional ones on arrival; durable ones only to
// fill a gap after the model was live (never the day's history at a restart).
class ModelTap {
  struct Book {
    bool valid = false;
    int64_t envelopeMs = 0;
    std::map<double, double> bids, asks; // native prices, whole book
    void clear() {
      valid = false;
      bids.clear();
      asks.clear();
    }
    void apply(int64_t envelope, const std::vector<recording::Level> &levels) {
      envelopeMs = envelope;
      for (const auto &l : levels) {
        if (!std::isfinite(l.price) || !std::isfinite(l.size))
          continue;
        auto &side = l.isBid ? bids : asks;
        if (l.size > 0)
          side[l.price] = l.size;
        else
          side.erase(l.price);
      }
    }
    void snapshot(int64_t envelope,
                  const std::vector<recording::Level> &levels) {
      bids.clear();
      asks.clear();
      apply(envelope, levels);
      valid = true;
    }
  };
  const ShadowConfig &cfg;
  const std::string product;
  std::atomic<bool> &reseed; // set by ShadowRoller::requestReseed
  Book durable, live;
  bool durableUp = true, liveUp = false;
  bool seeded = false;   // live follows the socket tip
  bool shown = false;    // the model holds a book from this tap
  bool reported = false; // last connection state handed to the model
  bool liveReported = false; // last ModelSink::live state
  bool tradeHere = false; // the record being applied carries new trades
  const JournalFeed *history = nullptr;
  std::unique_ptr<JournalFeed> feed;
  // Newest journal position whose trades reached the model.
  std::optional<JournalPos> delivered;
  std::string lastRun;
  json sentMetadata;
  int64_t lastRecordMs = 0;
  // Bound by the current LiveSource (run order and header metadata).
  std::function<bool(const JournalPos &, const JournalPos &)> older;
  std::function<const json *(const std::string &)> metadataOf;
  int64_t now() const {
    return cfg.liveNowForTest ? cfg.liveNowForTest() : nowMs();
  }
  // Strictly newer than the delivered position; unknown order is not newer.
  bool undelivered(const JournalPos &p) const {
    if (!delivered)
      return false;
    if (!older)
      return false;
    try {
      return !older(p, *delivered);
    } catch (const std::exception &) {
      return false;
    }
  }
  void sendMetadata(const std::string &run) {
    lastRun = run;
    if (!metadataOf)
      return;
    try {
      const auto *m = metadataOf(run);
      if (!m || *m == sentMetadata)
        return;
      sentMetadata = *m;
      cfg.model.metadata(product, *m);
    } catch (const std::exception &e) {
      sLog_Warning("Journal live metadata unavailable product="
                   << product << " run=" << run << " error=" << e.what());
    }
  }
  void show(const Book &b) {
    std::vector<OrderBookLevel> bids, asks;
    bids.reserve(b.bids.size());
    asks.reserve(b.asks.size());
    for (auto it = b.bids.rbegin(); it != b.bids.rend(); ++it)
      bids.push_back({it->first, it->second});
    for (const auto &[price, size] : b.asks)
      asks.push_back({price, size});
    cfg.model.snapshot(product, std::move(bids), std::move(asks),
                       b.envelopeMs);
    shown = true;
  }
  void hide(const std::string &reason) {
    if (!std::exchange(shown, false))
      return;
    cfg.model.invalidate(product, reason);
  }
  void report() {
    const bool connected = seeded && liveUp;
    if (connected == std::exchange(reported, connected))
      return;
    cfg.model.connection(product, connected);
  }
  // Candle closing follows the journal: held while not seeded, so trades the
  // recovery replays build their bars in order (ModelSink::live).
  void setLive(bool v) {
    if (v == std::exchange(liveReported, v))
      return;
    if (cfg.model.live)
      cfg.model.live(product, v);
  }
  void unseed() {
    seeded = false;
    feed.reset();
    live.clear();
    setLive(false);
  }
  void serviceReseed() {
    if (!reseed.load(std::memory_order_relaxed) || !reseed.exchange(false))
      return;
    if (!seeded || !live.valid)
      return; // the next seed sends a snapshot anyway
    show(live);
    sLog_Data("Journal live re-seed product=" << product << " bids="
                                              << live.bids.size()
                                              << " asks=" << live.asks.size());
  }

public:
  ModelTap(const ShadowConfig &c, std::string p, std::atomic<bool> &r)
      : cfg(c), product(std::move(p)), reseed(r) {}
  bool enabled() const { return bool(cfg.model); }
  void bind(std::function<bool(const JournalPos &, const JournalPos &)> o,
            std::function<const json *(const std::string &)> m) {
    older = std::move(o);
    metadataOf = std::move(m);
  }
  // RollOptions::onFeed: follow the day's history feed. A new day replays
  // from its anchor, so the durable book starts empty.
  void observe(JournalFeed &f) {
    history = &f;
    durable.clear();
    durableUp = true;
    f.onSnapshot = [this, next = std::move(f.onSnapshot)](
                       int64_t e, int64_t l, std::vector<recording::Level> v) {
      durable.snapshot(e, v);
      if (next)
        next(e, l, std::move(v));
    };
    f.onUpdates = [this, next = std::move(f.onUpdates)](
                      int64_t e, int64_t l, std::vector<recording::Level> v) {
      durable.apply(e, v);
      if (next)
        next(e, l, std::move(v));
    };
    f.onInvalid = [this, next = std::move(f.onInvalid)](
                      int64_t t, const std::string &reason) {
      durable.clear();
      if (next)
        next(t, reason);
    };
    f.onTrade = [this](const Trade &t) {
      if (tradeHere)
        cfg.model.trade(t);
    };
    f.onConnection = [this](bool up) { durableUp = up; };
  }
  // onRecorder(nullptr): the day's feed goes away (day end or failure). Quiet:
  // the model keeps its book until the next seed or drop().
  void detach() {
    history = nullptr;
    unseed();
  }
  // Before history applies a durable record (RollOptions::nextRecord).
  void durableRecord(const JournalPos &p) {
    tradeHere = !seeded && undelivered(p);
    if (tradeHere)
      delivered = p;
  }
  // At the socket tip, after every returned record reached history.
  void ensure(const std::deque<JournalRecord> &pending,
              const JournalPos &applied) {
    if (seeded || !history)
      return;
    const auto began = std::chrono::steady_clock::now();
    sendMetadata(applied.run);
    live = durable;
    liveUp = durableUp;
    feed = std::make_unique<JournalFeed>(*history);
    feed->onSnapshot = [this](int64_t e, int64_t,
                              std::vector<recording::Level> v) {
      live.snapshot(e, v);
      show(live);
    };
    feed->onUpdates = [this](int64_t e, int64_t,
                             std::vector<recording::Level> v) {
      live.apply(e, v);
      if (!shown)
        return;
      std::vector<BookLevelUpdate> updates;
      updates.reserve(v.size());
      for (const auto &l : v)
        updates.push_back({l.isBid, l.price, l.size});
      cfg.model.updates(product, std::move(updates), e);
    };
    feed->onInvalid = [this](int64_t, const std::string &reason) {
      live.clear();
      hide(reason);
    };
    feed->onTrade = [this](const Trade &t) {
      if (tradeHere)
        cfg.model.trade(t);
    };
    feed->onConnection = [this](bool up) {
      liveUp = up;
      report();
    };
    feed->onTick = {};
    seeded = true;
    if (!delivered || undelivered(applied))
      delivered = applied; // the durable tip's trades are history
    lastRecordMs = now();
    if (live.valid)
      show(live);
    else
      hide("journal book invalid at the tip");
    sLog_Data("Journal live seed product="
              << product << " valid=" << live.valid
              << " bids=" << live.bids.size() << " asks=" << live.asks.size()
              << " provisional=" << pending.size() << " seedUs="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - began)
                     .count());
    for (const auto &r : pending)
      apply(r);
    if (!seeded)
      return; // a replayed record failed: dropped
    report();
    setLive(true);
  }
  // A provisional record at the tip, on arrival.
  void apply(const JournalRecord &r) {
    if (!seeded)
      return;
    lastRecordMs = now();
    if (r.pos.run != lastRun)
      sendMetadata(r.pos.run);
    tradeHere = undelivered(r.pos);
    if (tradeHere)
      delivered = r.pos;
    sLog_Probe("journal.live",
               "product=" << product << " kind=" << int(r.record.kind)
                          << " ageMs="
                          << nowMs() - r.record.time.systemNs / 1000000);
    try {
      feed->apply(r);
    } catch (const std::exception &e) {
      drop(std::string("journal live feed failed: ") + e.what());
      return;
    }
    serviceReseed();
  }
  // Socket waits: the tip-age guard and re-seed requests.
  void idle() {
    if (!seeded)
      return;
    if (now() - lastRecordMs >= cfg.tipSilenceMs) {
      sLog_Warning("Journal live tip silent product="
                   << product << " ms=" << now() - lastRecordMs);
      drop("journal tip silent");
      return;
    }
    serviceReseed();
  }
  // Failure or provisional discard: the model's book is invalid until the
  // next seed.
  void drop(const std::string &reason) {
    unseed();
    if (shown)
      sLog_Data("Journal live book invalidated product=" << product
                                                        << " reason=" << reason);
    hide(reason);
    report();
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
  // At the socket with every returned record applied by history: the lead
  // may fork there.
  bool atTip = false;
  std::function<void(const std::string &)> retry;
  LiveLead *lead;
  ModelTap *tap;
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
      if (lead) {
        if (atTip) // quiet socket: retry a fork the final-minute gate deferred
          lead->ensure(pending);
        lead->idle();
      }
      if (tap)
        tap->idle();
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
      if (tap)
        tap->apply(pending.back());
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
    atTip = false;
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
             LiveLead *liveLead = nullptr, ModelTap *modelTap = nullptr)
      : cfg(c), product(std::move(p)), stopping(stop),
        retry(std::move(onRetry)), lead(liveLead), tap(modelTap) {
    handshake(checkpoint);
    if (tap)
      tap->bind(
          [this](const JournalPos &a, const JournalPos &b) {
            return older(a, b);
          },
          [this](const std::string &run) -> const json * {
            if (!metadata.contains(run))
              inventory();
            const auto it = metadata.find(run);
            return it == metadata.end() ? nullptr : &it->second;
          });
    // A day-anchor rebuild can exceed the bounded server backlog. Hold no
    // socket during that replay; reconnect at the fully applied durable tip.
    socket.abort();
    bytes.clear();
    discardProvisional();
    received.reset();
  }
  ~LiveSource() {
    if (tap)
      tap->bind({}, {});
  }
  LiveSource(const LiveSource &) = delete;
  LiveSource &operator=(const LiveSource &) = delete;
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
      if (lead) {
        atTip = true;
        lead->ensure(pending);
      }
      if (tap && applied)
        tap->ensure(pending, *applied);
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
    bool healthy = false; // readyMutex
    // recording.live_feed: journal: the model asked for a fresh snapshot.
    std::atomic<bool> reseed{false};
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
  std::mutex readyMutex;
  size_t healthyCount = 0; // readyMutex
  std::mutex mutex;
  std::condition_variable wake;
  std::thread checker;
  Impl(ShadowConfig c, std::vector<std::string> names, fs::path root,
       metrics::MetricsRegistry &registry)
      : cfg(std::move(c)), primary(std::move(root)) {
    for (auto &name : names) {
      auto p = std::make_unique<Product>();
      p->name = std::move(name);
      registerJournalMetrics(registry, p->name);
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
    // Outlives each day's source and feed: a day rotation re-seeds the model
    // without invalidating it.
    ModelTap tap(cfg, p.name, p.reseed);
    const auto retry = [&](const std::string &reason) {
      if (tap.enabled())
        tap.drop(reason);
      unhealthy(p, reason);
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
        LiveLead lead(cfg, p.name, *p.leadForks,
                      [&p] { return p.committed.load(); });
        LiveSource source(cfg, p.name, stopping, checkpoint, retry,
                          lead.enabled() ? &lead : nullptr,
                          tap.enabled() ? &tap : nullptr);
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
          if (h) {
            if (cfg.publisher)
              sLog_App("Roller writer open product=" << p.name);
          } else
            unhealthy(p, "writer closed");
          lead.attach(h, f);
          if (!h && !f)
            tap.detach();
        };
        if (tap.enabled())
          o.onFeed = [&](JournalFeed &f) { tap.observe(f); };
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
          if (tap.enabled())
            tap.durableRecord(input.pos);
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
          healthy(p);
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
  // Deploy readiness (deploy-runtime.sh), serving only: a product is healthy
  // from the first durable checkpoint of its current writer (after the policy
  // check, leases and a committed minute) until a failure or the writer
  // closes (also at day end). "Roller serving ready" is logged when every
  // configured product is healthy at once, "Roller serving not ready" on each
  // loss; the deploy check takes the latest of the two.
  void healthy(Product &p) {
    if (!cfg.publisher)
      return;
    std::lock_guard lock(readyMutex);
    if (std::exchange(p.healthy, true))
      return;
    sLog_App("Roller writer healthy product=" << p.name);
    if (++healthyCount == products.size())
      sLog_App("Roller serving ready products=" << products.size()
                                                << " root=" << cfg.outputRoot);
  }
  void unhealthy(Product &p, const std::string &reason) {
    if (!cfg.publisher)
      return;
    std::lock_guard lock(readyMutex);
    if (!std::exchange(p.healthy, false))
      return;
    --healthyCount;
    sLog_App("Roller serving not ready product=" << p.name
                                                 << " reason=" << reason);
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
void ShadowRoller::requestReseed(const std::string &product) {
  if (!m)
    return;
  for (auto &p : m->products)
    if (p->name == product)
      p->reseed.store(true);
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
