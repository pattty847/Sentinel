// Interface cost of RecorderConfig::sink on the recorder worker: a fixed
// synthetic update stream, sink null vs an empty sink, alternating runs.
// Prints worker CPU per accepted level change. Not a ctest.
#include "servermodel/BookRecorder.hpp"
#include <QTemporaryDir>
#include <mach/mach.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>

using namespace recording;
namespace {
struct Empty final : RecorderSink {
    uint64_t levels = 0;
    void onReset(const std::string &, int64_t) noexcept override {}
    void onLevel(const std::string &, int64_t, const LevelChange &) noexcept override { ++levels; }
    void onApplied(const std::string &, int64_t, bool, bool) noexcept override {}
    void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
    void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override {}
};
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
} // namespace

int main(int argc, char **argv) {
    const int messages = argc > 1 ? std::atoi(argv[1]) : 400'000, rounds = argc > 2 ? std::atoi(argv[2]) : 5;
    // BTC-like book: 1-cent units around 100k, 16 levels per update within +-2%.
    std::mt19937_64 rng(7);
    std::vector<std::vector<Level>> stream(static_cast<size_t>(messages));
    for (auto &m : stream) {
        m.resize(16);
        for (auto &l : m) {
            const bool bid = rng() & 1;
            const int64_t off = int64_t(rng() % 200'000) + 1;
            l = {bid, (bid ? 10'000'000 - off : 10'000'000 + off) / 100.0, (rng() % 4) ? double(rng() % 1000) / 1000 : 0.0};
        }
    }
    std::vector<Level> snapshot;
    for (int64_t i = 1; i <= 20'000; ++i) {
        snapshot.push_back({true, (10'000'000 - i * 10) / 100.0, 1.0});
        snapshot.push_back({false, (10'000'000 + i * 10) / 100.0, 1.0});
    }
    std::vector<double> nsNull, nsEmpty;
    uint64_t levels = 0;
    for (int round = 0; round < 2 * rounds; ++round) {
        const bool withSink = round % 2;
        QTemporaryDir dir;
        RecorderConfig c;
        c.root = dir.path().toStdString();
        c.priceScale = 100;
        c.layers = {{"near", 100, .95, 1.05, false}, {"deep", 500, .25, 4., true}};
        Empty sink;
        if (withSink) c.sink = &sink;
        int64_t cpu = 0;
        {
            BookRecorder r(c);
            const int64_t t0 = kHmc2MinMs + 86'400'000;
            r.onSnapshotAt("BTC-USD", t0, t0, snapshot);
            r.drainForTest();
            const int64_t before = threadCpuNs("book-recorder");
            for (int i = 0; i < messages; ++i) {
                const int64_t t = t0 + int64_t(i) * 5; // 200 messages per second
                r.onUpdatesAt("BTC-USD", t, t, stream[size_t(i)]);
            }
            r.drainForTest();
            cpu = threadCpuNs("book-recorder") - before;
        }
        if (withSink) levels = sink.levels;
        (withSink ? nsEmpty : nsNull).push_back(double(cpu));
    }
    std::sort(nsNull.begin(), nsNull.end());
    std::sort(nsEmpty.begin(), nsEmpty.end());
    const double mNull = nsNull[nsNull.size() / 2], mEmpty = nsEmpty[nsEmpty.size() / 2];
    std::cout << "messages=" << messages << " acceptedLevelChanges=" << levels << " rounds=" << rounds << "\n"
              << "workerCpuMs null median=" << mNull / 1e6 << " min=" << nsNull.front() / 1e6 << " max=" << nsNull.back() / 1e6 << "\n"
              << "workerCpuMs emptySink median=" << mEmpty / 1e6 << " min=" << nsEmpty.front() / 1e6 << " max=" << nsEmpty.back() / 1e6 << "\n"
              << "ns per accepted level change: null=" << mNull / double(levels) << " emptySink=" << mEmpty / double(levels)
              << " delta=" << (mEmpty - mNull) / double(levels) << "\n";
}
