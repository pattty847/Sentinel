#include "EventLoopLag.hpp"
#include "MetricsRegistry.hpp"
#include "../SentinelLogging.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <algorithm>
#include <cmath>

namespace sentinel::metrics {
EventLoopLagSampler::~EventLoopLagSampler() { stop(); }

void EventLoopLagSampler::start() {
    stop();
    m_next = m_size = 0;
    {
        std::lock_guard lock(m_stopMutex);
        m_stopping = false;
    }
    m_worker = std::thread([this] { run(); });
}

void EventLoopLagSampler::stop() {
    {
        std::lock_guard lock(m_stopMutex);
        m_stopping = true;
    }
    m_wake.notify_one();
    if (m_worker.joinable()) m_worker.join();
    // This private receiver accepts only our probe events. Joining first means
    // no helper can post while they are removed, or after stop returns.
    QCoreApplication::removePostedEvents(&m_receiver, QEvent::MetaCall);
    m_outstanding.store(false, std::memory_order_release);
}

void EventLoopLagSampler::run() {
    const auto interval = std::chrono::milliseconds(kIntervalMs);
    auto nextWake = Clock::now() + interval;
    std::unique_lock lock(m_stopMutex);
    while (!m_wake.wait_until(lock, nextWake, [this] { return m_stopping; })) {
        postProbe();
        nextWake += interval;
        const auto now = Clock::now();
        if (nextWake <= now) nextWake = now + interval; // No wake-up burst after worker starvation.
    }
}

bool EventLoopLagSampler::postProbe() {
    if (m_stopping) return false;
    if (m_outstanding.exchange(true, std::memory_order_acq_rel)) {
        if (m_skipped) m_skipped->inc();
        return false;
    }
    const auto sent = Clock::now();
    if (!QMetaObject::invokeMethod(&m_receiver, [this, sent] {
            recordQueueLatency(sent, Clock::now());
            m_outstanding.store(false, std::memory_order_release);
        }, Qt::QueuedConnection)) {
        m_outstanding.store(false, std::memory_order_release);
        sLog_Warning("Main-thread queue-latency probe post failed");
        return false;
    }
    return true;
}

void EventLoopLagSampler::recordQueueLatency(Clock::time_point sent, Clock::time_point delivered) {
    const double lagMs = std::max(0.0, std::chrono::duration<double, std::milli>(delivered - sent).count());
    const auto atMs = std::chrono::duration_cast<std::chrono::milliseconds>(delivered.time_since_epoch()).count();
    m_ring[m_next] = {atMs, float(lagMs)};
    m_next = (m_next + 1) % m_ring.size();
    m_size = std::min(m_size + 1, m_ring.size());
    if (lagMs > kLateMs && m_late) m_late->inc();
}

double EventLoopLagSampler::quantileMs(double q) {
    return quantileMsAt(q, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
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
                               "Main-thread queued probe calls that waited more than 100 ms.");
    m_skipped = &registry.counter("sentinel_server_event_loop_skipped_ticks_total",
                                  "Probe wakes skipped because the previous main-thread call is still outstanding.");
    registry.familyFn("sentinel_server_event_loop_lag_ms",
                      "Main-thread queue latency in ms over the last 60 s, sampled by a queued call every 100 ms "
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
