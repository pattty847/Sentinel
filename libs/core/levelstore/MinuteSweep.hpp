/*
Sentinel — levelstore (Architecture C), MinuteSweep
Role: derives per-minute metrics from C's exact timeline, with the Data Lab's
promised semantics (Sentinel-DataLab engine/src/ref.cpp, RESULTS.md "Summary"):
  integral  size x ms over observed time;
  presence  observed ms during which the bin holds size > 0;
  peak      max of the bin's size over every observed message state, each taken
            after the whole message (atomic), plus the observed state at the
            minute start. A state that a later message in the same millisecond
            replaces lasts 0 ms: it counts for peak and adds nothing to integral
            or presence. Sizes inside one message never count (the recorder
            applies a message atomically and C keeps only its final sizes);
  last      the bin's size at the minute end (the recorder's book: empty while
            the recorder is invalid, where the Data Lab froze the book);
  covered   observed ms of the minute.
Unobserved time accrues nothing. binUnits == 1 gives native levels with exact
sizes. binUnits > 1 groups levels into rows of binUnits units the way BookRecorder
does (a running long-double row sum reset to exactly 0 when the row's level count
reaches 0), so HMC2 rows can be checked against C.
Threading: single-threaded.
*/
#pragma once

#include "LevelTimeline.hpp"
#include <unordered_map>
#include <vector>

namespace sentinel::levelstore {

struct BinRow {
    bool isBid = false;
    int64_t bin = 0; // floorDiv(units, binUnits)
    long double integral = 0;
    int64_t presenceMs = 0;
    long double peak = 0;
    long double last = 0;
    bool operator==(const BinRow &) const = default;
};

class MinuteSweep {
  public:
    explicit MinuteSweep(int64_t binUnits = 1);
    void start(const BookImage &image);
    // Folds one block in order. rows: every bin with a nonzero metric, bids first,
    // ascending bin. A damaged block is read as unobserved (no integral, presence,
    // peak or covered; its changes and clears apply). A block's observedAtStart is
    // taken as given after a damaged block or a missing minute (C skips unobserved
    // minutes and loses a minute whose output failed); otherwise it must continue the
    // sweep, and a block that does not (time order, observation) throws std::runtime_error.
    void apply(const MinuteBlock &block, std::vector<BinRow> &rows, int64_t &coveredMs);
    bool observed() const { return observed_; }
    size_t levels() const { return levels_[0].size() + levels_[1].size(); }

  private:
    struct Bin {
        long double sum = 0, integral = 0, peak = 0;
        uint32_t count = 0;
        int64_t lastT = 0, presence = 0;
        uint64_t minute = 0, message = 0;
    };
    struct Key {
        bool isBid;
        int64_t bin;
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key &k) const { return std::hash<int64_t>{}(k.bin) ^ (k.isBid ? 0x9e3779b97f4a7c15ull : 0); }
    };
    Bin &at(const Key &k);
    void accrue(Bin &b, int64_t t) const;
    void accrueLive(int64_t t);
    Key keyOf(bool isBid, int64_t units) const;

    int64_t binUnits_;
    bool started_ = false, observed_ = false, damaged_ = false, previousDamaged_ = false;
    int64_t previousBucket_ = INT64_MIN;
    std::unordered_map<int64_t, double> levels_[2]; // [isBid]
    std::unordered_map<Key, Bin, KeyHash> bins_;
    uint64_t minute_ = 0, message_ = 0;
    int64_t minuteStart_ = 0;
    std::vector<Key> touched_;
};

} // namespace sentinel::levelstore
