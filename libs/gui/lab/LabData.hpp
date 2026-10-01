#pragma once
// The lab's heatmap data path: the production S5 components, as the main chart
// will use them in S6 (plan docs/research/2026-09-s5-plan.md section 2).
// One per process: a "heatmap-data" QThread hosting the ChunkFetcher, the
// transport, the SpanSourceCache and every chart's HeatmapSourceController; one
// ChunkStore shared by all of them. The transport is LocalChunkTransport (HMC2
// recording, read-only; the default and the pinned bench) or, in server mode
// (S5L-c), SentinelStreamClientTransport against a running sentinel-server, so
// the live edge (heatmap_live_subscribe, SHC1 kind 2) is real.
// GUI thread API unless noted. Charts create their controller here and talk to
// it with queued calls; they read snapshots and capacity from any thread.
#include "heatmap/ChunkFetcher.hpp"
#include "heatmap/LocalChunkTransport.hpp"
#include "render/heatmap/HeatmapSourceController.hpp"
#include <QThread>
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
    static void configureServer(const Server &server);
    static std::optional<Server> server();
    enum class Connection { Local, Connecting, Connected, Disconnected };
    Connection connection() const { return connection_.load(); }
    QString connectionText() const;

    heatmap::ChunkStore &store() { return store_; }
    QThread *thread() const { return thread_.get(); }
    heatmap::ChunkFetcher *fetcher() const { return fetcher_; }
    heatmap::SpanSourceCache *cache() const { return cache_; }

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

    struct Stats {
        heatmap::ChunkStore::Stats store;
        heatmap::ChunkFetcher::Stats fetcher;
        heatmap::SpanSourceCache::Stats cache;
        size_t committedCpuBytes = 0;
    };
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
    heatmap::ChunkStore store_;
    std::unique_ptr<QThread> thread_;
    heatmap::ChunkTransport *transport_ = nullptr; // data thread objects
    QObject *client_ = nullptr;                     // SentinelStreamClient (server mode)
    QTimer *reconnectTimer_ = nullptr;
    std::atomic<Connection> connection_{Connection::Local};
    void startServerTransport();
    heatmap::ChunkFetcher *fetcher_ = nullptr;
    heatmap::SpanSourceCache *cache_ = nullptr;
    QObject *context_ = nullptr; // data-thread context for timers and relays
    QTimer *statsTimer_ = nullptr;
    mutable std::mutex mutex_;
    std::optional<heatmap::ChunkAvailability> availability_;
    Stats stats_;
    int controllers_ = 0;
};
} // namespace lab
