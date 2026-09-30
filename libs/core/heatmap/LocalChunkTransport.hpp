#pragma once
#include "ChunkTransport.hpp"
#include <QThread>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace recording { class Hmc2Reader; }

namespace heatmap {
// Read-only HMC2 lab source. All availability scans, builds and hashing run on
// one worker; the Hmc2Reader is constructed and destroyed there. No writer lock.
class LocalChunkTransport final : public ChunkTransport {
    Q_OBJECT
public:
    // Worker-thread fault/latch seams for deterministic tests; empty in production.
    struct TestHooks {
        int pollIntervalMs = 1000; // <= 0 disables automatic scans
        std::function<void(recording::Hmc2Reader &, std::stop_token)> beforeAvailability;
        std::function<void(recording::Hmc2Reader &, std::stop_token)> beforeBuild;
    };
    LocalChunkTransport(std::filesystem::path root, TestHooks hooks, QObject *parent = nullptr);
    explicit LocalChunkTransport(std::filesystem::path root, QObject *parent = nullptr);
    ~LocalChunkTransport() override;
    // Start after attaching the fetcher. Polls watched symbols once per second;
    // refreshAvailability is also an explicit, asynchronous deterministic seam.
    void start(std::vector<std::string> symbols);
    void refreshAvailability(const std::string &symbol);
    quint64 request(const std::string &symbol, const std::string &source, int64_t levelMs,
                    std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) override;
private:
    struct Worker;
    std::stop_source stop_;
    QThread workerThread_;
    Worker *worker_ = nullptr;
    QTimer *poll_ = nullptr;
    std::vector<std::string> symbols_;
    quint64 nextRequest_ = 0;
    void refreshAvailability(const std::string &symbol, bool force);
};
} // namespace heatmap
