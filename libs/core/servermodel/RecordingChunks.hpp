#pragma once
#include "Hmc2Store.hpp"
#include "BookRecorder.hpp"
#include "../heatmap/ChunkCodec.hpp"
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace recording {
using ChunkKey = heatmap::ChunkKey;
using ChunkState = heatmap::ChunkState;
// Only native levels are chunks. Minute chunks span a UTC hour; hour chunks
// (hmc2.deep only) span a UTC day. The start must be aligned to that span.
int64_t chunkEndMs(const ChunkKey& key);
// Migration-only mapping from an hmc2.* source id to its HMC2 layer name.
std::string hmc2Layer(const ChunkKey& key);
// Sealed states report committedThroughMs == chunk end and revision 0.
ChunkState chunkState(const ChunkKey& key, const BookRecorder::Watermarks& watermarks,
                      uint64_t revision);
// Omit watermarks only when the caller knows the entire chunk is committed.
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key);
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key,
                                  const BookRecorder::Watermarks& watermarks);

// Cancellable worker read. A cancelled/partial scan throws; no partial chunk is published.
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key,
                                  const BookRecorder::Watermarks& watermarks, ReadControl& control);

// Shared cache of exact encoded sealed SHC1 bytes. Open frames are refused.
class EncodedChunkLru {
public:
    explicit EncodedChunkLru(size_t maxBytes) : maxBytes_(maxBytes) {}
    void put(std::vector<uint8_t> encoded);
    std::shared_ptr<const std::vector<uint8_t>> get(const ChunkKey& key);
    size_t bytes() const;
    size_t size() const;
private:
    struct Entry { ChunkKey key; std::shared_ptr<const std::vector<uint8_t>> wire; };
    struct Hash {
        size_t operator()(const ChunkKey& key) const;
    };
    mutable std::mutex mutex_;
    size_t maxBytes_ = 0, bytes_ = 0;
    std::list<Entry> lru_;
    std::unordered_map<ChunkKey, std::list<Entry>::iterator, Hash> entries_;
};
} // namespace recording
