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
    m_expectedNs = kIntervalNs;
    m_next = m_size = 0;
    m_timer.start();
}

void EventLoopLagSampler::tick() {
    recordTick(m_clock.nsecsElapsed());
}

void EventLoopLagSampler::recordTick(int64_t nowNs) {
    const double lagMs = std::max(0.0, double(nowNs - m_expectedNs) / 1e6);
    // Qt's calculateNextTimeout: advance the previous deadline, then rebase
    // to now + interval only if the next deadline is already in the past.
    // Keeping sub-interval delays out of the deadline preserves sustained lag;
    // rebasing after an overrun avoids carrying a stall into future samples.
    m_expectedNs += kIntervalNs;
    if (m_expectedNs < nowNs) m_expectedNs = nowNs + kIntervalNs;
    m_ring[m_next] = {nowNs / 1'000'000, float(lagMs)};
    m_next = (m_next + 1) % m_ring.size();
    m_size = std::min(m_size + 1, m_ring.size());
    if (lagMs > kLateMs && m_late) m_late->inc();
}

double EventLoopLagSampler::quantileMs(double q) {
    if (!m_clock.isValid()) return -1;
    return quantileMsAt(q, m_clock.elapsed());
}

double EventLoopLagSampler::quantileMsAt(double q, int64_t nowMs) {
    const int64_t from = nowMs - kWindowMs;
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
