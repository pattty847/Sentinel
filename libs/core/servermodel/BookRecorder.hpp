#pragma once
#include "Hmc2Store.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace recording {
struct LayerConfig {
    std::string name;
    int64_t rowTickUnits;
    double lowFrac;
    double highMult;
    bool hourlyRollup;
};
struct RecorderConfig {
    std::filesystem::path root;
    double priceScale = 100.0;
    SizeScale sizeScale;
    std::vector<LayerConfig> layers;
    int64_t latenessMs = 2000;
    size_t maxQueuedLevels = 2'000'000;
    // Worker callback: bounded handoff only; no I/O or projection. Installed before start.
    std::function<void(std::shared_ptr<const Hmc2Record>)> publisher;
    // Deterministic allocation-failure seam, before making a publication copy.
    std::function<void(bool provisional)> beforePublicationForTest;
};
struct Level {
    bool isBid;
    double price;
    double size;
};
class BookRecorder {
  public:
    struct Watermarks {
        int64_t minuteThroughMs = 0; // exclusive, lateness already applied
        int64_t hourThroughMs = 0;   // exclusive, after hour rollup persistence
    };
    explicit BookRecorder(RecorderConfig cfg);
    // Deterministic clock injection for replay/tests; called on the producer only.
    BookRecorder(RecorderConfig cfg, std::function<int64_t()> localClock);
    ~BookRecorder(); // Drains accepted work; drops the open minute and uncommitted lateness tail.
    BookRecorder(const BookRecorder &) = delete;
    BookRecorder &operator=(const BookRecorder &) = delete;
    void onSnapshot(const std::string &symbol, int64_t envelopeMs, std::vector<Level> levels);
    void onUpdates(const std::string &symbol, int64_t envelopeMs, std::vector<Level> levels);
    void onInvalid(const std::string &symbol, int64_t localMs, std::string reason);
    void onTick(int64_t localNowMs);
    struct Stats {
        uint64_t columnsWritten, lateEvents, backwardSteps, queueDrops, invalidations, diskErrors;
    };
    Stats stats() const;
    // Safe to query from chunk workers; one cutoff per persisted native level.
    Watermarks watermarks(const std::string &symbol, const std::string &layer) const;
    // Waits for all preceding enqueues and disk writes, without advancing time.
    void drainForTest();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
