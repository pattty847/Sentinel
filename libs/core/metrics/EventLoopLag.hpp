#pragma once
#include <QElapsedTimer>
#include <QTimer>
#include <array>
#include <cstdint>

namespace sentinel::metrics {
class Counter;
class MetricsRegistry;

// Main-thread event-loop lag: a repeating timer on the constructing thread
// records how late each tick fires (time since the previous tick minus the
// interval, floored at 0) into a fixed ring. Per tick: one clock read, one ring
// store, at most one relaxed counter increment; no allocation. Quantiles are
// computed at scrape over the last `window` from a preallocated scratch array.
// Construct, register and render on the same thread (the main thread in
// sentinel-server): ring and scratch are that thread's state.
class EventLoopLagSampler {
  public:
    static constexpr int kIntervalMs = 100;
    static constexpr int64_t kWindowMs = 60'000;
    static constexpr double kLateMs = 100.0;
    EventLoopLagSampler();
    EventLoopLagSampler(const EventLoopLagSampler&) = delete;
    EventLoopLagSampler& operator=(const EventLoopLagSampler&) = delete;
    // sentinel_server_event_loop_lag_ms{quantile="0.5|0.95|0.99|max"} (gauge,
    // absent until the window holds a tick) and
    // sentinel_server_event_loop_late_ticks_total (ticks more than 100 ms late).
    void registerMetrics(MetricsRegistry& registry);
    void start();
    // Lag of the q-quantile (0..1) over the window, or -1 with no tick in it.
    double quantileMs(double q);

  private:
    void tick();
    struct Entry { int64_t atMs = 0; float lagMs = 0; };
    // 60 s at 100 ms is 600 ticks; slack for timer jitter.
    std::array<Entry, 1024> m_ring{};
    std::array<float, 1024> m_scratch{};
    size_t m_next = 0, m_size = 0;
    int64_t m_lastNs = 0;
    QElapsedTimer m_clock;
    QTimer m_timer;
    Counter* m_late = nullptr;
};
} // namespace sentinel::metrics
