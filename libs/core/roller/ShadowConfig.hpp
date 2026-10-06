#pragma once
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace recording {
struct Hmc2Record;
}
namespace sentinel::roller {
struct JournalRecord;
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
};
} // namespace sentinel::roller
