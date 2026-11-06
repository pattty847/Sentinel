#pragma once
// Stored 1h and 1d rollup levels of HMC2 minutes (docs/MARKETDATA.md, "Rollup
// levels"). A derived, rebuildable store owned by the server:
// - Each closed UTC day of each (symbol, layer) is composed from its minute
//   chunks with the production compose (heatmap::compose), the same picture as
//   composing minutes at view time, and written as two files: the 1h day chunk
//   and the day's 1d piece. Writes are atomic (temp file + rename).
// - A dedicated low-priority builder thread backfills missing closed days
//   newest first and builds each day once the recorder's minute watermark has
//   passed its end. A day whose minute files change is rebuilt.
// - serve() returns SHC1 chunk frames for 1h and 1d keys from these files. A
//   missing or corrupt file is served by composing the day's minutes on the
//   calling worker (correct, slower) and queued for rebuild.
// Deleting the store directory is safe: it is rebuilt.
#include "RecordingChunks.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <thread>

namespace recording {
// One closed UTC day of a series composed from its minutes: `hour` is the 1h
// level over [day, day + 1d); `day` the single 1d bucket over the same range.
struct DayRollup {
    heatmap::SparseColumns hour, day;
};
DayRollup composeDay(Hmc2Reader& reader, const std::string& symbol, const std::string& layer, int64_t dayMs,
                     ReadControl& control);
// The SHC1 frames a day contributes: the sealed 1h day chunk, and the day's 1d
// piece (an open frame of its kDayChunkDays chunk that scans only that day).
std::vector<uint8_t> encodeHourFrame(const std::string& symbol, const DayRollup& rollup, int64_t dayMs);
std::vector<uint8_t> encodeDayPiece(const std::string& symbol, const DayRollup& rollup, int64_t dayMs);

// File container: "SHR1", u16 version, u16 zero, u64 source fingerprint, u64
// frame length, then the SHC1 frame. Little-endian.
inline constexpr size_t kRollupHeaderBytes = 24;
std::vector<uint8_t> encodeRollupFile(uint64_t sourceFingerprint, std::span<const uint8_t> frame);
struct RollupFileHeader {
    uint64_t sourceFingerprint = 0, frameBytes = 0;
};
// Header and length check only (no frame decode); nullopt when missing or torn.
std::optional<RollupFileHeader> readRollupHeader(const std::filesystem::path& path);
// Full check: header, frame decode (content hash, structure) and its key.
// nullopt when missing or corrupt.
std::optional<std::vector<uint8_t>> readRollupFrame(const std::filesystem::path& path, const ChunkKey& expected);

// Identity of a series' recorded minute files for one UTC day: names, sizes and
// modification times of <hmc2Root>/<symbol>/<layer>-60000/<day>*.hmc2. Keyed by
// day start. A rollup built from other files is stale.
std::map<int64_t, uint64_t> minuteFingerprints(const std::filesystem::path& hmc2Root, const std::string& symbol,
                                               const std::string& layer);

// The store directory: `configured`, or "<hmc2Root>-rollups" when empty. Empty
// (refused) when it would be the HMC2 tree or inside it.
std::filesystem::path rollupRootFor(const std::string& configured, const std::filesystem::path& hmc2Root);

class RollupStore {
public:
    // The recorder's committed minute cutoff for a series (exclusive, epoch ms).
    using MinuteThroughFn = std::function<int64_t(const std::string& symbol, const std::string& layer)>;
    struct Options {
        std::filesystem::path hmc2Root, root;
        std::vector<std::string> symbols;
        MinuteThroughFn minuteThrough;
        std::chrono::milliseconds rescan{60'000};
        size_t cacheBytes = 128u * 1024u * 1024u;
        // False: a pass only indexes current files and builds nothing (read-only
        // lab use, LocalChunkTransport).
        bool buildMissing = true;
        // Test seam: runs after a temp file is written and synced, before rename.
        std::function<void(const std::filesystem::path& temp, const std::filesystem::path& final)> beforeRename;
    };
    struct LevelRange {
        int64_t levelMs = 0, oldestMs = 0, throughMs = 0; // bucket starts in [oldest, through)
    };
    struct Counters {
        std::atomic<uint64_t> daysBuilt{0}, buildFailures{0}, buildMicros{0}, fallbackServes{0};
        std::atomic<int64_t> pendingDays{0}; // closed days not yet built, as of the last pass
    };
    struct PassResult {
        uint64_t built = 0, failed = 0, pending = 0;
        bool stopped = false;
    };
    struct Reply {
        std::shared_ptr<const std::vector<uint8_t>> wire; // a Chunk frame, or null on error
        heatmap::ChunkError error = heatmap::ChunkError::BuildFailed;
        std::string message;
        bool fallback = false; // composed from minutes because a file was missing or corrupt
    };

