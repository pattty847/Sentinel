#include "CandleRestore.hpp"
#include "JournalFeed.hpp"
#include "SentinelLogging.hpp"
#include "servermodel/ServerDataModel.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace sentinel::roller {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int64_t kLagMs = ServerDataModel::kJournalCandleLagMs;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr int64_t kMinuteMs = 60'000;

int64_t receiveMs(const JournalRecord& r) { return r.record.time.systemNs / 1'000'000; }

// TimeframeAggregator's bucket alignment (UTC epoch).
int64_t bucketStart(int64_t ms, int64_t tf) {
    const int64_t q = ms / tf;
    return (ms < 0 && ms % tf != 0 ? q - 1 : q) * tf;
}

bool openBarTimeframe(int64_t tf) { return tf >= kMinuteMs; }

struct FileInfo {
    std::string run;
    uint64_t firstBlock = 0;
    int64_t openedMs = 0;
};

struct Inventory {
    std::vector<FileInfo> files;
    size_t throughFile = 0;
    std::map<std::string, size_t> runOrder; // runs are contiguous in reader order
};

Inventory inventory(const JournalReader& reader, const JournalPos& through) {
    Inventory inv;
    bool found = false;
    for (const auto& f : reader.files()) {
        FileInfo info{f.header.at("run_id").get<std::string>(),
                      f.header.at("first_block_ordinal").get<uint64_t>(),
                      f.header.at("opened_system_ns").get<int64_t>() / 1'000'000};
        // Same rule as JournalReader's start file.
        if (info.run == through.run && info.firstBlock <= through.block) {
            inv.throughFile = inv.files.size();
            found = true;
        }
        inv.runOrder.try_emplace(info.run, inv.runOrder.size());
        inv.files.push_back(std::move(info));
    }
    if (!found) throw Failure("through file missing");
    return inv;
}

// The newest file opened at or before the cutoff, or one file before the
// oldest earlier file whose header shows the clock already past the cutoff.
size_t selectStart(const Inventory& inv, int64_t cutoffMs) {
    size_t newest = 0;
    for (size_t i = 0; i <= inv.throughFile; ++i)
        if (inv.files[i].openedMs <= cutoffMs) newest = i;
    for (size_t i = 0; i < newest; ++i)
        if (inv.files[i].openedMs > cutoffMs) return i == 0 ? 0 : i - 1;
    return newest;
}

struct Read {
    std::unique_ptr<TimeframeAggregator> aggregator;
    std::unordered_set<std::string> ids;
    int64_t seedMs = 0, sentSecond = INT64_MIN;
    uint64_t records = 0, trades = 0, clockRegressions = 0, deepRegressions = 0;
};

struct Plan {
    const CandleRestoreInput& in;
    const Inventory& inv;
    std::map<int64_t, int> caps;
    std::function<bool(const JournalPos&)> beyond;
};

void countRegression(Read& read, int64_t& maxSeenMs, int64_t ms, const std::string& product) {
    if (ms < maxSeenMs) {
        ++read.clockRegressions;
        if (maxSeenMs - ms > kRestoreLeadInMs) ++read.deepRegressions;
        sLog_Probe("candles.restore.regression", "product=" << product.c_str() << " ms=" << ms
                   << " maxSeenMs=" << maxSeenMs);
    }
    maxSeenMs = std::max(maxSeenMs, ms);
}

// One pass from the first record of file `start` through `through`. nullopt
// when stopping was observed.
std::optional<Read> readFrom(const Plan& plan, size_t start) {
    const auto& in = plan.in;
    Read out;
    out.aggregator = std::make_unique<TimeframeAggregator>(in.timeframesMs, plan.caps);
    auto& aggregator = *out.aggregator;
    JournalFeed feed(in.product);
    feed.onTrade = [&](const Trade& trade) {
        ++out.trades;
        aggregator.onTrade(trade);
    };
    const auto& file = plan.inv.files[start];
    out.seedMs = file.openedMs - kLagMs;
    aggregator.seedJournalFloor(in.product, out.seedMs, in.timeframesMs);
    JournalReader reader(in.root, in.product, JournalPos{in.product, file.run, file.firstBlock, 0});
    JournalRecord r;
    int64_t maxSeenMs = INT64_MIN;
    for (;;) {
        if (!reader.next(r)) throw Failure("journal ended before through");
        if (r.pos != in.through && plan.beyond(r.pos)) throw Failure("read passed through");
        const int64_t ms = receiveMs(r);
        ++out.records;
        feed.apply(r);
        // The model tap's rule: one watermark per journal second, lagged.
        const int64_t second = ms / 1000 - (ms % 1000 < 0);
        if (second > out.sentSecond) {
            out.sentSecond = second;
            aggregator.advance(in.product, ms - kLagMs);
        }
        countRegression(out, maxSeenMs, ms, in.product);
        if (in.afterRecordForTest) in.afterRecordForTest(r);
        // Checked after every record, `through` included.
        if (in.stopping && in.stopping->load()) return std::nullopt;
        if (r.pos == in.through) break;
    }
    if (reader.damaged() != 0) throw Failure("damaged journal prefix");
    out.ids = feed.tradeIds();
    return out;
}

