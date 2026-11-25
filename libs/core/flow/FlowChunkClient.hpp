#pragma once
// Process owner creates one service, before connecting the stream client, and
// destroys chart consumers before it. Construction and teardown are synchronous;
// ordinary work is queued. No app instance is installed by core itself.
#include "FlowChunkFetcher.hpp"
#include <QThread>
namespace sentinel::flow {
class FlowChunkClient {
public:
    using Factory = std::function<protocol::chunks::Transport*(QObject*)>;
    explicit FlowChunkClient(Factory factory, size_t budget = 256ull << 20);
    ~FlowChunkClient();
    FlowChunkClient(const FlowChunkClient&) = delete;
    FlowChunkClient& operator=(const FlowChunkClient&) = delete;
    FlowChunkCache& cache() { return cache_; }
    // Signal connection only from other threads. Use queued() for method calls.
    FlowChunkFetcher& fetcher() { return *fetcher_; }
    void queued(std::function<void(FlowChunkFetcher&)> work);
private:
    FlowChunkCache cache_;
    QThread thread_;
    QObject* context_ = nullptr;
    protocol::chunks::Transport* transport_ = nullptr;
    FlowChunkFetcher* fetcher_ = nullptr;
};
}
