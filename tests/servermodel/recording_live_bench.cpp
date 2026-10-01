// Reproducible worker cost measurement; no exchange or socket cost; disk warmup is excluded.
#include "servermodel/RecordingLive.hpp"
#include "servermodel/BookRecorder.hpp"
#include "protocol/RecordingHistoryWire.hpp"
#include <QTemporaryDir>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <thread>
#include <random>
#include <algorithm>
#include <cmath>
using namespace recording;
namespace {
int sourceEntries = 12000;
int nativeRows = 20480;
RecordPtr sample(int minute, int observed, bool provisional) {
    auto r = std::make_shared<Hmc2Record>();
    r->header.symbol = "BENCH-USD";
    r->header.layer = "deep";
    r->header.rowTickUnits = 1000;
    r->bucketStartMs = kHmc2MinMs + minute * 60000;
    r->observedMs = observed;
    r->committedThroughMs = kHmc2MinMs + (provisional ? minute : minute + 1) * 60000;
    r->flags = provisional ? kProvisional : 0;
    r->bidRowLo = r->askRowLo = 0;
    r->bidRowHi = r->askRowHi = nativeRows - 1;
    for (int i = 0; i < sourceEntries; ++i) {
        const auto code = encodeSize((i % 97 + 1) * 0.125);
        r->entries.push_back({i, i % 2 != 0, code, code});
    }
    return r;
}
}
// Recorder-thread cost of one open-minute publication (publishOpen: copy and
// encode every layer's in-window rows, hand off to LiveCache), BTC-like book:
// mid $84,800, near $1 rows +-5% about half full (recorded minutes hold ~4,500
// near entries), deep $5 rows over [0.25x, 4x] with ~18,000 occupied (recorded:
// ~18,350), two $1 levels per occupied deep row so the near layer also tracks
// every far row it must skip. Two recorders get the same book and the same
// ticks every 500 ms; only one has a publisher. The difference per tick is the
// publication cost; minute closes (disk) are outside the timed loops.
int publishBench(int minutes) {
    constexpr double mid = 84'800;
    std::vector<recording::Level> book;
    std::mt19937 random(11);
    std::uniform_real_distribution<double> size(0.001, 2.0), coin(0, 1);
    for (double p = mid * 0.95; p <= mid * 1.05; p += 1)
        if (coin(random) < 0.53) book.push_back({p < mid, std::floor(p) + 0.37, size(random)});
    for (double p = mid * 0.25; p <= mid * 4; p += 5) {
        if (std::abs(p - mid) <= mid * 0.05 || coin(random) >= 0.28) continue;
        book.push_back({p < mid, std::floor(p) + 1.13, size(random)});
        book.push_back({p < mid, std::floor(p) + 3.61, size(random)});
    }
    struct Run {
        QTemporaryDir dir;
        LiveCache cache;
        int64_t local = 0;
        uint64_t publications = 0, entries = 0;
        std::unique_ptr<BookRecorder> recorder;
        double timedMs = 0;
        int64_t ticks = 0;
    };
    auto make = [&](Run& run, bool publish) {
        RecorderConfig c;
        c.root = run.dir.path().toStdString();
        c.priceScale = 100;
        c.sizeScale = {1e-8, 819};
        c.layers = {{"near", 100, 0.95, 1.05, false}, {"deep", 500, 0.25, 4.0, true}};
        c.livePublishMs = 500;
        if (publish) c.publisher = [&run](RecordPtr r) {
            ++run.publications; run.entries += r->entries.size(); run.cache.publish(std::move(r));
        };
        run.recorder = std::make_unique<BookRecorder>(std::move(c), [&run] { return run.local; });
        run.local = kHmc2MinMs;
        run.recorder->onSnapshot("BTC-USD", run.local, book);
        run.recorder->drainForTest();
    };
    Run with, without;
    make(with, true);
    make(without, false);
    std::vector<double> perTick;
    for (int m = 0; m < minutes; ++m) {
        double ms[2] = {0, 0};
        for (int which = 0; which < 2; ++which) { // alternate order against drift
            Run& run = (m + which) % 2 ? without : with;
            const int64_t minute = kHmc2MinMs + m * 60'000;
            const auto start = std::chrono::steady_clock::now();
            for (int k = 1; k < 120; ++k) {
                run.local = minute + k * 500;
                run.recorder->onTick(run.local);
                run.recorder->drainForTest();
            }
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            run.timedMs += elapsed; run.ticks += 119;
            ms[&run == &with ? 0 : 1] = elapsed;
            run.local = minute + 60'000 + 100; // close the minute (disk), untimed
            run.recorder->onTick(run.local);
            run.recorder->drainForTest();
        }
        perTick.push_back((ms[0] - ms[1]) / 119);
    }
    std::sort(perTick.begin(), perTick.end());
    const double median = perTick[perTick.size() / 2];
    std::cout << "book levels=" << book.size() << " publications=" << with.publications
              << " entries/publication(both layers)=" << with.entries / std::max<uint64_t>(1, with.publications / 2) << "\n"
              << std::fixed << std::setprecision(3)
              << "tick ms with publisher=" << with.timedMs / with.ticks << " without=" << without.timedMs / without.ticks << "\n"
              << "publishOpen ms (median of " << minutes << " minutes)=" << median
              << " min=" << perTick.front() << " max=" << perTick.back() << "\n"
              << "recorder-thread CPU at 2 Hz=" << median * 2 / 10 << "% of a core\n";
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--publish")
        return publishBench(argc > 2 ? std::clamp(std::atoi(argv[2]), 1, 60) : 9);
    if (argc > 1 && std::string_view(argv[1]) == "--raw") {
        const int iterations = argc > 2 ? std::clamp(std::atoi(argv[2]), 2, 1000) : 50;
        std::cout << "raw tail: 12000 shuffled entries/minute; sort, validation, hash and zstd; no disk/TLS\n"
                     "pending_minutes clients encodings/update ms/update bytes/frame\n";
        for (int pending : {0, 2, 60}) for (int clients : {1, 8}) {
            LiveCache cache;
            RawTailBuilder builder;
            std::mt19937 random(7);
            auto publish = [&](int minute, int observed) {
                auto record = std::make_shared<Hmc2Record>(*sample(minute, observed, true));
                record->committedThroughMs = kHmc2MinMs;
                std::shuffle(record->entries.begin(), record->entries.end(), random);
                cache.publish(record);
            };
            for (int i = 0; i < pending; ++i) publish(i, 60000);
            double elapsed = 0;
            size_t bytes = 0;
            for (int i = 0; i <= iterations; ++i) {
                publish(pending, 1000+i);
                const auto snapshot = cache.snapshot("BENCH-USD", "deep");
                const auto start = std::chrono::steady_clock::now();
                for (int c = 0; c < clients; ++c) {
                    const auto result = builder.build("BENCH-USD", "hmc2.deep", snapshot, kHmc2MinMs);
                    bytes = result.bytes->size()+14;
                }
                if (i) elapsed += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            }
            std::cout << pending << ' ' << clients << ' ' << double(builder.encodings())/(iterations+1) << ' '
                      << std::fixed << std::setprecision(3) << elapsed/iterations << ' ' << bytes << '\n';
        }
        return 0;
    }
    const int iterations = argc > 1 ? std::clamp(std::atoi(argv[1]), 2, 30) : 5;
    if (argc > 2) { sourceEntries = 262144; nativeRows = 262144; }
    std::cout << "fixture=" << sourceEntries << " source entries/minute, 2048 display rows, "
              << nativeRows / 2048 << " native rows/display row\n"
                 "five persisted fixture minutes span previous/current buckets; warmup excluded; delivery includes per-client JSON/base64 encoding, no socket/TLS\n";
    std::cout << "tf_ms clients builds/update project_ms/update encode_ms/update total_worker_ms/update cadence_s/update bytes/update\n";
    for (const int64_t tf : {60000, 300000, 86400000}) for (const int clients : {1, 8}) {
        QTemporaryDir dir;
        Hmc2Store store(dir.path().toStdString());
        LiveService service(dir.path().toStdString());
        std::mutex mutex;
        std::condition_variable wake;
        uint64_t received = 0, bytes = 0;
        const int base = tf / 60000;
        auto commit = [&](int m) {
            const auto r = sample(m, 60000, false);
            store.append(*r); // retained mailbox may evict dense committed records
            service.publish(r);
        };
        commit(base - 1);
        for (int m = 0; m < 4; ++m) commit(base + m);
        service.publish(sample(base + 4, 10000, true));
        std::vector<std::shared_ptr<LiveService::Subscription>> subscriptions;
        for (int c = 0; c < clients; ++c) {
            LiveView view{"BENCH-USD", "deep", tf, {0, 10.0 * nativeRows / 2048, 2048}, static_cast<uint64_t>(c)};
            subscriptions.push_back(service.subscribe(view, [&](const auto& v, const auto& page) {
                if (page.status != BuildStatus::Complete) throw std::runtime_error(page.message);
                const auto payload = protocol::recordingwire::buildLive(v, page).dump();
                { std::lock_guard lock(mutex); ++received; bytes += payload.size(); }
                wake.notify_one();
                return true;
            }));
        }
        auto round = [&](int step) {
            service.publish(sample(base + 4, 10000 + step * 1000, true));
            const auto expected = static_cast<uint64_t>((step + 1) * clients);
            std::unique_lock lock(mutex);
            if (!wake.wait_for(lock, std::chrono::seconds(step == 0 ? 90 : 15), [&] { return received >= expected; }))
                throw std::runtime_error("live measurement timed out");
            lock.unlock();
            while (service.diagnostics().deliveries < expected) std::this_thread::yield();
        };
        round(0);
        const auto initial = service.diagnostics();
        const auto initialBytes = bytes;
        const auto start = std::chrono::steady_clock::now();
        for (int step = 1; step <= iterations; ++step) round(step);
        service.shutdown();
        const auto end = service.diagnostics();
        const double projectMs = (end.buildMicros - initial.buildMicros) / (1000.0 * iterations);
        const double encodeMs = (end.deliveryMicros - initial.deliveryMicros) / (1000.0 * iterations);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / iterations;
        std::cout << tf << ' ' << clients << ' ' << double(end.builds - initial.builds) / iterations << ' '
                  << std::fixed << std::setprecision(3) << projectMs << ' ' << encodeMs << ' '
                  << projectMs + encodeMs << ' ' << seconds << ' ' << (bytes - initialBytes) / iterations << '\n';
    }
}
