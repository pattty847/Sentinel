#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sentinel::metrics { class MetricsRegistry; }

namespace sentinel::capture {
class QueuePool;
class Session;

// One product's feed state for /metrics, written by the ingest observer on the
// mdc-io thread and read by samplers on the main thread. Up/down and the down
// time are ONE atomic (kUp, or the steady ns the feed went down), so a sample
// can never pair "down" with a stale timestamp from an older outage.
struct FeedMetrics {
    static constexpr int64_t kUp = INT64_MIN;
    std::atomic<uint64_t> connection{0};
    std::atomic<int64_t> downSinceSteadyNs; // kUp while up
    explicit FeedMetrics(int64_t startedSteadyNs) : downSinceSteadyNs(startedSteadyNs) {}
    // Transport up/down as observed by the capture. A repeated down keeps the
    // first down time; a down while already down changes nothing.
    void transport(bool isUp, uint64_t connectionId, int64_t steadyNs) noexcept {
        connection.store(connectionId, std::memory_order_relaxed);
        if (isUp) { downSinceSteadyNs.store(kUp, std::memory_order_relaxed); return; }
        int64_t expected = kUp;
        downSinceSteadyNs.compare_exchange_strong(expected, steadyNs, std::memory_order_relaxed);
    }
    bool up() const noexcept { return downSinceSteadyNs.load(std::memory_order_relaxed) == kUp; }
    // Seconds down at nowSteadyNs (0 while up), from one load.
    double downSeconds(int64_t nowSteadyNs) const noexcept {
        const auto since = downSinceSteadyNs.load(std::memory_order_relaxed);
        return since == kUp || nowSteadyNs <= since ? 0.0 : double(nowSteadyNs - since) / 1e9;
    }
};
struct CaptureMetricsSource {
    std::string product;
    const FeedMetrics* feed;
    const Session* session;
};
// Registers the sentinel-capture series (ops/monitoring/README.md):
//   sentinel_capture_feed_up{product}               1 while the product's WebSocket is up
//   sentinel_capture_feed_down_seconds{product}     seconds down (0 while up; since start if never up)
//   sentinel_capture_connection{product}            established connection id (reconnects = id - 1)
//   sentinel_capture_queue_bytes{product}           bytes queued for the disk worker
//   sentinel_capture_queue_pool_bytes / _floor_bytes  shared pool total and per-product floor
//   sentinel_capture_stored_frames_total{product}   frames the disk worker wrote
//   sentinel_capture_file_bytes_total{product}      bytes written to RAWL2 files
// Every sampler reads atomics only (INV-094): never a session mutex, never a
// call into the I/O thread. Sources, pool and registry must outlive rendering.
void registerCaptureMetrics(metrics::MetricsRegistry& registry, std::shared_ptr<const QueuePool> pool,
                            std::vector<CaptureMetricsSource> sources, std::function<int64_t()> steadyNowNs = {});
} // namespace sentinel::capture
