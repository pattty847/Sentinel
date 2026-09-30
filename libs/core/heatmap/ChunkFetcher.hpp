#pragma once
#include "ChunkStore.hpp"
#include "ChunkTransport.hpp"
#include <QTimer>
#include <map>
#include <unordered_set>

namespace heatmap {
// One instance and one ChunkStore per process. All methods, except the store's
// revision callback, belong to its event-loop thread. Move it and its transport
// to the heatmap-data QThread before use; charts queue calls onto that thread.
// The store and transport must outlive it. It owns the store's revision listener.
class ChunkFetcher final : public QObject {
    Q_OBJECT
public:
    using ChartId = quint64;
    static constexpr size_t kMaxInFlightChunks = 4;
    static constexpr size_t kMaxInFlightBytes = 16ull << 20;
    struct Options {
        // Cold estimate is 4 MiB; warm estimates use decoded bytes. Estimates
        // saturate at 16 MiB so a large single chunk can still make progress.
        std::function<size_t(const ChunkKey &)> estimateBytes;
        std::function<int64_t()> nowMs; // monotonic; injectable for retry tests
        int retryBaseMs = 100, retryMaxMs = 5000;
        int requestTimeoutMs = 30'000; // deadline from request admission, not last reply
    };
    ChunkFetcher(ChunkStore &store, ChunkTransport &transport, QObject *parent = nullptr);
    ChunkFetcher(ChunkStore &store, ChunkTransport &transport, Options options, QObject *parent = nullptr);
    ~ChunkFetcher() override;
    // Add/update interests; greater priority wins, then first-wanted order.
    // A current cache hit emits nothing. Eviction is not signalled: controllers
    // peek the store when consuming a key and want() again after a cache miss.
    void want(ChartId chart, const std::vector<ChunkKey> &keys, int priority = 0);
    void release(ChartId chart, const std::vector<ChunkKey> &keys);
    void release(ChartId chart);
    void hostChanged(); // discard cache + old replies; wait for fresh availability
    void wireVersionMismatch();
    // Pump is normally scheduled automatically. Tests can advance nowMs then
    // call it to fire due retries without sleeping or altering retry policy.
    void pump();
    std::optional<ChunkAvailability> availability(const std::string &symbol) const;
    struct Stats {
        uint64_t requests = 0, requestedChunks = 0, bodies = 0, notModified = 0, retries = 0;
        size_t inFlightChunks = 0, inFlightBytes = 0;
    };
    Stats stats() const { return stats_; }
signals:
    void chunkStored(heatmap::ChunkKey key, quint64 generation);
    void chunkRevised(heatmap::ChunkKey key, quint64 generation);
    void chunkFailed(heatmap::ChunkKey key, QString code, QString message);
    void availabilityChanged(heatmap::ChunkAvailability value);
    void storeCleared();
private:
    struct Demand {
        std::map<ChartId, int> charts;
        bool pending = false, refresh = false;
        quint64 request = 0;
        uint64_t order = 0;
        int64_t dueMs = 0;
        unsigned busyCount = 0;
        size_t estimate = 0;
        // Keep have_hash's body alive until NotModified (LRU may evict it).
        std::shared_ptr<const StoredChunk> held;
    };
    struct RevisionRelay;
    ChunkStore &store_;
    ChunkTransport &transport_;
    Options options_;
    QTimer *retry_;
    std::shared_ptr<RevisionRelay> relay_;
    std::unordered_map<ChunkKey, Demand, ChunkKeyHash> demands_;
    struct Request { std::vector<ChunkKey> keys; int64_t deadlineMs = 0; };
    std::unordered_map<quint64, Request> requests_;
    std::unordered_set<ChunkKey, ChunkKeyHash> current_;
    std::map<std::string, ChunkAvailability> availability_;
    Stats stats_;
    uint64_t order_ = 0;
    bool connected_ = false, compatible_ = true, scheduled_ = false;
    void schedule();
    void disconnected();
    void clear();
    void onAvailability(ChunkAvailability value);
    void onReceived(quint64 request, ChunkFramePtr frame);
    void onFailed(quint64 request, OptionalChunkKey key, const QString &code, const QString &message);
    void finish(const ChunkKey &key, Demand &demand);
    void prune(const ChunkKey &key);
    bool ready(const ChunkKey &key) const;
    int64_t committedThrough(const ChunkKey &key) const;
    size_t estimate(const ChunkKey &key) const;
};
} // namespace heatmap
