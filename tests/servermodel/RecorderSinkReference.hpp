#pragma once
// Reference RecorderSink consumer for tests and offline evidence. It rebuilds
// per-native-level piecewise-constant runs and per-minute covered time from sink
// events only, then checks them against the recorder's own HMC2 minute rows and
// exported book. It never reads the journal or recomputes the recorder clock.
#include "servermodel/BookRecorder.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace recording::sinkref {

class ReferenceConsumer final : public RecorderSink {
  public:
    struct Level {
        double size = 0;
        int64_t runStart = 0, since = 0;
        long double integral = 0; // size x observed ms within the open minute
    };
    struct Run {
        bool isBid;
        int64_t units, start, end;
        double size;
    };
    struct Book {
        std::map<int64_t, Level> side[2]; // [0] bids, [1] asks; size 0 entries hold a minute integral only
        bool valid = false, observing = false;
        int64_t clock = -1, obsSince = 0;
        uint64_t covered = 0; // observed ms since the last reported minute
    };
    struct Minute {
        uint32_t coveredMs = 0, eventObservedMs = 0, flags = 0;
        std::vector<std::tuple<bool, int64_t, long double>> integrals; // isBid, units, size x ms
    };
    struct Counts {
        uint64_t resets = 0, levels = 0, applied = 0, invalid = 0, upstreamInvalid = 0, minutes = 0,
                 offGrid = 0, outsideDomain = 0, runsClosed = 0, suspensions = 0, resumptions = 0, validations = 0,
                 minutesNotStored = 0;
    };

    bool keepRuns = false;
    int64_t storeFromMs = std::numeric_limits<int64_t>::min(); // minutes before this are checked, not stored
    std::vector<Run> runs;
    std::map<std::string, Book> books;
    std::map<std::pair<std::string, int64_t>, Minute> minutes; // finished, not yet verified
    Counts counts;
    std::vector<std::string> errors;

    void onReset(const std::string &symbol, int64_t clock) noexcept override {
        auto &b = at(symbol, clock);
        ++counts.resets;
        clearBook(b, clock);
    }
    void onImprecise(const std::string &, int64_t, const LevelChange &c) noexcept override {
        ++(c.precision == Precision::OutsideDomain ? counts.outsideDomain : counts.offGrid);
    }
    void onLevel(const std::string &symbol, int64_t clock, const LevelChange &c) noexcept override {
        auto &b = at(symbol, clock);
        ++counts.levels;
        const int side = c.isBid ? 0 : 1;
        auto [it, added] = b.side[side].try_emplace(c.units);
        auto &l = it->second;
        if (added) l.since = clock;
        accrue(b, l, clock);
        closeRun(c.isBid, c.units, l, clock);
        l.size = c.size;
        l.runStart = clock;
    }
    void onApplied(const std::string &symbol, int64_t clock, bool valid, bool observing) noexcept override {
        auto &b = at(symbol, clock);
        ++counts.applied;
        if (valid && !b.valid) ++counts.validations;
        if (observing != b.observing) {
            accrueAll(b, clock);
            if (b.observing) {
                b.covered += static_cast<uint64_t>(clock - b.obsSince);
                ++counts.suspensions;
            } else {
                b.obsSince = clock;
                ++counts.resumptions;
            }
        }
        b.observing = observing;
        b.valid = valid;
    }
    void onInvalid(const std::string &symbol, int64_t clock, const std::string &, bool upstream) noexcept override {
        auto &b = at(symbol, clock);
        ++counts.invalid;
        if (upstream) ++counts.upstreamInvalid;
        clearBook(b, clock);
        if (b.observing) b.covered += static_cast<uint64_t>(clock - b.obsSince);
        b.observing = b.valid = false;
    }
    void onMinute(const std::string &symbol, int64_t bucket, uint32_t observedMs, uint32_t flags) noexcept override {
        const int64_t end = bucket + 60'000;
        auto &b = at(symbol, end);
        ++counts.minutes;
        accrueAll(b, end);
        if (b.observing) {
            b.covered += static_cast<uint64_t>(end - b.obsSince);
            b.obsSince = end;
        }
        Minute m;
        m.coveredMs = static_cast<uint32_t>(b.covered);
        m.eventObservedMs = observedMs;
        m.flags = flags;
        if (b.covered != observedMs)
            errors.push_back("covered time " + std::to_string(b.covered) + " != observedMs " +
                             std::to_string(observedMs) + " bucket=" + std::to_string(bucket));
        b.covered = 0;
        for (int side = 0; side < 2; ++side)
            for (auto it = b.side[side].begin(); it != b.side[side].end();) {
                if (it->second.integral > 0) m.integrals.emplace_back(side == 0, it->first, it->second.integral);
                it->second.integral = 0;
                it = it->second.size > 0 ? std::next(it) : b.side[side].erase(it);
            }
        if (observedMs && bucket < storeFromMs) ++counts.minutesNotStored;
        else if (observedMs) minutes[{symbol, bucket}] = std::move(m);
    }

