#pragma once
// Lab-only configurator for the shared HeatmapDataService: local read-only HMC2
// or a reconnecting SentinelStreamClient, plus bench helpers. Cache, controller,
// thread and stats lifetimes are implemented by the production service.
// GUI thread API unless noted; controllers receive queued calls.
#include "render/heatmap/HeatmapDataService.hpp"
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lab {
inline constexpr const char *kSymbol = "BTC-USD";

// Process physical footprint (macOS task_vm_info; includes GPU memory on unified memory), bytes.
uint64_t processFootprintBytes();

class LabData {
public:
    // The process-wide instance, created on first use with the recording root
    // (recordingRoot() unless setRoot) and the pinned end. It shuts down (on the
    // data thread) when the QCoreApplication is destroyed.
    static LabData &instance();
    // Restart the whole data path on another recording root / pinned end (UTC ms,
    // exclusive; 0 = live). Empty root: recordingRoot(). Every controller must
    // have been destroyed first. Clears every cache.
    static void configure(const std::string &root, int64_t pinnedEndMs);
    static std::string root();
    static int64_t pinnedEndMs();
    // Server mode: chunks and the live edge from sentinel-server at host:port
    // (TLS, caFile verifies it). Like configure(), only before charts exist.
    struct Server {
        std::string host, port, caFile;
    };
    // nullopt: back to the local recording.
    static void configureServer(const std::optional<Server> &server);
    // Tests: emit the client's connected() inside the next shutdown, right before
    // the data path is deleted (its queued delivery must never reach a dead client);
    // and the connection callbacks that ran after the teardown had started.
    static void queueConnectedOnShutdownForTest(bool queue);
    static int lateConnectionCallbacksForTest();
    static std::optional<Server> server();
    enum class Connection { Local, Connecting, Connected, Disconnected };
    Connection connection() const { return connection_.load(); }
    QString connectionText() const;

    heatmap::HeatmapDataService &service() { return *service_; }
    heatmap::ChunkStore &store() { return service_->store(); }
    QThread *thread() const { return service_->thread(); }
    heatmap::ChunkFetcher *fetcher() const { return service_->fetcher(); }
    heatmap::SpanSourceCache *cache() const { return service_->cache(); }

    // A controller living on the data thread (blocking construction there), and
    // its destruction there. gpuBytes: the per-chart GPU cap.
    heatmap::HeatmapSourceController *createController(size_t gpuBytes);
    void destroyController(heatmap::HeatmapSourceController *controller);

    // Latest availability of kSymbol (updated on the data thread).
    std::optional<heatmap::ChunkAvailability> availability() const;
    // Every cache emptied: the next views decode and build from scratch (bench cold runs).
    void clearCaches();
    // A new generation of each source's newest stored minute chunk, as if the
    // server had sent a revision (bench and tests); every chart hears it.
    // Returns the revised chunks with their new generations.
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> reviseNewestChunks();

    using Stats = heatmap::HeatmapDataService::Stats;
    // Refreshed on the data thread every 250 ms and after clearCaches().
    Stats stats() const;
    void refreshStats(); // blocking: now

    // Median best-bid/best-ask midpoint of the newest recorded minutes, read
    // directly from the recording (bench session views). 0 when none.
    double recentMidPrice() const;

    ~LabData();

private:
    LabData(std::string root, int64_t pinnedEndMs, std::optional<Server> server);
    void start();
    void shutdown();
    std::string root_;
    int64_t pinnedEndMs_ = 0;
    std::optional<Server> server_;
    std::unique_ptr<heatmap::HeatmapDataService> service_;
    QObject *client_ = nullptr;                     // SentinelStreamClient (server mode)
    QTimer *reconnectTimer_ = nullptr;
    std::atomic<Connection> connection_{Connection::Local};
    std::atomic<bool> tearingDown_{false};
    heatmap::ChunkTransport *createServerTransport(QObject *context);
};
} // namespace lab