bool hasBar(const OHLCVBar& bar) { return bar.count != 0 || bar.timestamp_ms != 0; }

// Whether a read from file `start` would hold a `tf` bar older than
// requiredMs, decided by its first accepted trade (a later bar is never
// older) or by the floor reaching requiredMs. Reads only up to that decision,
// and with fileOnly only file `start` itself (undecided there: false). Counts
// the records it read. nullopt when stopping was observed.
std::optional<bool> probe(const Plan& plan, size_t start, bool fileOnly, int64_t tf, int64_t requiredMs,
                          uint64_t& records) {
    const auto& in = plan.in;
    TimeframeAggregator aggregator({tf});
    JournalFeed feed(in.product);
    feed.onTrade = [&](const Trade& trade) { aggregator.onTrade(trade); };
    const auto& file = plan.inv.files[start];
    const FileInfo* next = start + 1 < plan.inv.files.size() ? &plan.inv.files[start + 1] : nullptr;
    aggregator.seedJournalFloor(in.product, file.openedMs - kLagMs, {tf});
    JournalReader reader(in.root, in.product, JournalPos{in.product, file.run, file.firstBlock, 0});
    JournalRecord r;
    int64_t sentSecond = INT64_MIN;
    for (;;) {
        if (!reader.next(r)) throw Failure("journal ended before through");
        if (r.pos != in.through && plan.beyond(r.pos)) throw Failure("read passed through");
        if (fileOnly && (r.pos.run != file.run || (next && next->run == file.run && r.pos.block >= next->firstBlock)))
            return false;
        const int64_t ms = receiveMs(r);
        ++records;
        feed.apply(r);
        const int64_t second = ms / 1000 - (ms % 1000 < 0);
        if (second > sentSecond) {
            sentSecond = second;
            aggregator.advance(in.product, ms - kLagMs);
        }
        if (in.afterRecordForTest) in.afterRecordForTest(r);
        if (in.stopping && in.stopping->load()) return std::nullopt;
        // No closed bar exists before the first one opens, so this copies nothing.
        const auto series = aggregator.exportSeries(in.product, tf);
        if (hasBar(series.active) || !series.closed.empty())
            return (series.closed.empty() ? series.active : series.closed.front()).timestamp_ms < requiredMs;
        if (series.floorMs && *series.floorMs >= requiredMs) return false;
        if (r.pos == in.through) return false;
    }
}

// Only an accepted lead-in trade creates a bar older than the window.
bool established(const TimeframeAggregator& aggregator, const std::string& product, int64_t tf,
                 int64_t windowStartMs) {
    const auto series = aggregator.exportSeries(product, tf);
    return (!series.closed.empty() && series.closed.front().timestamp_ms < windowStartMs) ||
           (hasBar(series.active) && series.active.timestamp_ms < windowStartMs);
}

std::function<bool(const JournalPos&)> defaultBeyond(const Inventory& inv, const JournalPos& through) {
    const size_t throughRun = inv.runOrder.at(through.run);
    return [&inv, through, throughRun](const JournalPos& p) {
        if (p.run == through.run)
            return std::pair(p.block, p.record) > std::pair(through.block, through.record);
        const auto run = inv.runOrder.find(p.run);
        return run == inv.runOrder.end() || run->second > throughRun;
    };
}

bool supported(int64_t tf) {
    return tf == 100 || tf == 1'000 ||
           (tf >= kMinuteMs && tf % kMinuteMs == 0 && tf <= kRestoreMaxTimeframeMs);
}

