#pragma once
#include <QObject>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace sentinel::metrics {
class Counter;
class MetricsRegistry;

// Main-thread queue latency: a helper wakes every 100 ms and posts its steady
// send time to a receiver on the constructing thread. Only one post may be
// outstanding; additional wakes increment the skipped counter. The receiver
// records delivery minus send into fixed storage, with no allocation or lock.
// Construct, register, start, stop, destroy and scrape on the main thread;
// stop joins the helper and cancels its pending event before receiver teardown.
class EventLoopLagSampler {
  public:
    static constexpr int kIntervalMs = 100;
    static constexpr int64_t kWindowMs = 60'000;
    static constexpr double kLateMs = 100.0;
    EventLoopLagSampler() = default;
    ~EventLoopLagSampler();
    EventLoopLagSampler(const EventLoopLagSampler&) = delete;
    EventLoopLagSampler& operator=(const EventLoopLagSampler&) = delete;
    // sentinel_server_event_loop_lag_ms{quantile="0.5|0.95|0.99|max"} (gauge,
    // absent until the window holds a delivered sample) and
    // sentinel_server_event_loop_late_ticks_total (queue wait >100 ms) and
    // sentinel_server_event_loop_skipped_ticks_total (a post still outstanding).
    void registerMetrics(MetricsRegistry& registry);
    void start();
    void stop();
    // Queue wait of the q-quantile (0..1), or -1 with no delivered sample.
    double quantileMs(double q);

  private:
    // Deterministic send/delivery/query driver and helper synchronization.
    friend struct EventLoopLagTestAccess;
    using Clock = std::chrono::steady_clock;
    void run();
    // Called with m_stopMutex held, serializing posting with stop.
    bool postProbe();
    void recordQueueLatency(Clock::time_point sent, Clock::time_point delivered);
    double quantileMsAt(double q, int64_t nowMs);
    struct Entry { int64_t atMs = 0; float lagMs = 0; };
    // 60 s at 100 ms is 600 ticks; slack for timer jitter.
    std::array<Entry, 1024> m_ring{};
    std::array<float, 1024> m_scratch{};
    size_t m_next = 0, m_size = 0;
    Counter* m_late = nullptr;
    Counter* m_skipped = nullptr;
    std::atomic<bool> m_outstanding{false};
    std::mutex m_stopMutex;
    std::condition_variable m_wake;
    bool m_stopping = true; // Protected by m_stopMutex.
    std::thread m_worker;
    QObject m_receiver;
};
} // namespace sentinel::metrics