    // Open-minute state normalised to a checkpoint clock: the recorder's exported
    // clock, which can lead the last sink event (ticks and dropped updates move it
    // silently; observation cannot change without an event, so the gap accrues).
    struct Normalised {
        struct LevelState {
            bool isBid;
            int64_t units;
            double size;
            int64_t runStartAge;  // clock - run start (live levels)
            long double integral; // size x observed ms in the open minute, through clock
        };
        int64_t clock = 0, lagMs = 0; // lagMs: clock - last sink event clock
        std::vector<LevelState> live, removed;
        bool observing = false;
        int64_t observingForMs = 0; // clock - start of the current observing interval
        uint64_t coveredMs = 0;     // observed ms of the open minute, through clock
    };
    Normalised normalise(const std::string &symbol, int64_t clock) const {
        Normalised n;
        n.clock = clock;
        const auto it = books.find(symbol);
        if (it == books.end()) return n;
        const auto &b = it->second;
        n.lagMs = clock - b.clock;
        n.observing = b.observing;
        n.observingForMs = b.observing ? clock - b.obsSince : 0;
        n.coveredMs = b.covered + uint64_t(n.observingForMs);
        for (int sd = 0; sd < 2; ++sd)
            for (const auto &[units, l] : b.side[sd]) {
                const long double integral =
                    l.integral + (b.observing && clock > l.since ? static_cast<long double>(l.size) * (clock - l.since) : 0);
                if (l.size > 0) n.live.push_back({sd == 0, units, l.size, clock - l.runStart, integral});
                else if (integral > 0) n.removed.push_back({sd == 0, units, 0, 0, integral});
            }
        return n;
    }

    // Live levels (size > 0) per side, ascending units.
    std::vector<std::pair<int64_t, double>> live(const std::string &symbol, bool bids) const {
        std::vector<std::pair<int64_t, double>> out;
        const auto it = books.find(symbol);
        if (it == books.end()) return out;
        for (const auto &[units, l] : it->second.side[bids ? 0 : 1])
            if (l.size > 0) out.emplace_back(units, l.size);
        return out;
    }
    size_t liveCount(const std::string &symbol) const {
        return live(symbol, true).size() + live(symbol, false).size();
    }

  private:
    Book &at(const std::string &symbol, int64_t clock) {
        auto &b = books[symbol];
        if (clock < b.clock)
            errors.push_back("clock regression " + std::to_string(b.clock) + " -> " + std::to_string(clock));
        b.clock = std::max(b.clock, clock);
        return b;
    }
    static void accrue(const Book &b, Level &l, int64_t t) {
        if (b.observing && t > l.since) l.integral += static_cast<long double>(l.size) * (t - l.since);
        l.since = t;
    }
    static void accrueAll(Book &b, int64_t t) {
        for (auto &side : b.side)
            for (auto &[_, l] : side) accrue(b, l, t);
    }
    void closeRun(bool isBid, int64_t units, const Level &l, int64_t t) {
        if (l.size <= 0) return;
        ++counts.runsClosed;
        if (keepRuns) runs.push_back({isBid, units, l.runStart, t, l.size});
    }
    void clearBook(Book &b, int64_t t) {
        accrueAll(b, t);
        for (int side = 0; side < 2; ++side)
            for (auto it = b.side[side].begin(); it != b.side[side].end();) {
                closeRun(side == 0, it->first, it->second, t);
                it->second.size = 0;
                it = it->second.integral > 0 ? std::next(it) : b.side[side].erase(it);
            }
    }
};

