#pragma once
#include "Hmc2Store.hpp"
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sentinel::metrics { class Counter; }

namespace recording {
struct LayerConfig {
    std::string name;
    int64_t rowTickUnits;
    double lowFrac;
    double highMult;
    bool hourlyRollup;
};
// The recorder's own book, observation and integration-clock decisions, called
// synchronously on the worker thread in decision order. `clock` is the symbol's
// integration clock (never decreases; backward and late envelopes are clamped).
// Order within one message: onMinute for each minute advance() finishes, then:
//   Tick: nothing more. Invalid: onInvalid(upstream=true).
//   Snapshot/update applied: [onReset] then per input level [onImprecise] [onLevel],
//   then exactly one of onApplied or onInvalid. A malformed batch gets onInvalid
//   only, before any level; a one-sided or crossed snapshot, or a row overflow,
//   gets onInvalid after its levels. A one-sided or crossed update only stops
//   observation (onApplied observing=false). An update while invalid: nothing.
//   A rejected timestamp or a worker exception invalidates every symbol
//   (onInvalid upstream=false), possibly after some of the message's onLevel calls.
// onLevel reports each accepted change (size differs) after the book holds it.
// onReset and onInvalid clear the book; after onInvalid nothing integrates until
// an accepted snapshot. Time integrates only while observing (onApplied).
// onMinute fires after the minute is finalised (pending record built, clock and
// minute advanced), before the next minute's events: once per bucket at most. A
// failed finalisation reports onInvalid instead and the minute stays open. Minutes
// it never reports observed nothing. importState and forkLead emit nothing.
// Implementations must not throw or block and must outlive the recorder.
//
// Price domain: binary64 prices parsed from exchange decimal text and a priceScale
// that is an exactly representable integer (the grid uses 10^k <= 1e15). Levels key
// on llround(price * priceScale). The recorder sees only the double, so decimal-text
// exactness ("99.0000000000000001" parses to 99.0) can only be judged where the text
// exists, before parsing. pricePrecision() judges the double: OnGrid never flags an
// on-grid decimal; for units < kPriceUnitsDomain every price more than 2^-9 of a unit
// off the grid is OffGrid; units >= kPriceUnitsDomain are OutsideDomain (unknown).
class RecorderSink {
  public:
    enum class Precision : uint8_t { OnGrid, OffGrid, OutsideDomain };
    struct LevelChange {
        bool isBid;
        int64_t units; // nearest price unit at priceScale, the book key
        double price;  // as received
        double size;   // new size, 0 removes the level
        Precision precision;
    };
    virtual ~RecorderSink() = default;
    virtual void onReset(const std::string &symbol, int64_t clock) noexcept = 0;
    // Every input level whose price is not OnGrid, before the zero-change check:
    // also when its rounded key collides with an equal-size level and the book
    // does not change.
    virtual void onImprecise(const std::string &symbol, int64_t clock, const LevelChange &level) noexcept = 0;
    virtual void onLevel(const std::string &symbol, int64_t clock, const LevelChange &level) noexcept = 0;
    virtual void onApplied(const std::string &symbol, int64_t clock, bool valid, bool observing) noexcept = 0;
    virtual void onInvalid(const std::string &symbol, int64_t clock, const std::string &reason,
                           bool upstream) noexcept = 0;
    // bucketStartMs + 60 s == clock. flags as HMC2 minute flags, without the
    // per-layer kUnderflow encoding bit.
    virtual void onMinute(const std::string &symbol, int64_t bucketStartMs, uint32_t observedMs,
                          uint32_t flags) noexcept = 0;
};
inline constexpr int64_t kPriceUnitsDomain = int64_t{1} << 40;
// An on-grid decimal's binary64 product is within 2^-52 |scaled| of its units
// (input and product rounding); the 2^-50 bound leaves a 4x margin.
inline RecorderSink::Precision pricePrecision(double price, int64_t units, double priceScale) {
    if (units >= kPriceUnitsDomain) return RecorderSink::Precision::OutsideDomain;
    const double scaled = price * priceScale;
    return std::abs(scaled - static_cast<double>(units)) <= std::ldexp(std::abs(scaled), -50)
               ? RecorderSink::Precision::OnGrid
               : RecorderSink::Precision::OffGrid;
}
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
    // Deterministic minute-finalisation failure seam (bucket start). Tests only.
    std::function<void(int64_t bucketStartMs)> beforeMinuteFinalizeForTest;
    // Continuous empty, one-sided or crossed books stay reconstructible but unobserved.
    // After this integration-clock duration, request a precautionary snapshot.
    int64_t oneSidedSuspectMs = 30'000;
    // Test synchronization only, called under the queue mutex before blocking.
    std::function<void()> beforeQueueWaitForTest;
    // Optional process counters; registry must outlive the recorder. Worker
    // increments only (one relaxed atomic), never registers or samples.
    sentinel::metrics::Counter* minuteColumnsCounter = nullptr;
    sentinel::metrics::Counter* writeErrorsCounter = nullptr;
    bool deterministicResume = false;
    std::string writerProduct; // opt-in product lease for independent shadow grids
    int64_t commitFloorMs = 0; // exclusive end <= floor: rebuild, do not append
    int64_t commitCeilingMs = kHmc2EndMs; // only complete buckets below this end
    // Publications the worker builds for the publisher. Finals: committed
    // minutes only (journal roller history, whose
    // forming minute comes from a lead fork). Lead: forming and finished-but-held
    // minutes only, never persisted or rolled up, with no committed cutoff
    // claim (committedThroughMs 0); normally created by forkLead().
    enum class Publication { Finals = 1, Lead = 2 };
    Publication publication = Publication::Finals;
    // Optional decision feed (RecorderSink). Not policy; forkLead does not copy it.
    RecorderSink *sink = nullptr;
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
    ~BookRecorder(); // Drains accepted work; drops the open minute and uncommitted lateness tail.
    BookRecorder(const BookRecorder &) = delete;
    BookRecorder &operator=(const BookRecorder &) = delete;
    void onSnapshotAt(const std::string &symbol, int64_t envelopeMs, int64_t localMs, std::vector<Level> levels);
    void onUpdatesAt(const std::string &symbol, int64_t envelopeMs, int64_t localMs, std::vector<Level> levels);
    // Cancel blocked journal admission before joining producers and destroying.
    // The destructor also requests stop; callers must not race object destruction.
    void requestStop();
    void drain(); // production fence: all preceding calls and durable writes completed
    void onInvalid(const std::string &symbol, int64_t localMs, std::string reason);
    void onTick(int64_t localNowMs);
    // Producer thread only: drains, then returns a Publication::Lead recorder
    // whose books and minute state equal this recorder's after every accepted
    // message. Feed it the same messages plus newer ones; it never writes.
    std::unique_ptr<BookRecorder> forkLead(std::function<void(std::shared_ptr<const Hmc2Record>)> publisher,
                                           int64_t livePublishMs);
    // Producer-only offline replay boundary. Export drains first; import requires
    // a fresh recorder and validates the complete candidate before installation.
    // Same policy/range/publication config, possibly a different output root.
    // Existing HMC2 prefix must be copied/present when continuing a recording.
    // No snapshot, tick, publication or resync event is generated by either call.
    // Empty export means diskErrors made this state ineligible for caching.
    std::vector<uint8_t> exportState();
    void importState(const std::vector<uint8_t>& state);
    struct Stats {
        uint64_t columnsWritten, lateEvents, backwardSteps, invalidations, diskErrors;
    };
    Stats stats() const;
    // Safe to query from chunk workers; one cutoff per persisted native level.
    Watermarks watermarks(const std::string &symbol, const std::string &layer) const;
    // Snapshot recovery for broken continuity, or precautionary recovery after
    // oneSidedSuspectMs. upstream means onInvalid (transport/journal); recorder
    // failures and suspicious continuous books use upstream=false. An accepted
    // snapshot clears any wait. A continuous book also clears its precautionary
    // wait when updates restore a two-sided, uncrossed, representable midpoint.
    // Thread-safe; reflects processed messages.
    struct SnapshotWait {
        std::string reason;
        bool upstream = false;
    };
    SnapshotWait snapshotWait(const std::string &symbol) const;
    // Waits for all preceding enqueues and disk writes, without advancing time.
    void drainForTest();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
