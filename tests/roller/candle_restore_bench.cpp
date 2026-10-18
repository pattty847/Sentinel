// Measures rebuildCandles on a scratch copy of real journals and compares it
// with a continuous pass from the copy's first record (--cmp).
//
//   candle_restore_bench synth <root> <count> <product>...
//       adds <count> header-only older files per product (one heartbeat each,
//       earlier run starts) so selection scans a realistic inventory.
//   candle_restore_bench run <root> [--threads N] [--file-back K] [--minute M]
//       [--window-ms W] [--force-reread] [--cmp] <product>...
//       `through` is the last record at or before minute M of the hour of the
//       file K places before the newest one.
// Never point it at a live recording root: synth writes into <root>.
#include "Crc32.hpp"
#include "roller/CandleRestore.hpp"
#include "roller/JournalFeed.hpp"
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace sentinel;
using namespace sentinel::roller;
namespace fs = std::filesystem;

namespace {
constexpr int64_t HourMs = 3'600'000;
const std::vector<int64_t> Tfs{100, 1'000};
// Continuous retention must hold the whole compared window.
std::map<int64_t, int> Caps{{100, 20'000}, {1'000, 2'000}};

void refuseLiveRoot(const fs::path& root) {
    const auto canonical = fs::weakly_canonical(root).string();
    for (const char* live : {"/Volumes/T7/sentinel-data", "/Users/copeharder/Programming/Sentinel/data"})
        if (canonical.starts_with(live)) throw std::runtime_error("refusing a live data root");
}

int64_t openedMs(const JournalFile& f) { return f.header.at("opened_system_ns").get<int64_t>() / 1'000'000; }

// Replace run_started_system_ns in place (same digit count) and re-sign the header.
void setRunStart(const fs::path& path, int64_t ns) {
    std::string bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size |= uint32_t(uint8_t(bytes[8 + i])) << (8 * i);
    std::string header = bytes.substr(16, size);
    const std::string key = "\"run_started_system_ns\":";
    const auto at = header.find(key);
    if (at == std::string::npos) throw std::runtime_error("no run start");
    const auto begin = at + key.size();
    const auto end = header.find_first_not_of("0123456789", begin);
    const auto digits = std::to_string(ns);
    if (digits.size() != end - begin) throw std::runtime_error("run start width differs");
    header.replace(begin, digits.size(), digits);
    const uint32_t crc = crc32(header.data(), header.size());
    for (int i = 0; i < 4; ++i) bytes[12 + i] = char(crc >> (8 * i));
    bytes.replace(16, size, header);
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

int synth(const fs::path& root, int count, const std::vector<std::string>& products) {
    for (const auto& product : products) {
        JournalReader reader(root, product);
        int64_t oldest = INT64_MAX;
        for (const auto& f : reader.files())
            oldest = std::min({oldest, openedMs(f),
                               f.header.at("run_started_system_ns").get<int64_t>() / 1'000'000});
        const auto metadata = nlohmann::json{{"product_metadata", reader.files().front().header.at("product_metadata")}};
        const int64_t base = oldest - oldest % HourMs - HourMs;
        capture::WriterConfig config;
        config.root = QString::fromStdString(root.string());
        config.symbol = product;
        config.fsyncBlocks = 0;
        for (int k = 0; k < count; ++k) {
            const int64_t ms = base - int64_t(count - k) * HourMs + 30'000;
            fs::path path;
            {
                capture::Writer writer(config, metadata);
                writer.append({capture::Kind::Frame, {ms * 1'000'000, ms * 1'000'000},
                               1, "{\"channel\":\"heartbeats\"}"});
                path = writer.currentPath().toStdString();
                writer.close();
            }
            setRunStart(path, ms * 1'000'000);
        }
        std::cout << "synth product=" << product << " files=" << count << "\n";
    }
    return 0;
}

struct Options {
    fs::path root;
    int threads = 1, fileBack = 0, minute = 50;
    int64_t windowMs = 0;
    bool forceReread = false, cmp = false;
};

JournalPos findThrough(const Options& o, const std::string& product) {
    JournalReader inventory(o.root, product);
    const auto& files = inventory.files();
    const auto& file = files.at(files.size() - 1 - size_t(o.fileBack));
    const int64_t opened = openedMs(file);
    const int64_t target = opened - opened % HourMs + int64_t(o.minute) * 60'000;
    JournalReader reader(o.root, product,
                         JournalPos{product, file.header.at("run_id"),
                                    file.header.at("first_block_ordinal").get<uint64_t>(), 0});
    JournalRecord r;
    std::optional<JournalPos> found;
    while (reader.next(r) && r.record.time.systemNs / 1'000'000 <= target) found = r.pos;
    if (!found) throw std::runtime_error("no record before target");
    return *found;
}

double threadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

struct Cmp {
    uint64_t differing = 0, compared = 0, sliver = 0, idsMissing = 0, idsExtra = 0, idsResent = 0;
};

uint64_t lateOf(const TimeframeAggregator& a, const std::string& product, int64_t tf) {
    for (const auto& d : a.lateDrops())
        if (d.symbol == product && d.timeframeMs == tf) return d.count;
    return 0;
}

// Continuous pass from the first record; a mirror seeded like the rebuild at
// its start file counts trades whose late/accepted classification differs.
Cmp compare(const Options& o, const CandleRestore& restore) {
    const auto& product = restore.product;
    JournalReader reader(o.root, product);
    const auto& start = reader.files().at(restore.startFile);
    const JournalPos startPos{product, start.header.at("run_id"),
                              start.header.at("first_block_ordinal").get<uint64_t>(), 0};
    TimeframeAggregator continuous(Tfs, Caps);
    std::unique_ptr<TimeframeAggregator> mirror;
    std::unordered_set<std::string> seen;
    Cmp out;
    JournalFeed feed(product);
    feed.onTrade = [&](const Trade& t) {
        if (!mirror) return continuous.onTrade(t);
        std::array<uint64_t, 2> before{}, mirrorBefore{};
        for (size_t i = 0; i < Tfs.size(); ++i) {
            before[i] = lateOf(continuous, product, Tfs[i]);
            mirrorBefore[i] = lateOf(*mirror, product, Tfs[i]);
        }
        continuous.onTrade(t);
        mirror->onTrade(t);
        if (!t.trade_id.empty()) seen.insert(t.trade_id);
        for (size_t i = 0; i < Tfs.size(); ++i)
            if ((lateOf(continuous, product, Tfs[i]) - before[i]) != (lateOf(*mirror, product, Tfs[i]) - mirrorBefore[i])) {
                ++out.sliver;
                break;
            }
    };
    int64_t sent = INT64_MIN;
    JournalRecord r;
    while (reader.next(r)) {
        if (!mirror && r.pos == startPos) {
            mirror = std::make_unique<TimeframeAggregator>(Tfs, Caps);
            mirror->seedJournalFloor(product, restore.seedWatermarkMs, Tfs);
        }
        feed.apply(r);
        const int64_t ms = r.record.time.systemNs / 1'000'000;
        const int64_t second = ms / 1000 - (ms % 1000 < 0);
        if (second > sent) {
            sent = second;
            continuous.advance(product, ms - 1'000);
            if (mirror) mirror->advance(product, ms - 1'000);
        }
        if (r.pos == restore.through) break;
    }
    for (const int64_t tf : Tfs) {
        const auto c = continuous.exportSeries(product, tf);
        std::map<int64_t, OHLCVBar> expected, actual;
        for (const auto& b : c.closed) if (b.timestamp_ms >= restore.windowStartMs) expected[b.timestamp_ms] = b;
        expected[-1] = c.active;
        const auto& s = restore.series.at(tf);
        for (const auto& b : s.closed) actual[b.timestamp_ms] = b;
        actual[-1] = s.active;
        std::set<int64_t> keys;
        for (const auto& [k, v] : expected) keys.insert(k);
        for (const auto& [k, v] : actual) keys.insert(k);
        for (const int64_t k : keys) {
            ++out.compared;
            const auto e = expected.find(k), a = actual.find(k);
            if (e == expected.end() || a == actual.end() ||
                std::tie(e->second.timestamp_ms, e->second.open, e->second.high, e->second.low, e->second.close,
                         e->second.volume, e->second.count, e->second.is_closed) !=
                    std::tie(a->second.timestamp_ms, a->second.open, a->second.high, a->second.low,
                             a->second.close, a->second.volume, a->second.count, a->second.is_closed))
                ++out.differing;
        }
    }
    for (const auto& id : feed.tradeIds())
        if (seen.contains(id) && !restore.tradeIds.contains(id)) ++out.idsMissing;
    // An id the continuous feed first saw before the read and the read saw
    // again (a resend) is in both sets; count it separately.
    for (const auto& id : restore.tradeIds) {
        if (!feed.tradeIds().contains(id)) ++out.idsExtra;
        else if (!seen.contains(id)) ++out.idsResent;
    }
    return out;
}

std::mutex printMutex;

bool runProduct(const Options& o, const std::string& product) {
    CandleRestoreInput in;
    in.root = o.root;
    in.product = product;
    in.through = findThrough(o, product);
    in.timeframesMs = Tfs;
    in.historyBars = Caps;
    in.windowMsForTest = o.windowMs;
    in.forceLeadInRereadForTest = o.forceReread;
    std::string failure;
    in.failure = &failure;
    const auto wall = std::chrono::steady_clock::now();
    const double cpu = threadCpuMs();
    const auto restore = rebuildCandles(in);
    const double cpuMs = threadCpuMs() - cpu;
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count();
    std::lock_guard lock(printMutex);
    if (!restore) {
        std::cout << "product=" << product << " FAILED reason=" << failure << "\n";
        return false;
    }
    std::cout << "product=" << product << " wallMs=" << wallMs << " cpuMs=" << cpuMs
              << " startFile=" << restore->startFile << " files=" << restore->files
              << " records=" << restore->records << " trades=" << restore->trades
              << " bars100=" << restore->series.at(100).closed.size()
              << " bars1000=" << restore->series.at(1'000).closed.size()
              << " lateDrops=" << restore->lateDrops << " clockRegressions=" << restore->clockRegressions
              << " deepRegressions=" << restore->deepRegressions
              << " outcome=" << (restore->outcome == RestoreOutcome::complete ? "complete" : "partial")
              << " tradeIds=" << restore->tradeIds.size() << " windowMin="
              << (restore->throughMs - restore->windowStartMs) / 60'000 << "\n";
    if (!o.cmp) return restore->outcome == RestoreOutcome::complete;
    const auto c = compare(o, *restore);
    const bool pass = c.differing == 0 && c.sliver == 0 && c.idsMissing == 0 && c.idsExtra == 0 &&
                      restore->outcome == RestoreOutcome::complete;
    std::cout << "cmp product=" << product << " comparedBars=" << c.compared << " differingBars=" << c.differing
              << " sliverTrades=" << c.sliver << " idsMissing=" << c.idsMissing << " idsExtra=" << c.idsExtra
              << " idsResentFromBeforeRead=" << c.idsResent << " " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

int run(Options o, const std::vector<std::string>& products) {
    const auto wall = std::chrono::steady_clock::now();
    std::atomic<int> failed{0};
    if (o.threads <= 1) {
        for (const auto& p : products) failed += runProduct(o, p) ? 0 : 1;
    } else {
        std::vector<std::thread> workers;
        for (const auto& p : products)
            workers.emplace_back([&, p] {
                try { failed += runProduct(o, p) ? 0 : 1; }
                catch (const std::exception& e) { std::cerr << p << ": " << e.what() << "\n"; ++failed; }
            });
        for (auto& w : workers) w.join();
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "total products=" << products.size() << " threads=" << o.threads << " wallMs="
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count()
              << " maxRssMb=" << usage.ru_maxrss / (1024.0 * 1024.0) << " failed=" << failed << "\n";
    return failed ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) try {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 2) throw std::runtime_error("usage: synth|run <root> ...");
    const fs::path root = args[1];
    refuseLiveRoot(root);
    if (args[0] == "synth") {
        if (args.size() < 4) throw std::runtime_error("usage: synth <root> <count> <product>...");
        return synth(root, std::stoi(args[2]), {args.begin() + 3, args.end()});
    }
    Options o;
    o.root = root;
    std::vector<std::string> products;
    for (size_t i = 2; i < args.size(); ++i) {
        const auto value = [&] { return args.at(++i); };
        if (args[i] == "--threads") o.threads = std::stoi(value());
        else if (args[i] == "--file-back") o.fileBack = std::stoi(value());
        else if (args[i] == "--minute") o.minute = std::stoi(value());
        else if (args[i] == "--window-ms") o.windowMs = std::stoll(value());
        else if (args[i] == "--force-reread") o.forceReread = true;
        else if (args[i] == "--cmp") o.cmp = true;
        else products.push_back(args[i]);
    }
    if (o.windowMs > 0)
        Caps = {{100, int(o.windowMs / 100 + 2'000)}, {1'000, int(o.windowMs / 1'000 + 200)}};
    return run(o, products);
} catch (const std::exception& e) {
    std::cerr << "candle_restore_bench: " << e.what() << "\n";
    return 2;
}
