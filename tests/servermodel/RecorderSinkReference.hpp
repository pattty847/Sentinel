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
                 inexact = 0, runsClosed = 0, suspensions = 0, resumptions = 0, validations = 0,
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
    void onLevel(const std::string &symbol, int64_t clock, const LevelChange &c) noexcept override {
        auto &b = at(symbol, clock);
        ++counts.levels;
        if (!c.exact) ++counts.inexact;
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
// size x observed ms / observedMs equals the entry within code quantisation.
struct RowCheck {
    uint64_t records = 0, entries = 0, rowsCompared = 0, underflowRows = 0, codeMismatches = 0, missingEntries = 0,
             observedMismatches = 0, flagMismatches = 0, underflowMismatches = 0, underflowAtFloor = 0,
             unmatchedRecords = 0;
    double minFloorTieRatio = 0; // smallest native-sum twap / floor among floor-tie entries
    double maxRelError = 0;
    std::vector<std::string> first;
    void note(const std::string &what) {
        if (first.size() < 8) first.push_back(what);
    }
    bool clean() const {
        return !codeMismatches && !missingEntries && !observedMismatches && !flagMismatches &&
               !underflowMismatches && !unmatchedRecords;
    }
    nlohmann::json json() const {
        return {{"records", records},           {"entries", entries},
                {"rowsCompared", rowsCompared}, {"underflowRows", underflowRows}, {"maxRelError", maxRelError},
                {"codeMismatches", codeMismatches}, {"missingEntries", missingEntries},
                {"observedMismatches", observedMismatches}, {"flagMismatches", flagMismatches},
                {"underflowMismatches", underflowMismatches}, {"underflowAtFloor", underflowAtFloor},
                {"minFloorTieRatio", minFloorTieRatio}, {"unmatchedRecords", unmatchedRecords},
                {"first", first}};
    }
    // Minute records only. Returns false when the consumer has no such minute.
    bool check(const Hmc2Record &r, const ReferenceConsumer::Minute &m) {
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
        bool underflow = false, floorTie = false;
        const double floor = r.header.sizeScale.floor;
        for (const auto &e : r.entries) {
            ++entries;
            const auto it = rows.find({e.row, e.isAsk});
            const long double expected = it == rows.end() ? 0 : it->second / r.observedMs;
            if (expected > 0 && expected < r.header.sizeScale.floor) {
                underflow = true;
                ++underflowRows;
            }
            // A row whose native levels sum to exactly the floor (or a code-1 peak) can
            // carry a running-sum residue just below it in the recorder's row size.
            if ((expected >= floor && expected <= floor * (1 + 1e-9)) || (e.peakCode & kMaxCode) == 1) {
                floorTie = true;
                if (expected > 0)
                    minFloorTieRatio = minFloorTieRatio == 0 ? double(expected / floor)
                                                             : std::min(minFloorTieRatio, double(expected / floor));
            }
            const auto code = encodeSize(static_cast<double>(expected), r.header.sizeScale);
            if (std::abs(int(code) - int(e.twapCode)) > 1) {
                ++codeMismatches;
                note("twap " + at + " row=" + std::to_string(e.row) + " ask=" + std::to_string(e.isAsk) +
                     " code=" + std::to_string(e.twapCode) + " expected=" + std::to_string(code));
            }
            if (expected >= r.header.sizeScale.floor) // below the floor the code saturates at 1
                maxRelError = std::max(maxRelError,
                    static_cast<double>(std::abs(decodeSize(e.twapCode, r.header.sizeScale) - expected) / expected));
            ++rowsCompared;
        }
        for (const auto &[key, integral] : rows) {
            if (integral <= 0 || !inWindow(key.first, key.second)) continue;
            const std::pair<int64_t, bool> k{key.first, key.second};
            const auto pos = std::lower_bound(r.entries.begin(), r.entries.end(), k,
                [](const Hmc2Entry &a, const std::pair<int64_t, bool> &b) { return std::pair(a.row, a.isAsk) < b; });
            const bool found = pos != r.entries.end() && pos->row == k.first && pos->isAsk == k.second;
            if (!found) {
                ++missingEntries;
                note("missing " + at + " row=" + std::to_string(key.first));
            }
        }
        const bool hmc2Underflow = (r.flags & kUnderflow) != 0;
        if (hmc2Underflow && !underflow && floorTie) {
            ++underflowAtFloor;
        } else if (underflow != hmc2Underflow) {
            ++underflowMismatches;
            note("underflow " + at);
        }
        return true;
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

} // namespace recording::sinkref
