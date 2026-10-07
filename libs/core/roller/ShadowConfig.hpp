#pragma once
#include "marketdata/model/TradeData.h"
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>
namespace recording {
struct Hmc2Record;
}
namespace sentinel::roller {
struct JournalRecord;
// recording.live_feed: journal (slice D-b1): the server's live model follows
// the capture journal instead of a Coinbase WebSocket. Installed by the server,
// never read from config and not part of configHash. Every call is made on the
// product's roller worker and must be one queued hand-off to the model's thread.
struct ModelSink {
  // A synthesized whole-book snapshot (native prices) at the socket tip.
  std::function<void(const std::string &product, std::vector<OrderBookLevel> bids,
                     std::vector<OrderBookLevel> asks, int64_t envelopeMs)>
      snapshot;
  // One provisional record's level changes, in arrival order.
  std::function<void(const std::string &product,
                     std::vector<BookLevelUpdate> updates, int64_t exchangeMs)>
      updates;
  // The book is unusable until the next snapshot.
  std::function<void(const std::string &product, const std::string &reason)>
      invalidate;
  std::function<void(const Trade &)> trade; // aggressor side
  std::function<void(const std::string &product, bool connected)> connection;
  // False while the product's journal feed is not live at the tip (recovery,
  // day rotation): the model holds candle closing, so replayed trades build
  // their bars in order; true after the seed. Optional.
  std::function<void(const std::string &product, bool live)> live;
  // The journal header's Coinbase product JSON (no REST).
  std::function<void(const std::string &product, const nlohmann::json &)>
      metadata;
  explicit operator bool() const { return bool(snapshot); }
};
struct ShadowConfig {
  bool enabled = false;
  std::string journalRoot = "/Volumes/T7/sentinel-data/raw-l2";
  std::string outputRoot = "/Volumes/T7/sentinel-data/hmc2";
  std::string socketPath; // empty: ~/Sentinel-runtime/run/capture.sock
  // Required explicit UTC start on first deployment; checkpoints retain it.
  std::string from;
  std::vector<std::string> protectedRoots; // configured primary/fallback roots
  std::chrono::milliseconds retryMin{1000}, retryMax{60000};
  std::chrono::milliseconds compareInterval{3600000};
  // Three identical faults without new durable progress enter a slow probe
  // loop.
  unsigned failureThreshold = 3;
  std::chrono::milliseconds failureMinDuration{120000};
  std::chrono::milliseconds failureCooldown{600000};
  // Deterministic monotonic clock for retry tests.
  std::function<std::chrono::steady_clock::time_point()> nowForTest;
  // Synchronization seam: called under the wait mutex after reading the
  // predicate.
  std::function<void()> beforeCompareWaitForTest;
  size_t pendingBytes = 32 * 1024 * 1024;
  // Receive/apply instrumentation, shadow thread only; no primary hook.
  std::function<void(const JournalRecord &, bool applied)> observeForTest;
  // roller_shadow.products; empty = the server's default_symbols. Products
  // without a matching journal are refused at startup (no worker).
  std::vector<std::string> products;
  // Serving path (recording.source: roller), installed by the server, never
  // read from config. Committed minutes come from the history recorder; the
  // forming minute from a lead fork that also applies provisional fan-out
  // records. Empty: shadow only.
  std::function<void(std::shared_ptr<const recording::Hmc2Record>)> publisher;
  // With publisher: withdraws the product's published provisional minutes
  // (LiveService::retractProvisional) after a lead that published is dropped.
  std::function<void(const std::string &product)> retractLive;
  // With retractLive: true when the live cache holds a committed minute of
  // (product, layer), storing the given one if not. The lead publishes only
  // while both layers hold one (it carries a withdrawal to the client). Empty:
  // no gate (tests with a recording sink).
  std::function<bool(const std::string &product, const std::string &layer,
                     std::shared_ptr<const recording::Hmc2Record>)>
      ensureLiveFinal;
  int64_t livePublishMs = 500;
  // Wall clock (epoch ms) for lead ticks every 250 ms; tests inject.
  std::function<int64_t()> liveNowForTest;
  // recording.live_feed: journal. Empty: the model is fed by the engine.
  ModelSink model;
  // With model: no socket record for this long at the tip invalidates the
  // model's book and reports connected=false (on the liveNowForTest clock).
  int64_t tipSilenceMs = 30000;
};
} // namespace sentinel::roller
