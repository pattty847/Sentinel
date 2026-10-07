#include "EventLoopLag.hpp"
#include "MetricsRegistry.hpp"
#include <algorithm>
#include <cmath>

namespace sentinel::metrics {
EventLoopLagSampler::EventLoopLagSampler() {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kIntervalMs);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { tick(); });
}

void EventLoopLagSampler::start() {
    m_clock.start();
    m_lastNs = 0;
    m_timer.start();
}

void EventLoopLagSampler::tick() {
    const int64_t nowNs = m_clock.nsecsElapsed();
    const double lagMs = std::max(0.0, double(nowNs - m_lastNs) / 1e6 - kIntervalMs);
    m_lastNs = nowNs;
    m_ring[m_next] = {nowNs / 1'000'000, float(lagMs)};
    m_next = (m_next + 1) % m_ring.size();
    m_size = std::min(m_size + 1, m_ring.size());
    if (lagMs > kLateMs && m_late) m_late->inc();
}

double EventLoopLagSampler::quantileMs(double q) {
    if (!m_clock.isValid()) return -1;
    const int64_t from = m_clock.elapsed() - kWindowMs;
    size_t n = 0;
    for (size_t i = 0; i < m_size; ++i)
        if (m_ring[i].atMs >= from) m_scratch[n++] = m_ring[i].lagMs;
    if (n == 0) return -1;
    const auto rank = std::min(n - 1, size_t(std::ceil(q * double(n))) - (q > 0 ? 1 : 0));
    std::nth_element(m_scratch.begin(), m_scratch.begin() + rank, m_scratch.begin() + n);
    return m_scratch[rank];
}

void EventLoopLagSampler::registerMetrics(MetricsRegistry& registry) {
    m_late = &registry.counter("sentinel_server_event_loop_late_ticks_total",
                               "Main-thread lag timer ticks that fired more than 100 ms late.");
    registry.familyFn("sentinel_server_event_loop_lag_ms",
                      "Main-thread event-loop lag over the last 60 s: how late a 100 ms timer fired "
                      "(quantile 0.5, 0.95, 0.99 and max).",
                      MetricsRegistry::Type::Gauge, [this] {
                          std::vector<MetricsRegistry::Sample> samples;
                          for (const auto& [label, q] : {std::pair{"0.5", 0.5}, std::pair{"0.95", 0.95},
                                                         std::pair{"0.99", 0.99}, std::pair{"max", 1.0}}) {
                              const double v = quantileMs(q);
                              if (v < 0) return std::vector<MetricsRegistry::Sample>{};
                              samples.push_back({{{"quantile", label}}, v});
                          }
                          return samples;
                      });
}
} // namespace sentinel::metrics
