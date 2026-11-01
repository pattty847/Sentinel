// Costs for Architecture C E1b-1 (not a ctest).
//   levelstore_bench sink [messages] [pairs]
//     Recorder worker CPU per accepted level change: sink null vs LevelRecorder (output
//     discarded), the same synthetic stream, ABBA-interleaved pairs, as recorder_sink_bench.
//   levelstore_bench burst [messages] [reps]
//     One minute that grows to N messages: worker CPU per message, uninterrupted and
//     continued after an export/import; the minute's output must be identical.
//   levelstore_bench feed RAW_ROOT PRODUCT FROM_ISO HOURS [reps]
//     JournalFeed on real RAWL2 records held in memory: total apply time per L2 level;
//     the decimal pass alone over already parsed (cold) JSON; and, in context, the parse
//     plus parseSnapshot/parseUpdates loop with and without the decimal pass on the same
//     warm nodes, ABBA-interleaved (the added cost).
#include "LevelTestSupport.hpp"
#include "roller/JournalFeed.hpp"
#include "marketdata/dispatch/BookParser.hpp"
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

// One minute that keeps growing: N update messages, each adding a new level, all in
// one minute (the open block grows to N messages and N changes). Worker CPU of the
// first and second half, uninterrupted and with an export/import between the halves;
// the minute's output must be byte-identical.
int burstBench(int messages, int reps) {
    const int64_t t0 = kHmc2MinMs + 86'400'000;
    const auto once = [&](bool restore, double &firstNs, double &secondNs) {
        TempDir dir;
        RecorderConfig cfg;
        cfg.root = dir.path;
        cfg.priceScale = 100;
        cfg.layers = {{"near", 100, .95, 1.05, false}, {"deep", 500, .25, 4., true}};
        cfg.deterministicResume = true;
        Capture out;
        const DayBinding bind{"BTC-USD", t0, t0 + 86'400'000, 2};
        auto c = std::make_unique<LevelRecorder>(bind, &out);
        cfg.sink = c.get();
        auto r = std::make_unique<BookRecorder>(cfg);
        r->onSnapshotAt("BTC-USD", t0, t0, {L(true, "99000", 1), L(false, "101000", 1)});
        r->drainForTest();
        const auto feed = [&](int from, int to) {
            const int64_t before = threadCpuNs("book-recorder");
            for (int i = from; i < to; ++i) {
                const int64_t t = t0 + 1 + int64_t(i) * 50'000 / messages;
                r->onUpdatesAt("BTC-USD", t, t, {L(true, centsText(9'800'000 - i).c_str(), 1.0)});
            }
            r->drainForTest();
            return double(threadCpuNs("book-recorder") - before);
        };
        firstNs = feed(0, messages / 2);
        if (restore) {
            const auto bytes = r->exportState();
            const auto clock = r->checkpointClock("BTC-USD");
            const auto blob = c->exportState(clock, bytes);
            r.reset();
            c = std::make_unique<LevelRecorder>(bind, &out);
            cfg.sink = c.get();
            r = std::make_unique<BookRecorder>(cfg);
            r->importState(bytes);
            if (!c->importState(blob, r->checkpointClock("BTC-USD"), bytes).ok) throw std::runtime_error("import refused");
        }
        secondNs = feed(messages / 2, messages);
        r->onTick(t0 + 61'000);
        r->drainForTest();
        return out.bytes();
    };
    std::vector<double> a1, a2, b1, b2;
    bool identical = true;
    for (int i = 0; i < reps; ++i) {
        double f = 0, g = 0, h = 0, k = 0;
        const auto plain = once(false, f, g);
        const auto restored = once(true, h, k);
        identical = identical && plain == restored;
        a1.push_back(f / (messages / 2)); a2.push_back(g / (messages / 2));
        b1.push_back(h / (messages / 2)); b2.push_back(k / (messages / 2));
    }
    std::cout << json({{"bench", "burst"}, {"messagesInOneMinute", messages}, {"reps", reps},
                       {"uninterruptedFirstHalfNsPerMessage", median(a1)}, {"uninterruptedSecondHalfNsPerMessage", median(a2)},
                       {"restoredFirstHalfNsPerMessage", median(b1)}, {"afterRestoreSecondHalfNsPerMessage", median(b2)},
                       {"outputIdentical", identical}})
                     .dump()
              << "\n";
    return identical ? 0 : 3;
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
    // In context: JournalFeed's own per-frame work (JSON parse from the payload, then
    // parseSnapshot/parseUpdates), without and with the decimal pass on the same warm
    // nodes, in the same loop. The difference is the added cost.
    std::vector<std::string> payloads;
    for (const auto &rec : records)
        if (rec.record.kind == sentinel::capture::Kind::Frame && rec.record.payload.find("\"l2_data\"") != std::string::npos)
            payloads.push_back(rec.record.payload);
    const auto contextOnce = [&](bool decimals) {
        uint64_t sink = 0;
        std::vector<OrderBookLevel> bids, asks;
        std::vector<BookLevelUpdate> updates;
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto &payload : payloads) {
            const auto j = nlohmann::json::parse(payload);
            for (const auto &event : j["events"]) {
                if (event.value("product_id", "") != product) continue;
                const auto type = event.value("type", "");
                if (type == "snapshot") {
                    bids.clear();
                    asks.clear();
                    sink += sentinel::dispatch::parseSnapshot(event, bids, asks) + bids.size() + asks.size();
                    if (decimals)
                        for (const auto &u : event["updates"]) {
                            bool isBid; double price, size;
                            if (!sentinel::dispatch::parseLevel(u, isBid, price, size) || !(size > 0)) continue;
                            sink += parsePriceDecimal(u.find("price_level")->get_ref<const std::string &>()).digits;
                        }
                } else if (type == "update" && sentinel::dispatch::parseUpdates(event, updates)) {
                    sink += updates.size();
                    if (decimals) {
                        const auto &u = event["updates"];
                        for (size_t i = 0; i < updates.size(); ++i)
                            sink += parsePriceDecimal(u[i].find("price_level")->get_ref<const std::string &>()).digits;
                    }
                }
            }
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (!sink) std::cerr << "empty\n";
        return ns;
    };
    std::vector<double> total, pass, without, with, deltas;
    for (int i = 0; i < reps; ++i) {
        if (i % 2) { pass.push_back(decimalOnce()); total.push_back(feedOnce()); }
        else { total.push_back(feedOnce()); pass.push_back(decimalOnce()); }
        double a = 0, b = 0;
        if (i % 2) { b = contextOnce(true); a = contextOnce(false); }
        else { a = contextOnce(false); b = contextOnce(true); }
        without.push_back(a);
        with.push_back(b);
        deltas.push_back((b - a) / double(levels));
    }
    const double perLevelTotal = median(total) / double(levels), perLevelPass = median(pass) / double(levels);
    std::cout << json({{"bench", "feed"}, {"product", product}, {"from", from}, {"hours", hours}, {"records", records.size()},
                       {"l2Frames", frames}, {"levels", levels}, {"reps", reps},
                       {"applyNsPerLevelMedian", perLevelTotal}, {"decimalPassNsPerLevelMedian", perLevelPass},
                       {"decimalPassShareOfApply", perLevelPass / perLevelTotal},
                       {"inContextParseNsPerLevelMedian", median(without) / double(levels)},
                       {"inContextWithDecimalsNsPerLevelMedian", median(with) / double(levels)},
                       {"inContextPairedDeltaNsPerLevelMedian", median(deltas)},
                       {"inContextPairedDeltaShareOfApply", median(deltas) / perLevelTotal}})
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
        if (argc >= 2 && std::string(argv[1]) == "burst")
            return burstBench(argc > 2 ? std::atoi(argv[2]) : 40'000, argc > 3 ? std::atoi(argv[3]) : 3);
        if (argc >= 6 && std::string(argv[1]) == "feed")
            return feedBench(argv[2], argv[3], argv[4], std::atoi(argv[5]), argc > 6 ? std::atoi(argv[6]) : 6);
        std::cerr << "usage: levelstore_bench sink [messages] [pairs] | burst [messages] [reps] | feed RAW PRODUCT FROM_ISO HOURS [reps]\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "levelstore_bench: " << e.what() << '\n';
        return 1;
    }
}