// HMC2 row equation: for each minute row, sum over native levels in the row of
// size x observed ms / observedMs equals the entry. Both sides use the same
// encoder; the only tolerated difference is one code, from summation order (the
// recorder keeps a running row sum, this sums native levels), counted separately.
// A zero integral must encode as code 0 and a nonzero one as code >= 1. Peaks
// are not reconstructed: peak and underflow completeness stay unproven.
struct RowCheck {
    uint64_t records = 0, entries = 0, rowsCompared = 0, codeExact = 0, codeOffByOne = 0, codeMismatches = 0,
             zeroNonzeroMismatches = 0, missingEntries = 0, observedMismatches = 0, flagMismatches = 0,
             underflowRows = 0, underflowRecords = 0, underflowTieRecords = 0, underflowPeakOnlyRecords = 0,
             underflowMismatches = 0, unmatchedRecords = 0;
    double maxRelError = 0;
    std::vector<std::string> first;
    void note(const std::string &what) {
        if (first.size() < 8) first.push_back(what);
    }
    bool clean() const {
        return !codeMismatches && !zeroNonzeroMismatches && !missingEntries && !observedMismatches &&
               !flagMismatches && !underflowMismatches && !unmatchedRecords;
    }
    nlohmann::json json() const {
        return {{"records", records}, {"entries", entries}, {"rowsCompared", rowsCompared},
                {"codeExact", codeExact}, {"codeOffByOne", codeOffByOne}, {"codeMismatches", codeMismatches},
                {"zeroNonzeroMismatches", zeroNonzeroMismatches}, {"maxRelError", maxRelError},
                {"missingEntries", missingEntries}, {"observedMismatches", observedMismatches},
                {"flagMismatches", flagMismatches}, {"underflowRows", underflowRows},
                {"underflowRecords", underflowRecords}, {"underflowTieRecords", underflowTieRecords},
                {"underflowPeakOnlyRecordsUnproven", underflowPeakOnlyRecords},
                {"underflowMismatches", underflowMismatches}, {"unmatchedRecords", unmatchedRecords},
                {"first", first}};
    }
    // Minute records only.
    void check(const Hmc2Record &r, const ReferenceConsumer::Minute &m) {
        ++records;
        const auto at = r.header.layer + "@" + std::to_string(r.bucketStartMs);
        if (r.observedMs != m.eventObservedMs || r.observedMs != m.coveredMs) {
            ++observedMismatches;
            note("observed " + at);
        }
        if ((r.flags & ~kUnderflow) != m.flags) {
            ++flagMismatches;
            note("flags " + at + " hmc2=" + std::to_string(r.flags) + " sink=" + std::to_string(m.flags));
        }
        struct KeyHash {
            size_t operator()(const std::pair<int64_t, bool> &k) const {
                return std::hash<int64_t>{}(k.first) ^ (k.second ? size_t{0x9e3779b9} : 0);
            }
        };
        std::unordered_map<std::pair<int64_t, bool>, long double, KeyHash> rows;
        rows.reserve(m.integrals.size());
        for (const auto &[isBid, units, integral] : m.integrals)
            rows[{floorDiv(units, r.header.rowTickUnits), !isBid}] += integral;
        const auto inWindow = [&](int64_t row, bool ask) {
            return ask ? row >= r.askRowLo && row <= r.askRowHi : row >= r.bidRowLo && row <= r.bidRowHi;
        };
        const double floor = r.header.sizeScale.floor;
        bool underflow = false, tieRow = false, peakCode1 = false;
        for (const auto &e : r.entries) {
            ++entries;
            ++rowsCompared;
            const auto it = rows.find({e.row, e.isAsk});
            const long double expected = it == rows.end() ? 0 : it->second / r.observedMs;
            const int twap = e.twapCode & kMaxCode;
            if (expected > 0 && expected < floor) { underflow = true; ++underflowRows; }
            // Per row: the recorder's running row sum can sit just below the floor
            // where the native levels sum to exactly the floor; it then encodes code 1.
            if (twap == 1 && expected >= floor && expected <= floor * (1 + 1e-9)) tieRow = true;
            if ((e.peakCode & kMaxCode) == 1) peakCode1 = true;
            if ((expected == 0) != (twap == 0)) {
                ++zeroNonzeroMismatches;
                note("zero/nonzero " + at + " row=" + std::to_string(e.row) + " code=" + std::to_string(twap));
                continue;
            }
            const int code = encodeSize(static_cast<double>(expected), r.header.sizeScale);
            const int diff = std::abs(code - twap);
            if (diff == 0) ++codeExact;
            else if (diff == 1) ++codeOffByOne;
            else {
                ++codeMismatches;
                note("twap " + at + " row=" + std::to_string(e.row) + " ask=" + std::to_string(e.isAsk) +
                     " code=" + std::to_string(twap) + " expected=" + std::to_string(code));
            }
            if (expected >= floor) // below the floor the code saturates at 1
                maxRelError = std::max(maxRelError,
                    static_cast<double>(std::abs(decodeSize(e.twapCode, r.header.sizeScale) - expected) / expected));
        }
        for (const auto &[key, integral] : rows) {
            if (integral <= 0 || !inWindow(key.first, key.second)) continue;
            const std::pair<int64_t, bool> k{key.first, key.second};
            const auto pos = std::lower_bound(r.entries.begin(), r.entries.end(), k,
                [](const Hmc2Entry &a, const std::pair<int64_t, bool> &b) { return std::pair(a.row, a.isAsk) < b; });
            if (pos == r.entries.end() || pos->row != k.first || pos->isAsk != k.second) {
                ++missingEntries;
                note("missing " + at + " row=" + std::to_string(key.first));
            }
        }
        const bool hmc2Underflow = (r.flags & kUnderflow) != 0;
        if (hmc2Underflow) ++underflowRecords;
        if (underflow && !hmc2Underflow) {
            ++underflowMismatches;
            note("underflow missing " + at);
        } else if (hmc2Underflow && !underflow) {
            if (tieRow) ++underflowTieRecords;
            else if (peakCode1) ++underflowPeakOnlyRecords; // a peak below the floor: not reconstructed
            else { ++underflowMismatches; note("underflow unexplained " + at); }
        }
    }
};

