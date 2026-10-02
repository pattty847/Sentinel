#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Process metrics in the Prometheus text exposition format 0.0.4, served on
// GET /metrics (docs: ops/monitoring/README.md). Plain C++, no Qt.
//
// Threading:
// - Counter::inc and Gauge::set/add are one relaxed atomic operation each: safe
//   from any thread, no allocation, no lock. These are the only calls allowed on
//   a hot path (recorder worker, I/O strand, per-message callbacks).
// - Registration (counter/gauge/...Fn) and render() take one registry mutex.
//   Register at startup, keep the returned reference.
// - Samplers (...Fn) run inside render(), on the thread that renders (the main
//   thread in sentinel-server). A sampler reads only atomics or state owned by
//   that thread; it never reads another thread's non-atomic state and never calls
//   back into the registry.
namespace sentinel::metrics {

using Labels = std::vector<std::pair<std::string, std::string>>;
// Returns the sample value, or nullopt to omit the sample from this scrape.
using Sampler = std::function<std::optional<double>()>;

class Counter {
  public:
    void inc(uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
    std::atomic<uint64_t> value_{0};
};

class Gauge {
  public:
    void set(double v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(double d) noexcept {
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + d, std::memory_order_relaxed)) {
        }
    }
    double value() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
    std::atomic<double> value_{0.0};
};

class MetricsRegistry {
  public:
    enum class Type { Counter, Gauge };

    MetricsRegistry();
    ~MetricsRegistry();
    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    // Same (name, labels) again returns the same object. Throws
    // std::invalid_argument for an invalid metric or label name and
    // std::logic_error when a name is reused with another type, or when an
    // atomic and a sampler share (name, labels). The reference lives as long as
    // the registry.
    Counter& counter(std::string_view name, std::string_view help, const Labels& labels = {});
    Gauge& gauge(std::string_view name, std::string_view help, const Labels& labels = {});
    // Values computed at scrape time (see the threading note above).
    void counterFn(std::string_view name, std::string_view help, const Labels& labels, Sampler sampler);
    void gaugeFn(std::string_view name, std::string_view help, const Labels& labels, Sampler sampler);

    // Families and series in registration order: deterministic output.
    std::string render() const;

    static constexpr std::string_view kContentType = "text/plain; version=0.0.4; charset=utf-8";

    // Exposed for tests.
    static bool validMetricName(std::string_view name);
    static bool validLabelName(std::string_view name);
    static std::string escapeLabelValue(std::string_view value);
    static std::string escapeHelp(std::string_view help);
    static std::string formatValue(double value);

  private:
    struct Series;
    struct Family;
    Family& family(std::string_view name, std::string_view help, Type type);
    Series* find(Family& f, const std::string& labelText);
    void addSampler(std::string_view name, std::string_view help, Type type, const Labels& labels, Sampler sampler);
    static std::string renderLabels(const Labels& labels);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Family>> families_;
};

} // namespace sentinel::metrics
