#pragma once
// Process-wide cache of decoded native heatmap chunks (plan S5, measured by B1).
// One immutable SparseColumns per ChunkKey, shared by every chart and timeframe,
// bounded by one global byte budget (LRU). Bodies arrive through put() (the
// ChunkFetcher dedups requests across charts, so no chunk is decoded twice).
// - Each stored version gets a process-unique generation; anything derived from
//   a chunk (span sources) records the generations it was built from and is
//   stale once any differs from generationOf(). The latest generation of a key
//   survives eviction, so "evicted" never reads as "unchanged" after a revision;
//   an equal content hash keeps the generation (a sealed chunk stored again after
//   eviction is not a revision).
// - Ordering: every put() takes a ticket when it starts. A completion never
//   replaces a version whose ticket is newer.
// - A listener is told about every revision (a put that stored a new generation
//   of a key already known), after the store lock is released.
// - Holders of an older version keep it alive through their shared_ptr; the
//   budget counts only what the store itself retains.
// - Wanted keys (setWanted, maintained by ChunkFetcher for keys some chart wants)
//   are never evicted, even a single chunk larger than the whole budget: the byte
//   cap evicts unwanted chunks only. Otherwise a working set above the budget
//   would evict a body before its span builds and refetch it forever. Their bytes
//   still count (Stats::wantedBytes), so an over-budget store is visible.
// Thread-safe. No Qt.
#include "ChunkCodec.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heatmap {

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
    int64_t committedThroughMs = 0;
    uint64_t contentHash = 0; // wire identity; legacy loads do not supply a hash
    uint64_t revision = 0;   // caller-defined (wire revision of an open chunk)
    uint64_t generation = 0; // process-unique per stored version
    uint64_t ticket = 0;     // acquisition order (see Ordering above)
    size_t bytes = 0;
};

class ChunkStore {
public:
    explicit ChunkStore(size_t maxBytes = 512ull << 20);

    // Already decoded, immutable columns (may alias a ChunkFrame). No copy.
    // Equal hashes keep the generation, including after eviction; sealing is
    // one revision even if the caller supplies the same hash. Older states lose.
    // Never retains more unwanted bytes than the store budget.
    std::shared_ptr<const StoredChunk> put(const ChunkKey &key, std::shared_ptr<const SparseColumns> columns,
                                            ChunkState state, uint64_t contentHash);

    // Cached chunk or nullptr. Counts as a hit when found.
    std::shared_ptr<const StoredChunk> peek(const ChunkKey &key);
    // Cached chunk or nullptr, without touching LRU order or stats (inspection).
    std::shared_ptr<const StoredChunk> cached(const ChunkKey &key) const;
    // Latest generation ever stored for the key, cached or evicted (0 = never
    // stored since the last clear()). Not counted in stats.
    uint64_t generationOf(const ChunkKey &key) const;
    bool contains(const ChunkKey &key) const { return cached(key) != nullptr; }
    // Marks a key as wanted (exempt from eviction) or not. Not cleared by clear().
    void setWanted(const ChunkKey &key, bool wanted);
    // Called after every put that stored a new generation of a known key, on the
    // calling thread, outside the store lock. Not called for first stores or an
    // unchanged body stored again.
    void setRevisionListener(std::function<void(const ChunkKey &)> listener);
    uint64_t revisionCount() const;

    struct Stats {
        uint64_t hits = 0, misses = 0;   // peek found / did not find it cached
        uint64_t loads = 0;              // accepted put bodies
        uint64_t evictions = 0, revisions = 0;
        size_t bytes = 0, entries = 0, maxBytes = 0;
        size_t wantedBytes = 0;          // retained bytes of wanted keys (may exceed maxBytes)
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
    mutable std::mutex mutex_;
    size_t maxBytes_ = 0, bytes_ = 0;
    std::list<ChunkKey> lru_; // front = most recent
    std::unordered_map<ChunkKey, Entry, ChunkKeyHash> entries_;
    std::unordered_set<ChunkKey, ChunkKeyHash> wanted_;
    struct Latest {
        uint64_t generation = 0, ticket = 0;
        bool sealed = false;
        std::optional<uint64_t> contentHash;
        uint64_t revision = 0;
        int64_t committedThroughMs = 0;
    };
    std::unordered_map<ChunkKey, Latest, ChunkKeyHash> latest_; // survives eviction
    uint64_t nextTicket_ = 0;
    std::function<void(const ChunkKey &)> listener_;
    std::atomic<uint64_t> revisionCount_{0};
    Stats stats_;
    void notify(const ChunkKey &key);
    // Stores unless a version with a newer ticket exists (then returns that one).
    // *stored tells which happened.
    std::shared_ptr<const StoredChunk> insertSharedLocked(const ChunkKey &key,
        std::shared_ptr<const SparseColumns> columns, ChunkState state, std::optional<uint64_t> hash,
        uint64_t ticket, bool *stored);
    std::shared_ptr<const StoredChunk> cachedLocked(const ChunkKey &key) const;
    void evictLocked();
};
} // namespace heatmap
