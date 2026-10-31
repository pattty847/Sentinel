// Costs for Architecture C E1b-1 (not a ctest).
//   levelstore_bench sink [messages] [pairs]
//     Recorder worker CPU per accepted level change: sink null vs LevelRecorder (output
//     discarded), the same synthetic stream, ABBA-interleaved pairs, as recorder_sink_bench.
//   levelstore_bench feed RAW_ROOT PRODUCT FROM_ISO HOURS [reps]
//     JournalFeed on real RAWL2 records held in memory: total apply time per L2 level,
//     and the added exact-decimal pass alone (the same text lookups and parse), timed on
//     the same frames, interleaved.
#include "LevelTestSupport.hpp"
#include "roller/JournalFeed.hpp"
#include "roller/Roller.hpp"
#include <QCoreApplication>
#include <mach/mach.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>

using namespace recording;
using namespace levelstore_test;
using nlohmann::json;

namespace {
int64_t threadCpuNs(const char *name) {
    thread_act_array_t threads;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return -1;
    int64_t out = -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_extended_info_data_t info;
        mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count) ==
                KERN_SUCCESS && std::strcmp(info.pth_name, name) == 0)
            out = int64_t(info.pth_user_time + info.pth_system_time);
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), n * sizeof(thread_act_t));
    return out;
}
struct Discard final : LevelOutput {
    uint64_t blocks = 0;
    void onDayStart(const DayBinding &, const BookImage &) override {}
    void onBlock(const DayBinding &, const MinuteBlock &) override { ++blocks; }
};
struct Counting final : RecorderSink {
    uint64_t levels = 0;
    void onReset(const std::string &, int64_t) noexcept override {}
    void onImprecise(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onLevel(const std::string &, int64_t, const LevelChange &) noexcept override { ++levels; }
    void onApplied(const std::string &, int64_t, bool, bool) noexcept override {}
    void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
    void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override {}
};
std::string centsText(int64_t units) {
    return std::to_string(units / 100) + "." + std::to_string(100 + units % 100).substr(1);
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

int sinkBench(int messages, int pairs) {
    std::mt19937_64 rng(7);
    std::vector<std::vector<Level>> stream(static_cast<size_t>(messages));
    for (auto &m : stream) {
        m.resize(16);
        for (auto &l : m) {
            const bool bid = rng() & 1;
            const int64_t off = int64_t(rng() % 200'000) + 1;
            const int64_t units = bid ? 10'000'000 - off : 10'000'000 + off;
            const auto text = centsText(units);
            l = {bid, std::strtod(text.c_str(), nullptr), (rng() % 4) ? double(rng() % 1000) / 1000 : 0.0,
                 parsePriceDecimal(text)};
        }
    }
    std::vector<Level> snapshot;
    for (int64_t i = 1; i <= 20'000; ++i) {
        snapshot.push_back(L(true, centsText(10'000'000 - i * 10).c_str(), 1.0));
        snapshot.push_back(L(false, centsText(10'000'000 + i * 10).c_str(), 1.0));
    }
    const int64_t t0 = kHmc2MinMs + 86'400'000;
    const auto once = [&](bool withLevel, Counting *counting) {
        TempDir dir;
        RecorderConfig c;
        c.root = dir.path;
        c.priceScale = 100;
        c.layers = {{"near", 100, .95, 1.05, false}, {"deep", 500, .25, 4., true}};
        c.deterministicResume = true;
        Discard out;
        std::unique_ptr<LevelRecorder> level;
        if (withLevel) level = std::make_unique<LevelRecorder>(DayBinding{"BTC-USD", t0, t0 + 86'400'000, 2}, &out);
        c.sink = counting ? static_cast<RecorderSink *>(counting) : level.get();
        BookRecorder r(c);
        r.onSnapshotAt("BTC-USD", t0, t0, snapshot);
        r.drainForTest();
        const int64_t before = threadCpuNs("book-recorder");
        for (int i = 0; i < messages; ++i) {
            const int64_t t = t0 + int64_t(i) * 5; // 200 messages per second
            r.onUpdatesAt("BTC-USD", t, t, stream[size_t(i)]);
        }
        r.drainForTest();
        return threadCpuNs("book-recorder") - before;
    };
    Counting counting;
    once(false, &counting);
    const uint64_t changes = counting.levels - snapshot.size();
    std::vector<double> deltas, nulls, levels;
    std::cout << "messages=" << messages << " timedLevelChanges=" << changes << "\npair,order,nullMs,levelMs,deltaNsPerChange\n";
    for (int p = 0; p < pairs; ++p) {
        int64_t a = 0, b = 0;
        if (p % 2 == 0) { a = once(false, nullptr); b = once(true, nullptr); }
        else { b = once(true, nullptr); a = once(false, nullptr); }
        const double delta = double(b - a) / double(changes);
        deltas.push_back(delta);
        nulls.push_back(double(a) / double(changes));
        levels.push_back(double(b) / double(changes));
        std::cout << p << "," << (p % 2 ? "level-first" : "null-first") << "," << a / 1e6 << "," << b / 1e6 << "," << delta << "\n";
    }
    std::sort(deltas.begin(), deltas.end());
    std::cout << json({{"bench", "sink"}, {"timedLevelChanges", changes}, {"pairs", pairs},
                       {"nullNsPerChangeMedian", median(nulls)}, {"levelNsPerChangeMedian", median(levels)},
                       {"pairedDeltaNsPerChange", {{"median", median(deltas)}, {"min", deltas.front()}, {"max", deltas.back()},
                                                   {"q1", deltas[deltas.size() / 4]}, {"q3", deltas[deltas.size() * 3 / 4]}}}})
                     .dump()
              << "\n";
    return 0;
}

int feedBench(const fs::path &raw, const std::string &product, const std::string &from, int hours, int reps) {
    using namespace sentinel::roller;
    const int64_t start = parseTime(from), end = start + int64_t(hours) * 3'600'000;
    JournalReader inventory(raw, product);
    JournalReader reader(raw, product, inventory.anchor(start));
    std::vector<JournalRecord> records;
    JournalRecord r;
    while (reader.next(r)) {
        const int64_t t = r.record.time.systemNs / 1'000'000;
        if (t >= end) break;
        records.push_back(std::move(r));
    }
    uint64_t levels = 0, frames = 0;
    const auto feedOnce = [&] {
        JournalFeed feed(product);
        uint64_t n = 0;
        feed.onSnapshot = [&](int64_t, int64_t, std::vector<Level> l) { n += l.size(); };
        feed.onUpdates = [&](int64_t, int64_t, std::vector<Level> l) { n += l.size(); };
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto &rec : records) feed.apply(rec);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        levels = n;
        return ns;
    };
    // The added pass on the same frames: the per-level text lookup and exact parse
    // JournalFeed now performs, after the JSON parse it already did.
    std::vector<nlohmann::json> parsed;
    for (const auto &rec : records)
        if (rec.record.kind == sentinel::capture::Kind::Frame && rec.record.payload.find("\"l2_data\"") != std::string::npos) {
            parsed.push_back(nlohmann::json::parse(rec.record.payload));
            ++frames;
        }
    const auto decimalOnce = [&] {
        uint64_t exact = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto &j : parsed)
            for (const auto &e : j["events"])
                if (e.contains("updates"))
                    for (const auto &u : e["updates"]) {
                        const auto it = u.find("price_level");
                        if (it != u.end() && it->is_string()) exact += parsePriceDecimal(it->get_ref<const std::string &>()).exact;
                    }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (!exact) std::cerr << "no exact decimal\n";
        return ns;
    };
    std::vector<double> total, pass;
    for (int i = 0; i < reps; ++i) {
        if (i % 2) { pass.push_back(decimalOnce()); total.push_back(feedOnce()); }
        else { total.push_back(feedOnce()); pass.push_back(decimalOnce()); }
    }
    const double perLevelTotal = median(total) / double(levels), perLevelPass = median(pass) / double(levels);
    std::cout << json({{"bench", "feed"}, {"product", product}, {"from", from}, {"hours", hours}, {"records", records.size()},
                       {"l2Frames", frames}, {"levels", levels}, {"reps", reps},
                       {"applyNsPerLevelMedian", perLevelTotal}, {"decimalPassNsPerLevelMedian", perLevelPass},
                       {"decimalPassShareOfApply", perLevelPass / perLevelTotal}})
                     .dump()
              << "\n";
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc >= 2 && std::string(argv[1]) == "sink")
            return sinkBench(argc > 2 ? std::atoi(argv[2]) : 400'000, argc > 3 ? std::atoi(argv[3]) : 10);
        if (argc >= 6 && std::string(argv[1]) == "feed")
            return feedBench(argv[2], argv[3], argv[4], std::atoi(argv[5]), argc > 6 ? std::atoi(argv[6]) : 6);
        std::cerr << "usage: levelstore_bench sink [messages] [pairs] | feed RAW PRODUCT FROM_ISO HOURS [reps]\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "levelstore_bench: " << e.what() << '\n';
        return 1;
    }
}
