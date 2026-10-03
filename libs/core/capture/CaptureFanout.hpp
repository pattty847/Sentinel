#pragma once
#include "RawCapture.hpp"
#include <array>
#include <atomic>

namespace sentinel::metrics { class MetricsRegistry; }
namespace sentinel::capture {
// Slice C merge seam: same coordinates as roller::JournalPos; no roller dependency.
struct JournalPosition {
    std::string product, runId;
    uint64_t block = 0;
    uint32_t record = 0;
    bool operator==(const JournalPosition&) const = default;
};
nlohmann::json positionJson(const JournalPosition&);
JournalPosition parsePosition(const nlohmann::json&);
struct FanoutConfig {
    QString socketPath; // empty -> ~/Sentinel-runtime/run/capture.sock
    size_t ringBytes = 32 * 1024 * 1024;
    size_t clientBytes = 16 * 1024 * 1024;
    size_t ingressBytes = 32 * 1024 * 1024; // each product, outside QueuePool
    std::chrono::milliseconds retention{60000}, resnapshotInterval{20000};
    std::function<bool()> controlReady; // false during capture startup/shutdown
    std::function<int64_t()> nowNs; // deterministic monotonic-clock seam
};
// Canonicalizes before creation; refuses volumes, checkout trees, symlink
// socket/parent, non-private existing parent and live/stale non-socket files.
QString prepareFanoutPath(const QString&);
class CaptureFanout {
public:
    static constexpr size_t MaxClients = 8;
    CaptureFanout(FanoutConfig, const std::vector<std::string>& products,
                  metrics::MetricsRegistry&, std::function<void(const std::string&)> resnapshot);
    ~CaptureFanout();
    CaptureFanout(const CaptureFanout&) = delete;
    CaptureFanout& operator=(const CaptureFanout&) = delete;
    // One disk producer per product. Never waits on worker/client locks, never
    // throws into the writer. Overflow invalidates this product's socket stream.
    void publish(size_t product, const JournalEvent&) noexcept;
    void stop(); // after joining writers; bounded even with blocked clients
    QString path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace sentinel::capture
