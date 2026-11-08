#pragma once
#include "JournalReader.hpp"
#include "servermodel/TimeframeAggregator.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sentinel::roller {
// Rebuilds one product's trade candles from its journal, ending at a known
// record, without the live model: the newest kRestoreWindowMs of 100 ms and
// 1 s bars, and the open bar of every timeframe from 1 m to 1 d (closed bars
// of those are served from REST, never installed from here).
constexpr int64_t kRestoreWindowMs = 1'800'000;
// Journal time read before the window (and before the oldest open bucket) so
// its first bar has an initial state.
constexpr int64_t kRestoreLeadInMs = 60'000;
// Most files the 100 ms / 1 s window may need (normal spans are 1-2).
constexpr size_t kRestoreMaxFiles = 4;
// Most files one rebuild may read for the open bars (hourly files: a day plus
// the lead-in and the safety margin). Beyond it the read starts at the cap and
// the open bars may be partial.
constexpr size_t kRestoreOpenBarMaxFiles = 32;
// Journal time read before the lead-in, so a continuous read has converged on
// the uninterrupted path (late trades, carried bars) before the first bar that
// matters. The read starts in the file holding that position.
constexpr int64_t kRestoreSafetyMarginMs = 7'200'000;
// Longest timeframe whose open bar is rebuilt.
constexpr int64_t kRestoreMaxTimeframeMs = 86'400'000;

// The configured timeframes the rebuild covers, sorted: 100, 1000 and the
// whole-minute timeframes up to kRestoreMaxTimeframeMs, plus 60000 whenever a
// coarser one is present (coarse bars roll up from the minute).
std::vector<int64_t> restoreTimeframes(const std::vector<int64_t>& configuredMs);

enum class RestoreOutcome { complete, partial };

struct CandleRestoreInput {
    std::filesystem::path root;
    std::string product;
    JournalPos through; // the last record to apply; its receive time ends the window
    // True for a position past `through` (reaching one first is a failure).
    // Empty: past `through` in this reader's journal order.
    std::function<bool(const JournalPos&)> beyond;
    const std::atomic<bool>* stopping = nullptr; // checked after every record
    std::vector<int64_t> timeframesMs{100, 1'000}; // a restoreTimeframes() list
    // Closed-bar retention as configured live; each rebuilt timeframe keeps at
    // least the window plus the lead-in while reading.
    std::map<int64_t, int> historyBars;
    std::function<void(const JournalRecord&)> afterRecordForTest;
    int64_t windowMsForTest = 0;           // 0: kRestoreWindowMs
    std::string* failure = nullptr;        // reason when nullopt is returned
};

struct CandleRestore {
    std::string product;
    JournalPos through;
    int64_t throughMs = 0, windowStartMs = 0, seedWatermarkMs = 0;
    // Oldest open-bucket start among the rebuilt 1 m+ timeframes (0: none).
    int64_t openBarsFromMs = 0;
    int64_t sentSecond = INT64_MIN; // last journal second whose watermark was applied
    RestoreOutcome outcome = RestoreOutcome::complete;
    // Per timeframe: closed bars at or after windowStartMs (100 ms and 1 s
    // only), the open bar (absent when that bucket has none yet), the floor.
    std::map<int64_t, TimeframeAggregator::Series> series;
    std::unordered_set<std::string> tradeIds; // the read's retained trade ids
    // Counters of the final read: records, trades delivered, trades dropped as
    // late (largest count over the rebuilt timeframes), records older than the
    // newest seen, and those older by more than kRestoreLeadInMs.
    uint64_t records = 0, trades = 0, lateDrops = 0, clockRegressions = 0, deepRegressions = 0;
    size_t files = 0, startFile = 0;
    size_t reads = 0;  // journal replays (always 1 on success)
    double readMs = 0; // wall time of every read, re-reads included
};

// Never throws. nullopt on failure or when `stopping` is set; the caller reads
// `stopping` to tell them apart. One replay from the file holding (the oldest
// needed bucket - lead-in - safety margin), clamped to the file cap. Outcome
// partial: that replay holds no 100 ms / 1 s bar older than the window, or no
// 1 m bar older than the oldest open bucket; those bars begin at the first accepted
// trade the read holds and the floor guards the rest. Nothing is synthesized
// for the part of a bucket the journal does not hold.
std::optional<CandleRestore> rebuildCandles(const CandleRestoreInput& input);
} // namespace sentinel::roller