    explicit RollupStore(Options options);
    ~RollupStore();
    RollupStore(const RollupStore&) = delete;
    RollupStore& operator=(const RollupStore&) = delete;

    // Builder thread (low priority): removes stale temp files, then runs a pass
    // every `rescan` or on wake(). stop() is idempotent and joins.
    void start();
    void stop();
    void wake();
    // One synchronous pass over every configured series (the builder loop body).
    // Passes never overlap. Tests call it directly instead of start().
    PassResult runPass(std::stop_token stop = {});
    // Replaces the series the next pass covers (between passes).
    void setSymbols(std::vector<std::string> symbols);

    // Advertised levels of a series: 1h and 1d, each only where every day is
    // built (see docs/MARKETDATA.md for the interval rules). Thread-safe, no I/O.
    std::vector<LevelRange> levels(const std::string& symbol, const std::string& layer) const;
    // Changes whenever any advertised level changes. Thread-safe, no I/O.
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

    // Thread-safe; `reader` belongs to the calling thread (minute fallback).
    Reply serve(const ChunkKey& key, Hmc2Reader& reader);

    std::filesystem::path filePath(const std::string& symbol, const std::string& layer, int64_t levelMs,
                                   int64_t dayMs) const;
    const std::filesystem::path& root() const { return options_.root; }
    const Counters& counters() const { return counters_; }

private:
    struct Series {
        int64_t firstMinuteMs = 0; // oldest recorded minute bucket (0: none)
        int64_t runStart = 0, runEnd = 0; // contiguous built days ending at the newest built day
    };
    struct CacheEntry {
        ChunkKey key;
        uint64_t identity = 0;
        std::shared_ptr<const std::vector<uint8_t>> wire;
    };
    Options options_;
    Counters counters_;
    mutable std::mutex indexMutex_;
    std::map<std::pair<std::string, std::string>, Series> index_;
    std::set<std::tuple<std::string, std::string, int64_t>> suspect_; // days to rebuild (corrupt at serve)
    std::atomic<uint64_t> generation_{1};
    std::mutex passMutex_;
    std::mutex cacheMutex_;
    std::list<CacheEntry> cache_;
    size_t cacheBytes_ = 0;
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;
    bool woken_ = false;
    std::stop_source stopSource_;
    std::thread thread_;

    void builderLoop(std::stop_token stop);
    void passSeries(Hmc2Reader& reader, const std::string& symbol, const std::string& layer, std::stop_token stop,
                    PassResult& result);
    void writeAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) const;
    void publish(const std::string& symbol, const std::string& layer, const Series& series);
    Series series(const std::string& symbol, const std::string& layer) const;
    void markSuspect(const std::string& symbol, const std::string& layer, int64_t dayMs);
    // A file of this day should exist (it has minutes): count, log and queue a
    // rebuild. False for a recorder-gap day.
    bool expectFile(const ChunkKey& key, const std::string& layer, int64_t dayMs);
    Reply serveHour(const ChunkKey& key, const std::string& layer, Hmc2Reader& reader);
    Reply serveDay(const ChunkKey& key, const std::string& layer, Hmc2Reader& reader);
    std::shared_ptr<const std::vector<uint8_t>> cached(const ChunkKey& key, uint64_t identity);
    void remember(const ChunkKey& key, uint64_t identity, std::shared_ptr<const std::vector<uint8_t>> wire);
};
} // namespace recording