void validateTimeframes(const std::vector<int64_t>& timeframes) {
    if (timeframes.empty()) throw Failure("no timeframes");
    bool minute = false, coarse = false;
    for (const int64_t tf : timeframes) {
        if (!supported(tf)) throw Failure("unsupported timeframe " + std::to_string(tf));
        minute = minute || tf == kMinuteMs;
        coarse = coarse || tf > kMinuteMs;
    }
    if (coarse && !minute) throw Failure("coarse timeframe without 60000");
}

void exportInto(CandleRestore& out, Read& read, const std::vector<int64_t>& timeframes) {
    for (const int64_t tf : timeframes) {
        auto series = read.aggregator->exportSeries(out.product, tf);
        if (openBarTimeframe(tf)) { // closed bars come from REST
            series.closed.clear();
            out.series[tf] = std::move(series);
            continue;
        }
        const auto first = std::partition_point(series.closed.begin(), series.closed.end(),
            [&](const OHLCVBar& bar) { return bar.timestamp_ms < out.windowStartMs; });
        series.closed.erase(series.closed.begin(), first);
        out.series[tf] = std::move(series);
    }
    for (const auto& drops : read.aggregator->lateDrops())
        if (drops.symbol == out.product &&
            std::find(timeframes.begin(), timeframes.end(), drops.timeframeMs) != timeframes.end())
            out.lateDrops = std::max(out.lateDrops, drops.count);
    out.tradeIds = std::move(read.ids);
    out.seedWatermarkMs = read.seedMs;
    out.sentSecond = read.sentSecond;
    out.records = read.records;
    out.trades = read.trades;
    out.clockRegressions = read.clockRegressions;
    out.deepRegressions = read.deepRegressions;
}

const char* outcomeName(RestoreOutcome outcome) {
    return outcome == RestoreOutcome::complete ? "complete" : "partial";
}

std::optional<CandleRestore> rebuild(const CandleRestoreInput& in, Clock::time_point began) {
    validateTimeframes(in.timeframesMs);
    CandleRestore out;
    out.product = in.product;
    out.through = in.through;
    std::optional<Inventory> inv;
    try {
        JournalReader probe(in.root, in.product, in.through);
        JournalRecord r;
        if (!probe.next(r) || r.pos != in.through) throw Failure("no record");
        out.throughMs = receiveMs(r);
        inv = inventory(probe, in.through);
    } catch (const std::exception& e) {
        throw Failure(std::string("through unreadable: ") + e.what());
    }
    const int64_t windowMs = in.windowMsForTest > 0 ? in.windowMsForTest : kRestoreWindowMs;
    out.windowStartMs = out.throughMs - windowMs;
    int64_t fineTf = 0;
    bool openBars = false;
    for (const int64_t tf : in.timeframesMs) {
        if (openBarTimeframe(tf)) {
            // The read's last watermark is at least the start of through's
            // second minus the lag; each open bucket starts at or after it.
            const int64_t watermarkMs = bucketStart(out.throughMs, 1'000) - kLagMs;
            const int64_t from = bucketStart(watermarkMs, tf);
            out.openBarsFromMs = openBars ? std::min(out.openBarsFromMs, from) : from;
            openBars = true;
        } else {
            fineTf = std::max(fineTf, tf);
        }
    }
    const auto span = [&](size_t first) { return inv->throughFile - first + 1; };
    const size_t fileCap = openBars ? kRestoreOpenBarMaxFiles : kRestoreMaxFiles;
    const int64_t fineCutoffMs = out.windowStartMs - kRestoreLeadInMs;
    int64_t cutoffMs = fineCutoffMs;
    size_t start = inv->throughFile;
    if (fineTf) {
        start = selectStart(*inv, fineCutoffMs);
        if (span(start) > kRestoreMaxFiles)
            throw Failure("file span " + std::to_string(span(start)));
    }
    if (openBars) {
        const int64_t barCutoffMs = out.openBarsFromMs - kRestoreLeadInMs;
        cutoffMs = fineTf ? std::min(cutoffMs, barCutoffMs) : barCutoffMs;
        const size_t earliest = span(0) > fileCap ? inv->throughFile + 1 - fileCap : 0;
        const size_t barStart = std::max(selectStart(*inv, barCutoffMs), earliest);
        start = fineTf ? std::min(start, barStart) : barStart;
    }
    Plan plan{in, *inv, in.historyBars, in.beyond ? in.beyond : defaultBeyond(*inv, in.through)};
    for (const int64_t tf : in.timeframesMs) {
        const int64_t covered = openBarTimeframe(tf) ? out.throughMs - cutoffMs : windowMs + kRestoreLeadInMs;
        const int need = static_cast<int>(covered / tf + 2);
        auto [cap, inserted] = plan.caps.try_emplace(tf, need);
        cap->second = std::max(cap->second, need);
    }
    sLog_App("Candle restore start product=" << in.product.c_str() << " through="
             << in.through.run.c_str() << "/" << in.through.block << "/" << in.through.record
             << " throughMs=" << out.throughMs << " windowStartMs=" << out.windowStartMs
             << " openBarsFromMs=" << out.openBarsFromMs << " startFile=" << start << " throughFile=" << inv->throughFile
             << " inventory=" << inv->files.size());
    // Choose the start by probing, then replay once. The probe of the first
    // candidate reads until its decision (the lead-in); each earlier candidate
    // probes only its own file, so the lead-in search reads each file at most
    // once and the busy interval is replayed exactly once. Checked: the 1 m
    // bar before the oldest open bucket (with open bars; a trade before it
    // also starts the 100 ms / 1 s bars), else the window's own timeframe.
    const int64_t checkTf = openBars ? kMinuteMs : fineTf;
    const int64_t requiredMs = openBars ? out.openBarsFromMs : out.windowStartMs;
    const size_t candidate = start;
    bool forceExtend = in.forceLeadInRereadForTest;
    for (bool fileOnly = false;; fileOnly = true) {
        const auto found = probe(plan, start, fileOnly, checkTf, requiredMs, out.probeRecords);
        if (!found) return std::nullopt;
        ++out.probes;
        if (*found && !std::exchange(forceExtend, false)) break;
        if (start == 0 || span(start - 1) > fileCap) {
            start = candidate; // earlier files hold no establishing trade: no reason to read them
            break;
        }
        sLog_App("Candle restore lead-in not established product=" << in.product.c_str()
                 << " startFile=" << start << " action=probe_one_file_earlier");
        --start;
    }
    auto read = readFrom(plan, start);
    if (!read) return std::nullopt;
    ++out.reads;
    const auto& aggregator = *read->aggregator;
    const bool ok =
        (!fineTf || established(aggregator, in.product, fineTf, out.windowStartMs)) &&
        (!openBars || established(aggregator, in.product, kMinuteMs, out.openBarsFromMs));
    out.outcome = ok ? RestoreOutcome::complete : RestoreOutcome::partial;
    exportInto(out, *read, in.timeframesMs);
    out.startFile = start;
    out.files = span(start);
    out.readMs = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    if (in.stopping && in.stopping->load()) return std::nullopt;
    return out;
}
} // namespace

