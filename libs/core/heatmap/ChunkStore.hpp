#pragma once
// Process-wide cache of decoded native heatmap chunks (plan S5, measured by B1).
// One immutable SparseColumns per ChunkKey, shared by every chart, timeframe and
// preparation mode, bounded by one global byte budget (LRU).
// - Concurrent get() calls for the same key share a single load: no chart ever
//   decodes a chunk that another chart is already decoding (no redundant decode).
// - An open (unsealed) chunk can be replaced by a newer revision (revise()). Each
//   stored version gets a process-unique generation; anything derived from a
//   chunk (composed tiles, render-ready tiles, GPU sources) records the
//   generations it was built from and is stale once any of them changes.
// - Holders of an older version keep it alive through their shared_ptr; the
//   budget counts only what the store itself retains.
// Thread-safe. No Qt.
#include "ChunkCodec.hpp"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace heatmap {

// Native chunk spans (S2): minute chunks span one UTC hour; deep hour chunks one UTC day.
inline int64_t chunkSpanMs(int64_t levelMs) {
    return levelMs == kMinuteMs ? kHourMs : levelMs == kHourMs ? kDayMs : 0;
}
// Approximate resident bytes of decoded columns (vectors' element storage and
// per-object overhead; allocator slack not included).
size_t sparseBytes(const SparseColumns &columns);

struct ChunkKeyHash {
    size_t operator()(const ChunkKey &key) const;
};

struct StoredChunk {
    ChunkKey key;
    std::shared_ptr<const SparseColumns> columns;
    bool sealed = false;
    uint64_t revision = 0;   // caller-defined (wire revision of an open chunk)
    uint64_t generation = 0; // process-unique per stored version
    size_t bytes = 0;
};

class ChunkStore {
public:
    struct Loaded {
        SparseColumns columns;
        bool sealed = true;
        uint64_t revision = 0;
    };
    // Called outside the store lock, on the requesting thread. Throws on failure.
    using Loader = std::function<Loaded(const ChunkKey &)>;
    ChunkStore(size_t maxBytes, Loader loader);

    // Cached chunk, or loads it (blocking). Validates loaded columns once.
    // Throws what the loader throws (waiters of a shared load rethrow it).
    std::shared_ptr<const StoredChunk> get(const ChunkKey &key);
    // Cached chunk or nullptr; never loads. Counts as a hit when found.
    std::shared_ptr<const StoredChunk> peek(const ChunkKey &key);
    // Cached chunk or nullptr, without touching LRU order or stats (inspection).
    std::shared_ptr<const StoredChunk> cached(const ChunkKey &key) const;
    // Current generation of a cached key (0 when absent). Not counted in stats.
    uint64_t generationOf(const ChunkKey &key) const;
    bool contains(const ChunkKey &key) const { return generationOf(key) != 0; }
    // Stores a new version (an open chunk's revision, or a reload). Returns it.
    std::shared_ptr<const StoredChunk> revise(const ChunkKey &key, Loaded loaded);
    // Reloads through the loader and stores the result as a new version.
    std::shared_ptr<const StoredChunk> reload(const ChunkKey &key);

    struct Stats {
        uint64_t hits = 0, misses = 0;   // get/peek found / did not find it cached
        uint64_t loads = 0;              // loader calls (decodes)
        uint64_t sharedLoads = 0;        // get() calls that waited on another caller's load
        uint64_t evictions = 0, revisions = 0;
        size_t bytes = 0, entries = 0, maxBytes = 0;
        double loadMs = 0;               // summed loader wall time
    };
    Stats stats() const;
    void resetStats();
    void clear();
    void setMaxBytes(size_t bytes);

private:
    struct Entry {
        std::shared_ptr<const StoredChunk> chunk;
        std::list<ChunkKey>::iterator lru;
    };
    struct InFlight {
        bool done = false;
        std::shared_ptr<const StoredChunk> result;
        std::exception_ptr error;
    };
    mutable std::mutex mutex_;
    std::condition_variable loaded_;
    Loader loader_;
    size_t maxBytes_ = 0, bytes_ = 0;
    std::list<ChunkKey> lru_; // front = most recent
    std::unordered_map<ChunkKey, Entry, ChunkKeyHash> entries_;
    std::unordered_map<ChunkKey, std::shared_ptr<InFlight>, ChunkKeyHash> inFlight_;
    Stats stats_;
    std::shared_ptr<const StoredChunk> insertLocked(const ChunkKey &key, Loaded loaded);
    void evictLocked();
};
} // namespace heatmap
