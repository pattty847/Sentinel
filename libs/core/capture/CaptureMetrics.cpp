#include "CaptureMetrics.hpp"
#include "CaptureSession.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <algorithm>
#include <chrono>
#include <optional>

namespace sentinel::capture {
void registerCaptureMetrics(metrics::MetricsRegistry& r, std::shared_ptr<const QueuePool> pool,
                            std::vector<CaptureMetricsSource> sources, std::function<int64_t()> steadyNowNs) {
    using Value = std::optional<double>;
    if (!steadyNowNs) steadyNowNs = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    r.gauge("sentinel_capture_queue_pool_bytes", "Shared capture queue pool (--queue-mib).").set(double(pool->total()));
    r.gauge("sentinel_capture_queue_floor_bytes", "Per-product floor inside the pool (--queue-floor-mib).").set(double(pool->floor()));
    r.gaugeFn("sentinel_capture_queue_used_bytes", "Bytes queued for disk across all products.", {},
              [pool]() -> Value { return double(pool->used()); });
    for (const auto& source : sources) {
        const metrics::Labels labels{{"product", source.product}};
        const auto* feed = source.feed;
        const auto* session = source.session;
        r.gaugeFn("sentinel_capture_feed_up", "1 while this product's upstream WebSocket is up.", labels,
                  [feed]() -> Value { return feed->up() ? 1.0 : 0.0; });
        r.gaugeFn("sentinel_capture_feed_down_seconds",
                  "Seconds this product's feed has been down (0 while up; since process start if never up).", labels,
                  [feed, steadyNowNs]() -> Value { return feed->downSeconds(steadyNowNs()); });
        r.gaugeFn("sentinel_capture_connection", "Established connection id of this product (reconnects = id - 1).", labels,
                  [feed]() -> Value { return double(feed->connection.load(std::memory_order_relaxed)); });
        r.gaugeFn("sentinel_capture_queue_bytes", "Bytes this product has queued for the disk worker.", labels,
                  [session]() -> Value { return double(session->queuedBytes()); });
        r.counterFn("sentinel_capture_stored_frames_total", "WebSocket frames written to this product's RAWL2 files.", labels,
                    [session]() -> Value { return double(session->storedFrames()); });
        r.counterFn("sentinel_capture_file_bytes_total", "Bytes written to this product's RAWL2 files.", labels,
                    [session]() -> Value { return double(session->storedFileBytes()); });
    }
}
} // namespace sentinel::capture
