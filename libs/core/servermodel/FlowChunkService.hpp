#pragma once
// Finished SCK1 frames only. Store fences always precede cache lookup and socket
// admission. Cache locks never call the store; lock order is store then cache.
#include "FlowChunkStore.hpp"
#include "protocol/ChunkFrame.hpp"
#include <list>
#include <tuple>

namespace recording {
class FlowChunkService {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    using Key = protocol::chunks::Key;
    static constexpr size_t DefaultCacheBytes = 64u * 1024u * 1024u;
    struct Reply {
        Bytes bytes;
        std::optional<FlowChunkStore::Stamp> stamp;
    };
    struct Stats { uint64_t hits = 0, misses = 0, bytes = 0, evictions = 0; };
    // Store and its source owner outlive this service and all history jobs.
    explicit FlowChunkService(FlowChunkStore&, size_t cacheBytes = DefaultCacheBytes,
                              sentinel::metrics::MetricsRegistry* metrics = nullptr);
    Reply serve(const Key&, std::optional<uint64_t> haveHash = {});
    // Called on the network executor. The action only admits already-built bytes
    // to the queue; the store holds its fence until admission is complete.
    bool admit(const Key&, const Reply&, const std::function<void()>& action) const;
    bool servable(const Key& key) const { return bool(store_.stamp(key.instrument, key.levelMs, key.startMs)); }
    static Bytes error(const Key&, protocol::chunks::Error, std::string message);
    // Concurrent subscribers share one scan per instrument generation.
    std::optional<FlowChunkStore::Snapshot> availability(const std::string& id);
    auto fingerprint(const std::string& id) const { return store_.fingerprint(id); }
    Stats stats() const;
    std::function<void(protocol::chunks::Frame&)> beforeEncodeForTest;
private:
    struct Cache;
    FlowChunkStore& store_;
    std::shared_ptr<Cache> cache_;
};
} // namespace recording
