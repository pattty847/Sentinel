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

int64_t receiveMs(const JournalRecord& r) { return r.record.time.systemNs / 1'000'000; }

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

void validateTimeframes(const std::vector<int64_t>& timeframes) {
    if (timeframes.empty()) throw Failure("no timeframes");
    for (const int64_t tf : timeframes)
        if (tf != 100 && tf != 1'000) throw Failure("unsupported timeframe " + std::to_string(tf));
}

void exportInto(CandleRestore& out, Read& read, const std::vector<int64_t>& timeframes) {
    for (const int64_t tf : timeframes) {
        auto series = read.aggregator->exportSeries(out.product, tf);
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
    const int64_t cutoffMs = out.windowStartMs - kRestoreLeadInMs;
    size_t start = selectStart(*inv, cutoffMs);
    const auto span = [&](size_t first) { return inv->throughFile - first + 1; };
    if (span(start) > kRestoreMaxFiles)
        throw Failure("file span " + std::to_string(span(start)));
    Plan plan{in, *inv, in.historyBars, in.beyond ? in.beyond : defaultBeyond(*inv, in.through)};
    for (const int64_t tf : in.timeframesMs) {
        const int need = static_cast<int>((windowMs + kRestoreLeadInMs) / tf + 2);
        auto [cap, inserted] = plan.caps.try_emplace(tf, need);
        cap->second = std::max(cap->second, need);
    }
    const int64_t checkTf = *std::max_element(in.timeframesMs.begin(), in.timeframesMs.end());
    sLog_App("Candle restore start product=" << in.product.c_str() << " through="
             << in.through.run.c_str() << "/" << in.through.block << "/" << in.through.record
             << " throughMs=" << out.throughMs << " windowStartMs=" << out.windowStartMs
             << " startFile=" << start << " throughFile=" << inv->throughFile
             << " inventory=" << inv->files.size());
    bool forceReread = in.forceLeadInRereadForTest;
    for (;;) {
        auto read = readFrom(plan, start);
        if (!read) return std::nullopt;
        const bool forced = std::exchange(forceReread, false);
        const bool ok =
            !forced && established(*read->aggregator, in.product, checkTf, out.windowStartMs);
        const bool extend = !ok && start > 0 && span(start - 1) <= kRestoreMaxFiles;
        if (extend) {
            sLog_App("Candle restore lead-in not established product=" << in.product.c_str()
                     << " startFile=" << start << " action=read_one_file_earlier");
            --start;
            continue;
        }
        out.outcome = ok ? RestoreOutcome::complete : RestoreOutcome::partial;
        exportInto(out, *read, in.timeframesMs);
        break;
    }
    out.startFile = start;
    out.files = span(start);
    out.readMs = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    if (in.stopping && in.stopping->load()) return std::nullopt;
    return out;
}
} // namespace

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
                 << " files=" << out->files << " records=" << out->records
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
