#pragma once
// GUI-owned process service. All transport/cache/controller work runs on the
// heatmap-data thread. Construct before connecting an external stream client.
#include "HeatmapSourceController.hpp"
#include "heatmap/ChunkFetcher.hpp"
#include <QThread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace heatmap {
class HeatmapDataService {
public:
    // Called on the data thread. Return a newly allocated transport; ancillary
    // QObjects may be parented to context and outlive the transport at shutdown.
    using TransportFactory = std::function<ChunkTransport *(QObject *context)>;
    using StartTransport = std::function<void(ChunkTransport &)>;
    explicit HeatmapDataService(TransportFactory factory, HeatmapBudgets budgets = {},
                                StartTransport start = {}, std::function<void()> beforeStop = {});
    ~HeatmapDataService();
    HeatmapDataService(const HeatmapDataService &) = delete;
    HeatmapDataService &operator=(const HeatmapDataService &) = delete;

    HeatmapSourceController *createController(size_t gpuBytes);
    void destroyController(HeatmapSourceController *controller);
    size_t controllerCount() const;
    ChunkStore &store() { return store_; }
    ChunkFetcher *fetcher() const { return fetcher_; }
    SpanSourceCache *cache() const { return cache_; }
    QThread *thread() const { return thread_.get(); }
    bool connected() const { return connected_.load(); }
    std::optional<ChunkAvailability> availability(const std::string &symbol) const;
    struct Stats {
        ChunkStore::Stats store;
        ChunkFetcher::Stats fetcher;
        SpanSourceCache::Stats cache;
        size_t committedCpuBytes = 0;
    };
    Stats stats() const;
    void refreshStats();
    bool setBudgets(const HeatmapBudgets &budgets);
    // Setup/bench only; never call from the render thread. Blocking, or inline
    // when already on the data thread. No GUI callbacks from inside work.
    void onData(std::function<void()> work) const;
private:
    void destroyData(); // data thread, including partial construction
    void stopThread();
    std::function<void()> beforeStop_;
    ChunkStore store_;
    std::unique_ptr<QThread> thread_;
    QObject *context_ = nullptr;
    ChunkTransport *transport_ = nullptr;
    ChunkFetcher *fetcher_ = nullptr;
    SpanSourceCache *cache_ = nullptr;
    QTimer *statsTimer_ = nullptr;
    std::set<HeatmapSourceController *> controllers_; // data thread only
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    std::map<std::string, ChunkAvailability> availability_;
    Stats stats_;
};
} // namespace heatmap
