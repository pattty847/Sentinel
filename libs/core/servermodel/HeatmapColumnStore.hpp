#pragma once

// HeatmapColumnStore — slot-addressed durable store for derived heatmap columns.
//
// Phase 1.2: writer half only. Reader (loadRecent / fetchRange) lands in phases 2-3.
//
// Decisions (see docs/private/plans/F1_HEATMAP_PERSISTENCE.md §Decisions 2026-05-08):
//   - One file per (symbol, timeframeMs, UTC day) under
//     <baseDir>/<symbol>/<timeframeMs>/v1/<YYYY-MM-DD>.hmcol.
//   - Slot index = (bucketStartMs - dayStartMs) / timeframeMs.
//   - Writes are idempotent: a slot already holding a valid record with the same
//     bucketStartMs is a no-op success; a different bucketStartMs at the same slot
//     is reported as a conflict and NOT overwritten.
//   - Day file is created sparse-extended to full day length on open. Empty slots
//     read back as zeroed (bucketStartMs == kEmptySlotSentinel).
//   - One process at a time, enforced by an exclusive OS lock on <baseDir>/.lock
//     held for the lifetime of the store object.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "HmcolFormat.hpp"

struct HeatmapColumnStoreConfig {
    int fsyncEveryNRecords = 5;     // upper bound between fsyncs
    int fsyncEveryMs = 1000;        // wall-clock upper bound between fsyncs
};

class HeatmapColumnStore {
public:
    using Config = HeatmapColumnStoreConfig;

    enum class AppendResult {
        Written,         // new record written to a previously-empty slot
        AlreadyPresent,  // slot already held an identical valid record (no-op)
        SlotConflict,    // slot held a valid record with a *different* bucketStartMs (skipped)
        IoError,         // file-system or low-level I/O failure
        BadInput,        // gridHeight mismatch / null pointer / out-of-range bucket
    };

    explicit HeatmapColumnStore(std::filesystem::path baseDir, Config cfg = Config{});
    ~HeatmapColumnStore();

    HeatmapColumnStore(const HeatmapColumnStore&) = delete;
    HeatmapColumnStore& operator=(const HeatmapColumnStore&) = delete;

    // Acquire the exclusive process lock at <baseDir>/.lock. Must be called
    // exactly once before any append(). Returns false if another process holds
    // the lock or the lock file cannot be created.
    bool acquireLock();

    // True between successful acquireLock() and the destructor.
    bool isLocked() const noexcept { return m_lockFd >= 0; }

    // Persist a finalized heatmap column.
    //   intensity: gridHeight u16 cells (must not be null).
    //   liquidity: gridHeight u16 cells, or nullptr if no liquidity column.
    // Caller's intensity/liquidity buffers are read but not retained.
    AppendResult append(const std::string& symbol,
                        int64_t timeframeMs,
                        int32_t gridHeight,
                        int64_t bucketStartMs,
                        int64_t bucketEndMs,
                        double minPrice,
                        double maxPrice,
                        double tickSize,
                        const uint16_t* intensity,
                        const uint16_t* liquidity,
                        double liquidityScale);

    // Force fsync on every open day-writer.
    void flush();

    // Lightweight observability for tests / logging.
    struct Stats {
        uint64_t written = 0;
        uint64_t alreadyPresent = 0;
        uint64_t slotConflicts = 0;
        uint64_t ioErrors = 0;
        uint64_t badInputs = 0;
        uint64_t flushes = 0;
    };
    Stats stats() const;

private:
    struct DayWriterKey {
        std::string symbol;
        int64_t timeframeMs;
        int64_t dayStartMs;

        bool operator==(const DayWriterKey& o) const noexcept {
            return symbol == o.symbol && timeframeMs == o.timeframeMs && dayStartMs == o.dayStartMs;
        }
    };
    struct DayWriterKeyHash {
        std::size_t operator()(const DayWriterKey& k) const noexcept {
            std::size_t h = std::hash<std::string>{}(k.symbol);
            h ^= std::hash<int64_t>{}(k.timeframeMs) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<int64_t>{}(k.dayStartMs) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct DayWriter {
        std::fstream stream;
        std::filesystem::path path;
        int64_t dayStartMs = 0;
        int64_t timeframeMs = 0;
        int32_t gridHeight = 0;
        uint8_t liquidityFormat = hmcol::kLiquidityFormatNone;
        std::size_t recordStride = 0;
        int64_t slotsPerDay = 0;
        int recordsSinceFlush = 0;
        std::chrono::steady_clock::time_point lastFlush{};
    };

    DayWriter* openOrGet(const DayWriterKey& key,
                         int32_t gridHeight,
                         uint8_t liquidityFormat,
                         const std::string& symbol);

    static std::filesystem::path filePathFor(const std::filesystem::path& baseDir,
                                             const std::string& symbol,
                                             int64_t timeframeMs,
                                             int64_t dayStartMs);

    void maybeFlush(DayWriter& w);

    Config m_config;
    std::filesystem::path m_baseDir;
    std::filesystem::path m_lockPath;
    int m_lockFd = -1;

    mutable std::mutex m_mutex;
    std::unordered_map<DayWriterKey, DayWriter, DayWriterKeyHash> m_writers;
    Stats m_stats;
};
