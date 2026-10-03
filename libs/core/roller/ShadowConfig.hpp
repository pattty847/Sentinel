#pragma once
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
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
};
} // namespace sentinel::roller
