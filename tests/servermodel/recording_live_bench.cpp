// Reproducible worker cost measurement; no exchange or socket cost; disk warmup is excluded.
#include "servermodel/RecordingLive.hpp"
#include "protocol/RecordingHistoryWire.hpp"
#include <QTemporaryDir>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <thread>
#include <random>
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
int main(int argc, char** argv) {
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
