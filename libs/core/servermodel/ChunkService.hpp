#pragma once
#include "RecordingChunks.hpp"
#include "RollupStore.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace recording {
// Error reply for key; strings are truncated to the wire's 255-byte limit.
std::shared_ptr<const std::vector<uint8_t>> chunkErrorFrame(const ChunkKey& key, heatmap::ChunkError code,
                                                            std::string message);
// Serves SHC1 chunk replies (chunk / not_modified / error): minute chunks from
// HMC2 recordings, 1h and 1d chunks from the RollupStore (when one is attached).
// Thread-safe: serve() and availability() run on history workers, each with its
// own thread-local reader; the sealed-chunk LRU is shared and mutex-protected.
class ChunkService {
public:
    using WatermarkFn = std::function<BookRecorder::Watermarks(const std::string& symbol,
                                                               const std::string& layer)>;
    using ClockFn = std::function<int64_t()>; // wall clock, epoch ms
    static constexpr size_t kDefaultCacheBytes = 256u * 1024u * 1024u;
    ChunkService(std::filesystem::path root, WatermarkFn watermarks, ClockFn now,
                 size_t cacheBytes = kDefaultCacheBytes);
    // Rollup levels are served only once a store is attached (before serving).
    void attachRollups(std::shared_ptr<RollupStore> rollups) { rollups_ = std::move(rollups); }
    const std::shared_ptr<RollupStore>& rollups() const { return rollups_; }

    // Never throws. Returns exact SHC1 bytes: the chunk, NotModified when
    // haveHash equals its content hash, or an Error frame for this key.
    std::shared_ptr<const std::vector<uint8_t>> serve(const ChunkKey& key, uint64_t haveHash);

    // The recorder's minute cutoff for a series it records in this process (the
    // hour field is not used: rollup levels come from minutes). A series with no
    // recorder watermark is not being written yet. With t = now minus a 5 minute
    // margin (a freshly started series publishes its first watermark within
    // about a minute): minutes before t's UTC minute are final.
    BookRecorder::Watermarks effectiveWatermarks(const std::string& symbol,
                                                 const std::string& hmc2Layer) const;

    struct LevelAvailability {
        int64_t levelMs = 0, chunkSpanMs = 0, committedThroughMs = 0;
        std::optional<int64_t> oldestMs, latestMs; // bucket starts on disk
        uint64_t contentEpoch = 0, contentGeneration = 0; // rollup levels: RollupStore::LevelRange
    };
    struct SourceAvailability {
        std::string id;
        std::vector<LevelAvailability> levels;
        std::optional<Hmc2Header> latestGrid; // native grid of the newest 1m record
    };
    // Sources with at least one persisted minute for symbol: the minute level,
    // then the rollup levels the store advertises. Throws on read failure.
    std::vector<SourceAvailability> availability(const std::string& symbol);
    // Cheap change detector for availability (recorder mutex and the rollup
    // generation only, no I/O).
    std::vector<int64_t> availabilityFingerprint(const std::string& symbol) const;

    EncodedChunkLru& cache() { return cache_; }

private:
    std::shared_ptr<const std::vector<uint8_t>> serveRollup(const ChunkKey& key, uint64_t haveHash);
    std::filesystem::path root_;
    WatermarkFn watermarks_;
    ClockFn now_;
    EncodedChunkLru cache_;
    std::shared_ptr<RollupStore> rollups_;
};
} // namespace recording
