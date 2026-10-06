#pragma once
#include "Roller.hpp"
#include "ShadowConfig.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <atomic>
#include <chrono>

namespace sentinel::roller {
// All I/O, parsing and retry waits belong to independent shadow workers. No
// callback into primary recording/streaming, and no exception escapes a worker.
class ShadowRoller {
public:
  ShadowRoller(ShadowConfig, std::vector<std::string> products,
               std::filesystem::path primaryRoot, metrics::MetricsRegistry &);
  ~ShadowRoller();
  ShadowRoller(const ShadowRoller &) = delete;
  ShadowRoller &operator=(const ShadowRoller &) = delete;
  void stop();
  // False when roller_shadow.enabled is false (nothing constructed).
  bool active() const { return m != nullptr; }
  // Thread-safe (chunk workers): the product's committed history watermarks,
  // never regressing across day rotation; zero when the product is not rolled.
  recording::BookRecorder::Watermarks
  watermarks(const std::string &product, const std::string &layer) const;
  // Thread-safe: the product's worker is consuming the journal/socket.
  bool running(const std::string &product) const;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
// Shared with the hourly checker and divergent-fixture tests. Strict journal
// parity includes missing and partial buckets; cross-connection is
// informational.
nlohmann::json compareShadow(const std::filesystem::path &shadow,
                             const std::filesystem::path &batch,
                             const std::filesystem::path &primary,
                             const std::string &product,
                             const std::string &layer, int64_t from, int64_t to,
                             metrics::Counter &mismatch);
} // namespace sentinel::roller
