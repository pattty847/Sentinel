/*
Sentinel — levelstore (Architecture C), LevelRecorder
Role: the exact native-level consumer of one product-day's history BookRecorder
(RecorderSink). It records the recorder's decisions, never its own reading of the
market: what the recorder applied is a level change, what it invalidated is a clear,
what it observes is observed. Output: one BookImage at the day floor, then one
MinuteBlock per finalised minute in [floorMs, ceilingMs), to a LevelOutput.
Open state (the book plus the unfinished minute) exports to versioned, checksummed
bytes at the recorder's drained checkpoint and imports into a fresh instance.
Threading: RecorderSink callbacks on the recorder worker; exportState/importState
and the accessors on the producer thread while the recorder is drained (between
them the worker does not touch this object). Never throws from a callback.
*/
#pragma once

#include "LevelTimeline.hpp"
#include "servermodel/BookRecorder.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sentinel::levelstore {

// The product-day a LevelRecorder belongs to, fixed at construction (roll() makes
// one per day's history recorder, see Roller.cpp makeLevel).
struct DayBinding {
    std::string symbol;
    int64_t floorMs = 0;   // first output bucket: the day start, or the commit floor
    int64_t ceilingMs = 0; // exclusive end of output buckets
    int priceDigits = -1;  // the recorder's priceScale is 10^priceDigits; -1: not decimal
    bool operator==(const DayBinding &) const = default;
};

// Receives C's output on the recorder worker. A throw is a C failure (see
// LevelRecorder::throwIfFailed), never propagated from a sink callback. A throw
// may follow partial output: abandon/reset the uncertain attempt before replay.
// This interface provides neither atomic publication nor durable recovery.
class LevelOutput {
  public:
    virtual ~LevelOutput() = default;
    virtual void onDayStart(const DayBinding &, const BookImage &) = 0;
    virtual void onBlock(const DayBinding &, const MinuteBlock &) = 0;
};

class LevelRecorder final : public recording::RecorderSink {
  public:
    // Unsupported is an input episode; Failed/RestoreRefused permanently stop the instance.
    enum class Unobserved : uint8_t { None = 0, Unsupported = 1, Failed = 2, RestoreRefused = 3 };
    struct Stats {
        uint64_t messages = 0;          // timeline messages recorded (all buckets)
        uint64_t changes = 0;           // level changes recorded
        uint64_t blocks = 0;            // minute blocks emitted
        uint64_t resets = 0;            // snapshot resets recorded
        uint64_t clears = 0;            // clears recorded
        uint64_t unsupportedPrices = 0; // THE counter: input levels whose price C cannot represent
        uint64_t unsupportedEpisodes = 0;
        uint64_t failures = 0;
        uint64_t restoreRefusals = 0;
        uint64_t foreignEvents = 0;     // events for another symbol, ignored
        bool operator==(const Stats &) const = default;
    };
    static constexpr uint16_t kStateVersion = 5;

    LevelRecorder(DayBinding binding, LevelOutput *output);

    void onReset(const std::string &symbol, int64_t clock) noexcept override;
    void onImprecise(const std::string &symbol, int64_t clock, const LevelChange &level) noexcept override;
    void onLevel(const std::string &symbol, int64_t clock, const LevelChange &level) noexcept override;
    void onApplied(const std::string &symbol, int64_t clock, bool valid, bool observing) noexcept override;
    void onInvalid(const std::string &symbol, int64_t clock, const std::string &reason,
                   bool upstream) noexcept override;
    void onMinute(const std::string &symbol, int64_t bucketStartMs, uint32_t observedMs,
                  uint32_t flags) noexcept override;

    // Checkpoint, producer thread, recorder drained (BookRecorder::exportState drains):
    // recorderClock is BookRecorder::checkpointClock(symbol) and recorderState the
    // bytes exportState() returned at this same drained position (INV-192). The blob
    // binds to both, so it can only be imported next to that recorder state.
    // Throws on terminal failure, or if called inside a message or with a clock behind C's.
    std::vector<uint8_t> exportState(std::optional<int64_t> recorderClock,
                                     const std::vector<uint8_t> &recorderState) const;
    // Restore into a fresh instance, before the restored recorder processes anything.
    // Never throws. Invalid/unpaired state or a non-fresh target permanently fails
    // this instance. No output or snapshot recovery follows a refused import.
    struct ImportResult {
        bool ok = false;
        std::string reason;
    };
    ImportResult importState(const std::vector<uint8_t> &blob, std::optional<int64_t> recorderClock,
                             const std::vector<uint8_t> &recorderState) noexcept;

    // Producer thread while drained. Failure is terminal; book/openBlock after a
    // failure are diagnostic partial state, not a usable replay boundary.
    bool failed() const { return unobserved_ == Unobserved::Failed || unobserved_ == Unobserved::RestoreRefused; }
    void throwIfFailed() const; // caller must check after drain, before reporting progress
    const DayBinding &binding() const { return binding_; }
    const Stats &stats() const { return stats_; }
    Unobserved unobserved() const { return unobserved_; }
    bool observed() const { return observed_; }
    bool recorderValid() const { return valid_; }
    bool dayStarted() const { return dayStarted_; }
    size_t liveLevels() const { return book_[0].size() + book_[1].size(); }
    std::vector<BookLevel> book() const; // sorted as BookImage::levels
    const MinuteBlock &openBlock() const { return open_; }
    // Deterministic allocation-failure seam, tests only: called at the allocating sites
    // of the clear and output paths ("clear", "changes"); true throws
    // std::bad_alloc there. Unset in production: one empty-function check per site.
    std::function<bool(const char *site)> failAllocationForTest;
    size_t memoryBytes() const; // estimate of the open state's heap use

  private:
    struct Slot {
        double size = 0;
        uint32_t touched = 0; // message serial of the last touch
    };
    struct Touch {
        bool isBid;
        int64_t units;
        double before;
    };
    using Side = std::unordered_map<int64_t, Slot>;

    bool mine(const std::string &symbol) noexcept;
    void begin(int64_t clock);
    void touch(bool isBid, int64_t units, double size);
    void unsupported(const LevelChange &level, int64_t clock);
    void endMessage(int64_t clock, bool valid, bool observing, bool invalidated);
    bool readerHadLevels() const noexcept; // inside a message: did the published book hold a level?
    void startDay();
    void fail(const char *what, int64_t clock) noexcept;
    void allocationSeam(const char *site);
    void clearBook() noexcept;

    DayBinding binding_;
    LevelOutput *output_;
    Side book_[2]; // [0] asks, [1] bids (index = isBid)
    std::vector<Touch> touched_;
    uint32_t serial_ = 0;
    bool inMessage_ = false, messageReset_ = false, messageUnsupported_ = false;
    bool dayStarted_ = false, valid_ = false, recorderObserving_ = false, observed_ = false;
    Unobserved unobserved_ = Unobserved::None;
    char failureReason_[192] = {}; // latched without allocation on the noexcept failure path
    int64_t lastClock_ = INT64_MIN;
    MinuteBlock open_;
    Stats stats_;
};

const char *unobservedName(LevelRecorder::Unobserved u) noexcept;
// "97.10000000000001" from its PriceDecimal; a description when no exact text exists.
std::string decimalText(const recording::RecorderSink::LevelChange &level);

} // namespace sentinel::levelstore
