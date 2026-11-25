#pragma once
// One process-wide scheduler on its own event-loop thread. All methods are
// data-thread-only; callers queue wants/releases. Transport is queued both ways.
// The cache is independently thread-safe. No GUI or live ownership logic here.
#include "FlowChunkCache.hpp"
#include "FlowChunkPlan.hpp"
#include "protocol/ChunkClientTransport.hpp"
#include <QTimer>
namespace sentinel::flow {
class FlowChunkFetcher final : public QObject {
    Q_OBJECT
public:
    using Key = protocol::chunks::Key;
    using ChartId = quint64;
    static constexpr size_t MaxChunks = 8, MaxBytes = 8ull << 20, PaddingBytes = 32ull << 20;
    struct Options {
        std::function<int64_t()> nowMs;
        std::function<size_t(const Key&)> estimateBytes;
        int retryBaseMs = 100, retryMaxMs = 5000, timeoutMs = 30000;
    };
    FlowChunkFetcher(FlowChunkCache&, protocol::chunks::Transport&, QObject* parent = nullptr);
    FlowChunkFetcher(FlowChunkCache&, protocol::chunks::Transport&, Options, QObject* parent = nullptr);
    ~FlowChunkFetcher() override;
    // Full replacement of this chart's previous interest. Returns false without
    // changing it for invalid input or a ninth distinct instrument.
    bool want(ChartId, FlowChunkWant);
    void release(ChartId);
    void hostChanged();
    void pump();
    std::optional<protocol::chunks::Availability> availability(const std::string&) const;
    struct Stats {
        uint64_t requested = 0, cacheHits = 0, bodies = 0, notModified = 0, retries = 0, rejected = 0;
        size_t wanted = 0, inFlightChunks = 0, inFlightBytes = 0;
    };
    Stats stats() const { return stats_; }
signals:
    void chunkStored(protocol::chunks::Key key);
    void chunkRevised(protocol::chunks::Key key);
    void chunkHidden(protocol::chunks::Key key);
    void chunkFailed(protocol::chunks::Key key, QString code, QString message);
    void availabilityChanged(protocol::chunks::Availability value);
    void storeCleared();
private:
    struct Demand {
        int tier = 0; uint64_t order = 0;
        bool wanted = false, failed = false;
        quint64 request = 0;
        int64_t due = 0;
        unsigned attempts = 0;
        size_t bytes = 0;
        std::optional<FlowChunkCache::Entry> held;
    };
    struct Request { std::vector<Key> keys; int64_t start = 0, deadline = 0; };
    FlowChunkCache& cache_;
    protocol::chunks::Transport& transport_;
    Options options_;
    QTimer* timer_;
    std::map<ChartId, FlowChunkWant> charts_;
    std::map<std::string, protocol::chunks::Availability> availability_;
    std::map<Key, Demand> demands_;
    std::map<quint64, Request> requests_;
    std::set<Key> unavailable_;
    Stats stats_;
    bool online_ = false, scheduled_ = false;
    std::optional<std::vector<std::string>> subscribed_;
    struct Burst { uint64_t requested = 0, hits = 0, bytes = 0; int64_t rtt = 0, lastLog = -1000; } burst_;
    size_t estimate(const Key&) const;
    void schedule();
    void replan();
    void subscribe(bool force = false);
    void down();
    void onAvailability(protocol::chunks::Availability);
    void received(quint64, protocol::chunks::ReplyPtr);
    void failed(protocol::chunks::Failure);
    void finish(const Key&);
    bool current(const Key&) const;
    void probe();
};
}