std::vector<int64_t> restoreTimeframes(const std::vector<int64_t>& configuredMs) {
    std::vector<int64_t> out;
    for (const int64_t tf : configuredMs)
        if (supported(tf)) out.push_back(tf);
    if (std::any_of(out.begin(), out.end(), [](int64_t tf) { return tf > kMinuteMs; }))
        out.push_back(kMinuteMs);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<CandleRestore> rebuildCandles(const CandleRestoreInput& in) {
    const auto began = Clock::now();
    const auto fail = [&](const std::string& reason) {
        if (in.failure) *in.failure = reason;
    };
    try {
        auto out = rebuild(in, began);
        if (!out) {
            fail("cancelled");
            sLog_App("Candle restore cancelled product=" << in.product.c_str());
            return std::nullopt;
        }
        if (out->deepRegressions)
            sLog_Warning("Candle restore deep clock regressions product=" << in.product.c_str()
                         << " deepRegressions=" << out->deepRegressions);
        sLog_App("Candle restore built product=" << in.product.c_str()
                 << " outcome=" << outcomeName(out->outcome) << " startFile=" << out->startFile
                 << " files=" << out->files << " reads=" << out->reads << " probes=" << out->probes
                 << " probeRecords=" << out->probeRecords << " records=" << out->records
                 << " trades=" << out->trades << " lateDrops=" << out->lateDrops
                 << " clockRegressions=" << out->clockRegressions
                 << " deepRegressions=" << out->deepRegressions
                 << " tradeIds=" << out->tradeIds.size() << " readMs=" << out->readMs);
        return out;
    } catch (const std::exception& e) {
        if (in.stopping && in.stopping->load()) {
            fail("cancelled");
            sLog_App("Candle restore cancelled product=" << in.product.c_str());
            return std::nullopt;
        }
        fail(e.what());
        sLog_Error("Candle restore failed product=" << in.product.c_str()
                   << " reason=" << e.what());
        return std::nullopt;
    }
}
} // namespace sentinel::roller
