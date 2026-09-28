// Reproducible worker cost measurement; no exchange, socket, or disk warmup cost.
#include "servermodel/RecordingLive.hpp"
#include "protocol/RecordingHistoryWire.hpp"
#include <QTemporaryDir>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <thread>
using namespace recording;
namespace {
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
    r->bidRowHi = r->askRowHi = 20479;
    for (int i = 0; i < 12000; ++i) {
        const auto code = encodeSize((i % 97 + 1) * 0.125);
        r->entries.push_back({2000 + i, i % 2 != 0, code, code});
    }
    return r;
}
}
int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::clamp(std::atoi(argv[1]), 2, 30) : 5;
    std::cout << "fixture=12000 source entries/minute, 2048 display rows, 10 native rows/display row\n"
                 "warmup excluded; delivery includes per-client JSON/base64 encoding, no socket/TLS\n";
    std::cout << "tf_ms clients builds/update project_ms/update encode_ms/update total_worker_ms/update cadence_s/update bytes/update\n";
    for (const int64_t tf : {60000, 300000}) for (const int clients : {1, 8}) {
        QTemporaryDir dir;
        LiveService service(dir.path().toStdString());
        std::mutex mutex;
        std::condition_variable wake;
        uint64_t received = 0, bytes = 0;
        std::vector<std::shared_ptr<LiveService::Subscription>> subscriptions;
        for (int c = 0; c < clients; ++c) {
            LiveView view{"BENCH-USD", "deep", tf, {0, 100, 2048}, static_cast<uint64_t>(c)};
            subscriptions.push_back(service.subscribe(view, [&](const auto& v, const auto& page) {
                if (page.status != BuildStatus::Complete) throw std::runtime_error(page.message);
                const auto payload = protocol::recordingwire::buildLive(v, page).dump();
                { std::lock_guard lock(mutex); ++received; bytes += payload.size(); }
                wake.notify_one();
                return true;
            }));
        }
        for (int m = 0; m < 4; ++m) service.publish(sample(m, 60000, false));
        auto round = [&](int step) {
            service.publish(sample(4, 10000 + step * 1000, true));
            const auto expected = static_cast<uint64_t>((step + 1) * clients);
            std::unique_lock lock(mutex);
            if (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return received >= expected; }))
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
