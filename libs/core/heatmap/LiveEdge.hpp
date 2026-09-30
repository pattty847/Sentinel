#pragma once
#include "ChunkStore.hpp"
#include <map>

namespace heatmap {
// Immutable handoff to the build pool. The version is client-local and never
// rewinds; revision belongs to the subscription epoch (a restart may reset it).
struct LiveEdgeSnapshot {
    std::string symbol, source, layer;
    uint64_t version = 0, revision = 0;
    int64_t openEndMs = 0, committedThroughMs = 0;
    std::map<int64_t, std::shared_ptr<const SparseColumn>> minutes;
    std::vector<SparseColumns::TimeRange> proven;
};

// Owned by ChunkFetcher, exclusively on the heatmap-data thread. Live minutes
// never enter ChunkStore. A stored cutoff owns EVERY earlier minute, including
// gaps, and trimming happens only after the corresponding body is in the store.
class LiveEdge {
public:
    LiveEdge(std::string symbol, std::string source);
    void newEpoch(); // freeze the old snapshot until the first new-epoch frame
    bool accept(std::shared_ptr<const ChunkFrame> frame, const ChunkStore &store);
    bool trim(const ChunkStore &store);
    std::shared_ptr<const LiveEdgeSnapshot> snapshot() const { return snapshot_; }
private:
    std::shared_ptr<const LiveEdgeSnapshot> snapshot_;
    bool fresh_ = true;
};

// Worker-owned incremental composition. Cache each chunk's contribution per
// generation, and each finished output bucket by its immutable input pointers.
// A normal 1 Hz update changes only the forming bucket. Changed generations or
// late finals invalidate precisely the affected buckets. No Qt or GPU types.
class LiveComposer {
public:
    struct Result {
        SparseColumns columns;
        uint64_t composedBuckets = 0, committedPieces = 0;
    };
    Result compose(const LiveEdgeSnapshot &edge, const std::vector<std::shared_ptr<const StoredChunk>> &chunks,
                   int64_t tfMs, int64_t startMs);
    size_t bytes() const; // retained worker cache, for the process CPU ledger
private:
    struct Piece {
        std::shared_ptr<const SparseColumn> column;
        std::vector<SparseColumns::TimeRange> proven;
    };
    struct ChunkParts {
        uint64_t generation = 0;
        std::map<int64_t, Piece> buckets;
    };
    struct Bucket {
        std::vector<std::shared_ptr<const SparseColumn>> inputs;
        std::shared_ptr<const SparseColumn> column;
    };
    int64_t tfMs_ = 0;
    std::map<int64_t, ChunkParts> chunks_;
    std::map<int64_t, Bucket> buckets_, prefixes_;
};
} // namespace heatmap