// Recorder book equality from exportState(): units and exact sizes, plus validity.
inline std::string compareBook(const std::vector<uint8_t> &state, const std::string &symbol,
                               const ReferenceConsumer &c, size_t *levels = nullptr) {
    if (state.size() < 5) return "empty recorder export";
    const auto j = nlohmann::json::from_cbor(state.begin(), state.end() - 4);
    const auto &symbols = j.at("symbols");
    if (!symbols.contains(symbol)) return c.liveCount(symbol) ? "consumer has levels, recorder has no symbol" : "";
    const auto &s = symbols.at(symbol);
    for (const bool bids : {true, false}) {
        const auto &arr = s.at(bids ? "bids" : "asks");
        const auto mine = c.live(symbol, bids);
        if (arr.size() != mine.size())
            return std::string(bids ? "bid" : "ask") + " count recorder=" + std::to_string(arr.size()) +
                   " sink=" + std::to_string(mine.size());
        size_t i = 0;
        for (const auto &e : arr) {
            if (e.at(0).get<int64_t>() != mine[i].first || e.at(1).get<double>() != mine[i].second)
                return std::string(bids ? "bid" : "ask") + " level differs at units " + std::to_string(mine[i].first);
            ++i;
        }
        if (levels) *levels += arr.size();
    }
    const auto it = c.books.find(symbol);
    const bool valid = s.at("valid").get<bool>();
    if (it != c.books.end() && it->second.valid != valid) return "validity differs";
    const bool observing = valid && !s.at("bids").empty() && !s.at("asks").empty() &&
                           s.at("bids").back().at(0).get<int64_t>() < s.at("asks").front().at(0).get<int64_t>();
    if (it != c.books.end() && it->second.observing != observing) return "observation differs";
    if (it != c.books.end() && it->second.clock > s.at("clock").get<int64_t>()) return "sink clock ahead of recorder";
    return "";
}

