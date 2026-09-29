#pragma once
#include "Hmc2Store.hpp"
#include "../heatmap/ChunkCodec.hpp"
#include <list>
#include <optional>
#include <unordered_map>

namespace recording {
using ChunkKey = heatmap::ChunkKey;
using ChunkState = heatmap::ChunkState;
// Only native levels are chunks. Minute chunks span a UTC hour; deep hour
// chunks span a UTC day. The start must be aligned to that span.
int64_t chunkEndMs(const ChunkKey& key);
ChunkState chunkState(const ChunkKey& key, int64_t committedThroughMs,
                      uint64_t revision, int64_t latenessMs);
// committedThroughMs limits the proven scan to completed native buckets for an
// open chunk. Omit it only when the caller knows the entire chunk is committed.
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key,
                                  std::optional<int64_t> committedThroughMs = {});

// Worker-owned cache of exact encoded sealed frames. Open frames are refused.
class EncodedChunkLru {
public:
    explicit EncodedChunkLru(size_t maxBytes) : maxBytes_(maxBytes) {}
    void put(const heatmap::ChunkFrame& frame, std::vector<uint8_t> encoded);
    std::optional<std::vector<uint8_t>> get(const ChunkKey& key);
    size_t bytes() const { return bytes_; }
    size_t size() const { return entries_.size(); }
private:
    struct Entry { ChunkKey key; std::vector<uint8_t> wire; };
    struct Hash {
        size_t operator()(const ChunkKey& key) const;
    };
    size_t maxBytes_ = 0, bytes_ = 0;
    std::list<Entry> lru_;
    std::unordered_map<ChunkKey, std::list<Entry>::iterator, Hash> entries_;
};
} // namespace recording
