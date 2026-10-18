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
// Rebuilds the newest kRestoreWindowMs of trade candles (100 ms and 1 s) from
// one product's journal, ending at a known record, without the live model.
constexpr int64_t kRestoreWindowMs = 1'800'000;
// Journal time read before the window so its first bar has an initial state.
constexpr int64_t kRestoreLeadInMs = 60'000;
// Most files one rebuild may read (normal spans are 1-2).
constexpr size_t kRestoreMaxFiles = 4;

enum class RestoreOutcome { complete, partial };

struct CandleRestoreInput {
    std::filesystem::path root;
    std::string product;
    JournalPos through; // the last record to apply; its receive time ends the window
    // True for a position past `through` (reaching one first is a failure).
    // Empty: past `through` in this reader's journal order.
    std::function<bool(const JournalPos&)> beyond;
    const std::atomic<bool>* stopping = nullptr; // checked after every record
    std::vector<int64_t> timeframesMs{100, 1'000}; // trade timeframes below 1 m only
    // Closed-bar retention as configured live; each rebuilt timeframe keeps at
    // least the window plus the lead-in while reading.
    std::map<int64_t, int> historyBars;
    std::function<void(const JournalRecord&)> afterRecordForTest;
    int64_t windowMsForTest = 0;           // 0: kRestoreWindowMs
    bool forceLeadInRereadForTest = false; // first read counts as not established
    std::string* failure = nullptr;        // reason when nullopt is returned
};

struct CandleRestore {
    std::string product;
    JournalPos through;
    int64_t throughMs = 0, windowStartMs = 0, seedWatermarkMs = 0;
    int64_t sentSecond = INT64_MIN; // last journal second whose watermark was applied
    RestoreOutcome outcome = RestoreOutcome::complete;
    // Per timeframe: closed bars at or after windowStartMs, the open bar, the floor.
    std::map<int64_t, TimeframeAggregator::Series> series;
    std::unordered_set<std::string> tradeIds; // the read's retained trade ids
    // Counters of the final read: records, trades delivered, trades dropped as
    // late (largest count over the rebuilt timeframes), records older than the
    // newest seen, and those older by more than kRestoreLeadInMs.
    uint64_t records = 0, trades = 0, lateDrops = 0, clockRegressions = 0, deepRegressions = 0;
    size_t files = 0, startFile = 0;
    double readMs = 0; // wall time of every read, re-reads included
};

// Never throws. nullopt on failure or when `stopping` is set; the caller reads
// `stopping` to tell them apart. Outcome partial: no bar older than the window
// could be established inside kRestoreMaxFiles; the bars begin at the first
// accepted trade in the window and the floor guards the rest.
std::optional<CandleRestore> rebuildCandles(const CandleRestoreInput& input);
} // namespace sentinel::roller