// Normalised open-minute state against the recorder's own export at the same
// drained point: covered ms equals the symbol's `observed`, and per-level
// integrals summed into a layer's rows equal the rows accrued to the clock.
// Tolerance: 1e-9 relative, plus the recorder's running-row-sum residue. Its row
// size keeps cancellation error up to a few ulps of the largest size the row held
// (bounded here by peak x 2^-40, i.e. 4096 ulps) over at most 60 s.
struct OpenMinuteDiff {
    size_t rows = 0, residueRows = 0; // residueRows: within tolerance only through the residue term
    long double maxAbs = 0;
};
inline std::string compareOpenMinute(const nlohmann::json &state, const std::string &symbol,
                                     const ReferenceConsumer::Normalised &n, size_t layer, int64_t rowTickUnits,
                                     size_t *rowsCompared = nullptr, OpenMinuteDiff *diff = nullptr) {
    const auto &s = state.at("symbols").at(symbol);
    if (s.at("clock").get<int64_t>() != n.clock) return "normalised clock differs from the export";
    if (s.at("observed").get<uint64_t>() != n.coveredMs)
        return "covered " + std::to_string(n.coveredMs) + " != recorder observed " +
               std::to_string(s.at("observed").get<uint64_t>());
    const bool observing = s.at("valid").get<bool>() && !s.at("bids").empty() && !s.at("asks").empty() &&
                           s.at("bids").back().at(0).get<int64_t>() < s.at("asks").front().at(0).get<int64_t>();
    std::map<std::pair<int64_t, bool>, long double> mine, theirs, peaks;
    for (const auto *levels : {&n.live, &n.removed})
        for (const auto &l : *levels)
            if (l.integral > 0) mine[{floorDiv(l.units, rowTickUnits), !l.isBid}] += l.integral;
    for (const auto &row : s.at("layers").at(layer).at("rows")) {
        const long double size = std::stold(row.at(2).get<std::string>());
        long double integral = std::stold(row.at(3).get<std::string>());
        const int64_t last = row.at(5).get<int64_t>();
        if (observing && n.clock > last) integral += size * (n.clock - last);
        if (integral > 0) {
            theirs[{row.at(0).get<int64_t>(), row.at(1).get<bool>()}] = integral;
            peaks[{row.at(0).get<int64_t>(), row.at(1).get<bool>()}] =
                std::max(size, std::stold(row.at(4).get<std::string>()));
        }
    }
    for (const auto &[key, v] : theirs) {
        const auto it = mine.find(key);
        const long double other = it == mine.end() ? 0 : it->second;
        const long double delta = std::abs(v - other), relative = 1e-9L * std::max(v, other);
        const long double residue = std::ldexp(peaks[key], -40) * 60'000;
        if (delta > relative + residue)
            return "row " + std::to_string(key.first) + (key.second ? " ask" : " bid") + " recorder=" +
                   std::to_string(double(v)) + " normalised=" + std::to_string(double(other)) +
                   " delta=" + std::to_string(double(delta));
        if (rowsCompared) ++*rowsCompared;
        if (diff) {
            ++diff->rows;
            if (delta > relative) ++diff->residueRows;
            diff->maxAbs = std::max(diff->maxAbs, delta);
        }
    }
    for (const auto &[key, v] : mine)
        if (!theirs.contains(key)) return "row " + std::to_string(key.first) + " only in the normalised state";
    return "";
}

} // namespace recording::sinkref
