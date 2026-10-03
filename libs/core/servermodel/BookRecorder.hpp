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
    // Open-minute publication interval (integration clock): at most one
    // provisional record per layer per interval, and none while an invalid
    // book's record is unchanged. Server config
    // recording.live_publish_ms (default 500, clamped to [250, 5000]).
    int64_t livePublishMs = 500;
    // Worker callback: bounded handoff only; no I/O or projection. Installed before start.
    std::function<void(std::shared_ptr<const Hmc2Record>)> publisher;
    // Deterministic allocation-failure seam, before building any publication
    // (open-minute records and pending/committed copies). Tests only.
    std::function<void(bool provisional)> beforePublicationForTest;
    // Worker callback: the recorder invalidated a symbol on its own (not via
    // onInvalid), or dropped its first snapshot, so only a fresh upstream snapshot
    // can resume it. Repeated while the symbol stays invalid, with a per-symbol
    // interval (local time) that starts at resnapshotIntervalMs, doubles up to
    // resnapshotMaxIntervalMs, and resets once a snapshot has stayed valid for
    // resnapshotStableMs. Must only hand off (queue); no I/O. Installed before start.
    std::function<void(const std::string &symbol, const std::string &reason)> onSelfInvalidated;
    // After all final publications/writes, on the recorder worker.
    std::function<void(const std::string &)> onReleased;
    int64_t resnapshotIntervalMs = 30'000;
    int64_t resnapshotMaxIntervalMs = 600'000;
    int64_t resnapshotStableMs = 60'000;
    // A valid book may stay one-sided this long (integration clock) before it is
    // invalidated as "one-sided book": a lasting empty side freezes the mid window.
    int64_t oneSidedGraceMs = 5'000;
    // Offline journal mode only. Live defaults and overloads are unchanged.
    bool blockingQueue = false;
    // Test synchronization only, called under the queue mutex before blocking.
    std::function<void()> beforeQueueWaitForTest;
    bool deterministicResume = false;
    int64_t commitFloorMs = 0; // exclusive end <= floor: rebuild, do not append
    int64_t commitCeilingMs = kHmc2EndMs; // only complete buckets below this end

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
        int64_t lastColumnMs = 0;    // newest minute bucket this layer appended, 0 = none
    };
    explicit BookRecorder(RecorderConfig cfg);
    // Deterministic clock injection for replay/tests; called on the producer only.
    BookRecorder(RecorderConfig cfg, std::function<int64_t()> localClock);
    ~BookRecorder(); // Drains accepted work; drops the open minute and uncommitted lateness tail.
    BookRecorder(const BookRecorder &) = delete;
    BookRecorder &operator=(const BookRecorder &) = delete;
    void onSnapshot(const std::string &symbol, int64_t envelopeMs, std::vector<Level> levels);
    void onUpdates(const std::string &symbol, int64_t envelopeMs, std::vector<Level> levels);
    void onSnapshotAt(const std::string &symbol, int64_t envelopeMs, int64_t localMs, std::vector<Level> levels);
    void onUpdatesAt(const std::string &symbol, int64_t envelopeMs, int64_t localMs, std::vector<Level> levels);
    // Cancel blocked offline admission before joining producers and destroying.
    // The destructor also requests stop; callers must not race object destruction.
    void requestStop();
    void drain(); // production fence: all preceding calls and durable writes completed
    void onInvalid(const std::string &symbol, int64_t localMs, std::string reason);
    // Ordered, lossless lifecycle control: commit observed tail and forget the symbol.
    void releaseSymbol(const std::string &symbol, int64_t localMs);
    void onTick(int64_t localNowMs);
    struct Stats {
        uint64_t columnsWritten, lateEvents, backwardSteps, queueDrops, invalidations, diskErrors;
    };
    Stats stats() const;
    // Safe to query from chunk workers; one cutoff per persisted native level.
    Watermarks watermarks(const std::string &symbol, const std::string &layer) const;
    // Waits for all preceding enqueues and disk writes, without advancing time.
    void drainForTest();
    size_t retainedSymbolStatesForTest(); // producer + worker symbol entries after drain

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
