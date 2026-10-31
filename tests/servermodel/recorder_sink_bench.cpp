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
    void onReset(const std::string &, int64_t) noexcept override {}
    void onImprecise(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onLevel(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onApplied(const std::string &, int64_t, bool, bool) noexcept override {}
    void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
    void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override {}
};
struct Counting final : RecorderSink {
    bool timing = false;
    uint64_t levels = 0, messages = 0, minutes = 0;
    void onReset(const std::string &, int64_t) noexcept override {}
    void onImprecise(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onLevel(const std::string &, int64_t, const LevelChange &) noexcept override { levels += timing; }
    void onApplied(const std::string &, int64_t, bool, bool) noexcept override { messages += timing; }
    void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
    void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override { minutes += timing; }
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
    const int messages = argc > 1 ? std::atoi(argv[1]) : 400'000, pairs = argc > 2 ? std::atoi(argv[2]) : 10;
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
    // Worker CPU of the timed update stream only (the snapshot is outside it).
    const auto once = [&](RecorderSink *sink, Counting *counting) {
        QTemporaryDir dir;
        RecorderConfig c;
        c.root = dir.path().toStdString();
        c.priceScale = 100;
        c.layers = {{"near", 100, .95, 1.05, false}, {"deep", 500, .25, 4., true}};
        c.sink = sink;
        BookRecorder r(c);
        const int64_t t0 = kHmc2MinMs + 86'400'000;
        r.onSnapshotAt("BTC-USD", t0, t0, snapshot);
        r.drainForTest();
        if (counting) counting->timing = true;
        const int64_t before = threadCpuNs("book-recorder");
        for (int i = 0; i < messages; ++i) {
            const int64_t t = t0 + int64_t(i) * 5; // 200 messages per second
            r.onUpdatesAt("BTC-USD", t, t, stream[size_t(i)]);
        }
        r.drainForTest();
        return threadCpuNs("book-recorder") - before;
    };
    Counting counting; // the stream is deterministic: count once, time with an empty sink
    once(&counting, &counting);
    Empty empty;
    std::vector<double> deltas;
    std::cout << "messages=" << messages << " timedLevelChanges=" << counting.levels
              << " timedAppliedMessages=" << counting.messages << " timedMinutes=" << counting.minutes << "\n"
              << "pair,order,nullMs,emptySinkMs,deltaNsPerLevel\n";
    for (int p = 0; p < pairs; ++p) {
        int64_t a = 0, b = 0;
        if (p % 2 == 0) { a = once(nullptr, nullptr); b = once(&empty, nullptr); }
        else { b = once(&empty, nullptr); a = once(nullptr, nullptr); }
        const double delta = double(b - a) / double(counting.levels);
        deltas.push_back(delta);
        std::cout << p << "," << (p % 2 ? "sink-first" : "null-first") << "," << a / 1e6 << "," << b / 1e6 << ","
                  << delta << "\n";
    }
    std::sort(deltas.begin(), deltas.end());
    const auto q = [&](double f) { return deltas[size_t(f * double(deltas.size() - 1) + 0.5)]; };
    std::cout << "deltaNsPerLevel median=" << q(0.5) << " q25=" << q(0.25) << " q75=" << q(0.75)
              << " min=" << deltas.front() << " max=" << deltas.back() << "\n";
}
