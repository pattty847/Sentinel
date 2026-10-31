// RecorderSink contract: event order, reconstruction from events only, and
// HMC2 byte identity with and without a sink.
#include "RecorderSinkReference.hpp"
#include "servermodel/BookRecorder.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cmath>
#include <random>
#include <algorithm>
#include <set>
#include <sstream>

using namespace recording;
using sinkref::ReferenceConsumer;
using sinkref::RowCheck;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kEpoch = kHmc2MinMs; // 2000-01-01T00:00:00Z
constexpr int64_t kMin = 60'000, kDay = 86'400'000;
const std::string kSym = "BTC-USD";

struct Step {
    enum Kind { Snap, Upd, Inv, Tick } kind;
    int64_t t;
    std::vector<Level> levels = {};
    int64_t local = -1; // receive time; defaults to t
};
Step snap(int64_t t, std::vector<Level> l) { return {Step::Snap, t, std::move(l)}; }
Step upd(int64_t t, std::vector<Level> l, int64_t local = -1) { return {Step::Upd, t, std::move(l), local}; }
Step inv(int64_t t) { return {Step::Inv, t}; }
Step tick(int64_t t) { return {Step::Tick, t}; }
const std::vector<Level> kBook = {{true, 99, 2}, {true, 98, 3}, {false, 101, 4}, {false, 102, 1}};

// Forwards to the reference consumer and logs every event (times relative to kEpoch).
struct Tee final : RecorderSink {
    ReferenceConsumer ref;
    std::vector<std::string> log;
    static std::string at(int64_t clock) { return " @" + std::to_string(clock - kEpoch); }
    void onReset(const std::string &s, int64_t clock) noexcept override {
        log.push_back("reset" + at(clock));
        ref.onReset(s, clock);
    }
    static const char *precision(Precision p) {
        return p == Precision::OnGrid ? "" : p == Precision::OffGrid ? " offgrid" : " outside";
    }
    void onImprecise(const std::string &s, int64_t clock, const LevelChange &c) noexcept override {
        std::ostringstream o;
        o << "imprecise " << (c.isBid ? "b " : "a ") << c.units << " " << c.size << precision(c.precision) << at(clock);
        log.push_back(o.str());
        ref.onImprecise(s, clock, c);
    }
    void onLevel(const std::string &s, int64_t clock, const LevelChange &c) noexcept override {
        std::ostringstream o;
        o << "level " << (c.isBid ? "b " : "a ") << c.units << " " << c.size << precision(c.precision) << at(clock);
        log.push_back(o.str());
        ref.onLevel(s, clock, c);
    }
    void onApplied(const std::string &s, int64_t clock, bool valid, bool observing) noexcept override {
        log.push_back("applied v" + std::to_string(valid) + " o" + std::to_string(observing) + at(clock));
        ref.onApplied(s, clock, valid, observing);
    }
    void onInvalid(const std::string &s, int64_t clock, const std::string &reason, bool upstream) noexcept override {
        log.push_back("invalid " + reason + " u" + std::to_string(upstream) + at(clock));
        ref.onInvalid(s, clock, reason, upstream);
    }
    void onMinute(const std::string &s, int64_t bucket, uint32_t observed, uint32_t flags) noexcept override {
        log.push_back("minute " + std::to_string(bucket - kEpoch) + " obs=" + std::to_string(observed) +
                      " flags=" + std::to_string(flags));
        ref.onMinute(s, bucket, observed, flags);
    }
};

struct Result {
    std::map<std::string, std::string> files; // relative path -> bytes, every .hmc2 file
    BookRecorder::Stats stats{};
    std::vector<std::shared_ptr<const Hmc2Record>> published;
    std::vector<std::string> bookErrors;
    size_t bookChecks = 0;
    BookRecorder::SnapshotWait finalWait;
};

class RecorderSinkTest : public testing::Test {
  protected:
    QTemporaryDir dirOff, dirOn;
    RowCheck lastRows;
    RecorderConfig config(const std::string &root) {
        RecorderConfig c;
        c.root = root;
        c.priceScale = 100;
        c.layers = {{"near", 100, 0.5, 2, false}, {"deep", 500, 0.25, 4, true}};
        c.latenessMs = 2000;
        return c;
    }
    static void feed(BookRecorder &r, const Step &s) {
        const int64_t local = kEpoch + (s.local < 0 ? s.t : s.local);
        switch (s.kind) {
        case Step::Snap: r.onSnapshotAt(kSym, kEpoch + s.t, local, s.levels); break;
        case Step::Upd: r.onUpdatesAt(kSym, kEpoch + s.t, local, s.levels); break;
        case Step::Inv: r.onInvalid(kSym, local, "disconnect"); break;
        case Step::Tick: r.onTick(local); break;
        }
    }
    Result run(const std::vector<Step> &steps, Tee *sink, std::vector<size_t> *logMarks = nullptr) {
        Result out;
        const std::string root = (sink ? dirOn : dirOff).path().toStdString();
        {
            auto c = config(root);
            c.sink = sink;
            c.publisher = [&out](std::shared_ptr<const Hmc2Record> r) { out.published.push_back(std::move(r)); };
            BookRecorder rec(c);
            auto all = steps;
            int64_t last = 0;
            for (const auto &s : steps) last = std::max(last, std::max(s.t, s.local));
            // Mid-minute, past lateness: every finished minute commits.
            all.push_back(tick((last + 3 * kMin) / kMin * kMin + 30'000));
            for (size_t i = 0; i < all.size(); ++i) {
                feed(rec, all[i]);
                rec.drainForTest();
                if (logMarks && sink) logMarks->push_back(sink->log.size());
                const auto state = rec.exportState(); // both runs: identical producer calls
                if (!sink) continue;
                ++out.bookChecks;
                if (const auto e = sinkref::compareBook(state, kSym, sink->ref); !e.empty())
                    out.bookErrors.push_back("step " + std::to_string(i) + ": " + e);
            }
            out.stats = rec.stats();
            out.finalWait = rec.snapshotWait(kSym);
        }
        for (const auto &e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || e.path().extension() != ".hmc2") continue;
            std::ifstream in(e.path(), std::ios::binary);
            out.files[fs::relative(e.path(), root).string()] =
                std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return out;
    }
    // Twin run (sink null vs reconstructing sink) with every acceptance check.
    Result verify(const std::vector<Step> &steps, Tee &tee, std::vector<size_t> *logMarks = nullptr) {
        const auto off = run(steps, nullptr);
        auto on = run(steps, &tee, logMarks);
        EXPECT_FALSE(off.files.empty());
        EXPECT_EQ(off.files, on.files) << "HMC2 bytes differ with a sink installed";
        EXPECT_EQ(off.stats.columnsWritten, on.stats.columnsWritten);
        EXPECT_EQ(off.stats.lateEvents, on.stats.lateEvents);
        EXPECT_EQ(off.stats.backwardSteps, on.stats.backwardSteps);
        EXPECT_EQ(off.stats.invalidations, on.stats.invalidations);
        EXPECT_EQ(off.stats.diskErrors, on.stats.diskErrors);
        EXPECT_EQ(off.published.size(), on.published.size());
        EXPECT_EQ(on.bookErrors, std::vector<std::string>{});
        EXPECT_EQ(tee.ref.errors, std::vector<std::string>{});
        RowCheck rc;
        std::set<int64_t> matched;
        for (const auto &r : on.published) {
            if (r->header.tfMs != kMin) continue;
            const auto it = tee.ref.minutes.find({kSym, r->bucketStartMs});
            if (it == tee.ref.minutes.end()) {
                ++rc.unmatchedRecords;
                continue;
            }
            rc.check(*r, it->second);
            matched.insert(r->bucketStartMs);
        }
        EXPECT_TRUE(rc.clean()) << rc.json().dump();
        lastRows = rc;
        EXPECT_GT(rc.rowsCompared, 0u);
        EXPECT_EQ(matched.size(), tee.ref.minutes.size()) << "a finished observed minute has no HMC2 record";
        return on;
    }
};

TEST_F(RecorderSinkTest, SnapshotUpdatesDeletesAndRuns) {
    Tee tee;
    tee.ref.keepRuns = true;
    verify({snap(0, kBook),
            upd(10'000, {{true, 99, 5}, {true, 98, 0}, {false, 103, 7}}), // multi-level record
            upd(30'000, {{false, 101, 0}}),
            upd(30'000, {{false, 101, 0}}), // removing an absent level is not a change
            upd(45'500, {{true, 97.5, 1.25}}),
            snap(70'000, {{true, 99, 1}, {false, 100, 2}}), // resnapshot while observing
            upd(130'000, {{true, 99, 3}}),
            tick(185'000)},
           tee);
    EXPECT_EQ(tee.ref.counts.resets, 2u);
    EXPECT_EQ(tee.ref.counts.levels, 4u + 3 + 1 + 1 + 2 + 1);
    EXPECT_EQ(tee.ref.counts.offGrid + tee.ref.counts.outsideDomain, 0u);
    // Bid 99: size 2 over [0,10s), size 5 over [10s,70s), reset at the snapshot.
    const auto run99 = [&](int64_t start) {
        return std::find_if(tee.ref.runs.begin(), tee.ref.runs.end(),
                            [&](const auto &r) { return r.isBid && r.units == 9900 && r.start == kEpoch + start; });
    };
    ASSERT_NE(run99(0), tee.ref.runs.end());
    EXPECT_EQ(run99(0)->end, kEpoch + 10'000);
    EXPECT_EQ(run99(0)->size, 2);
    ASSERT_NE(run99(10'000), tee.ref.runs.end());
    EXPECT_EQ(run99(10'000)->end, kEpoch + 70'000);
    EXPECT_EQ(run99(10'000)->size, 5);
}

TEST_F(RecorderSinkTest, MalformedBatchInvalidatesBeforeAnyLevel) {
    Tee tee;
    std::vector<size_t> marks;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto out = verify({snap(0, kBook),
                             upd(20'000, {{true, 99, 7}, {true, nan, 1}}),
                             upd(25'000, {{true, 99, 9}}), // dropped while invalid
                             snap(40'000, kBook),
                             upd(70'000, {{false, 101, 2}})},
                            tee, &marks);
    ASSERT_GE(marks.size(), 4u);
    EXPECT_EQ(std::vector<std::string>(tee.log.begin() + marks[0], tee.log.begin() + marks[1]),
              std::vector<std::string>{"invalid malformed L2 u0 @20000"});
    EXPECT_EQ(marks[1], marks[2]) << "an update while invalid emits nothing";
    EXPECT_EQ(out.stats.invalidations, 1u);
}

TEST_F(RecorderSinkTest, TwoSidednessLossCrossedAndResume) {
    Tee tee;
    verify({snap(0, kBook),
            upd(10'000, {{false, 101, 0}, {false, 102, 0}}),       // one-sided: suspended
            upd(20'000, {{true, 99, 4}}),                          // book still changes, unobserved
            upd(25'000, {{false, 101, 6}}),                        // resumed
            upd(50'000, {{true, 101.5, 1}}),                       // crossed
            upd(75'000, {{true, 101.5, 0}}),                       // resumed
            upd(90'000, {{true, 99, 0}, {true, 98, 0}}),           // bids empty
            upd(100'000, {{true, 98, 2}})},
           tee);
    EXPECT_EQ(tee.ref.counts.suspensions, 3u);
    EXPECT_EQ(tee.ref.counts.resumptions, 4u); // the first snapshot starts observation
    EXPECT_EQ(tee.ref.counts.invalid, 0u);
}

TEST_F(RecorderSinkTest, OneSidedSuspectWaitsWithoutBookEvents) {
    Tee tee;
    const auto out = verify({snap(0, kBook),
                             upd(5'000, {{false, 101, 0}, {false, 102, 0}}),
                             tick(40'000), // > oneSidedSuspectMs: precautionary snapshot wait
                             upd(50'000, {{true, 97, 1}})},
                            tee);
    EXPECT_EQ(out.finalWait.reason, "unobserved suspect");
    EXPECT_EQ(tee.ref.counts.invalid, 0u);
    EXPECT_EQ(tee.ref.counts.suspensions, 1u);
}

TEST_F(RecorderSinkTest, UpstreamInvalidationRejectedSnapshotsAndGap) {
    Tee tee;
    std::vector<size_t> marks;
    const auto out = verify({snap(0, kBook),
                             upd(10'000, {{true, 99, 6}}),
                             inv(30'000),
                             snap(40'000, {{true, 99, 1}, {true, 98, 1}}),        // missing asks
                             snap(45'000, {{true, 102, 1}, {false, 101, 1}}),     // crossed
                             snap(5 * kMin + 10'000, kBook),                       // after a gap
                             upd(5 * kMin + 20'000, {{false, 104, 3}})},
                            tee, &marks);
    ASSERT_GE(marks.size(), 5u);
    const auto slice = [&](size_t i) {
        return std::vector<std::string>(tee.log.begin() + marks[i - 1], tee.log.begin() + marks[i]);
    };
    EXPECT_EQ(slice(2), std::vector<std::string>{"invalid disconnect u1 @30000"});
    EXPECT_EQ(slice(3), (std::vector<std::string>{"reset @40000", "level b 9900 1 @40000", "level b 9800 1 @40000",
                                                  "invalid missing two-sided mid u0 @40000"}));
    EXPECT_EQ(slice(4), (std::vector<std::string>{"reset @45000", "level b 10200 1 @45000", "level a 10100 1 @45000",
                                                  "invalid unrepresentable or crossed mid u0 @45000"}));
    EXPECT_EQ(tee.ref.counts.upstreamInvalid, 1u);
    EXPECT_EQ(out.stats.invalidations, 3u);
    EXPECT_EQ(out.finalWait.reason, "");
}

TEST_F(RecorderSinkTest, BackwardAndLateEnvelopesUseTheClampedClock) {
    Tee tee;
    std::vector<size_t> marks;
    const auto out = verify({snap(0, kBook),
                             upd(50'000, {{true, 99, 4}}),
                             upd(48'000, {{true, 98, 5}}),                 // backward step
                             upd(125'000, {{false, 101, 3}}),
                             upd(110'000, {{false, 102, 2}}, 126'000),     // late: before closedThrough
                             tick(150'000)},
                            tee, &marks);
    ASSERT_GE(marks.size(), 5u);
    EXPECT_EQ(tee.log[marks[1]], "level b 9800 5 @50000");
    EXPECT_EQ(tee.log[marks[3]], "level a 10200 2 @125000");
    EXPECT_EQ(out.stats.backwardSteps, 2u);
    EXPECT_EQ(out.stats.lateEvents, 1u);
}

TEST_F(RecorderSinkTest, MinuteHourAndUtcDayBoundaries) {
    Tee tee;
    std::vector<size_t> marks;
    const int64_t d = kDay - 90'000; // 23:58:30
    verify({snap(d, kBook),
            upd(d + 30'000, {{true, 99, 3}}),
            upd(d + 90'000, {{false, 101, 5}}), // exactly midnight
            upd(d + 95'000, {{true, 98, 0}}),
            tick(d + 210'000)},
           tee, &marks);
    ASSERT_GE(marks.size(), 3u);
    // The minute ending at midnight is reported before the level at midnight.
    EXPECT_EQ(std::vector<std::string>(tee.log.begin() + marks[1], tee.log.begin() + marks[2]),
              (std::vector<std::string>{"minute " + std::to_string(kDay - kMin) + " obs=60000 flags=0",
                                        "level a 10100 5 @" + std::to_string(kDay),
                                        "applied v1 o1 @" + std::to_string(kDay)}));
}

TEST_F(RecorderSinkTest, FinerTickPriceIsDetectedAndHmc2Unchanged) {
    Tee tee;
    std::vector<size_t> marks;
    verify({snap(0, kBook), upd(10'000, {{true, 99.004, 1}, {true, 97.1, 2}})}, tee, &marks);
    EXPECT_EQ(tee.ref.counts.offGrid, 1u);
    EXPECT_EQ(tee.log[marks[0]], "imprecise b 9900 1 offgrid @10000"); // rounded onto 99.00
    EXPECT_EQ(tee.log[marks[0] + 1], "level b 9900 1 offgrid @10000");
    EXPECT_EQ(tee.log[marks[0] + 2], "level b 9710 2 @10000");
}

TEST_F(RecorderSinkTest, ForkLeadDoesNotCopyTheSink) {
    Tee tee;
    auto c = config(dirOn.path().toStdString());
    c.sink = &tee;
    BookRecorder rec(c);
    rec.onSnapshotAt(kSym, kEpoch, kEpoch, kBook);
    auto lead = rec.forkLead([](std::shared_ptr<const Hmc2Record>) {}, 500);
    const auto before = tee.log.size();
    lead->onUpdatesAt(kSym, kEpoch + 1'000, kEpoch + 1'000, {{true, 99, 8}});
    lead->onTick(kEpoch + 2 * kMin);
    lead->drainForTest();
    EXPECT_EQ(tee.log.size(), before);
    rec.onUpdatesAt(kSym, kEpoch + 1'000, kEpoch + 1'000, {{true, 99, 8}});
    rec.drainForTest();
    EXPECT_EQ(tee.log.size(), before + 2);
}
} // namespace

namespace {
TEST_F(RecorderSinkTest, RowSumResidueAtTheFloorIsClassified) {
    // Native levels in row 99 sum to exactly the floor (1e-6) after 1.0 leaves; the
    // recorder's running row size keeps 9.999999999e-7 and flags kUnderflow.
    Tee tee;
    verify({snap(0, {{true, 99, 1.0}, {true, 99.5, 1e-6}, {true, 98, 3}, {false, 101, 4}}),
            upd(10'000, {{true, 99, 0}}),
            tick(130'000)},
           tee);
    EXPECT_GE(lastRows.underflowTieRecords, 1u);
    EXPECT_EQ(lastRows.underflowMismatches, 0u);
}
} // namespace

namespace {
// An off-grid price whose rounded key holds the same size changes nothing in the
// book, so no onLevel: it must still be visible.
TEST_F(RecorderSinkTest, SameSizeCollisionIsStillReported) {
    Tee tee;
    std::vector<size_t> marks;
    verify({snap(0, kBook), upd(10'000, {{true, 99.004, 2}})}, tee, &marks);
    EXPECT_EQ(std::vector<std::string>(tee.log.begin() + marks[0], tee.log.begin() + marks[1]),
              (std::vector<std::string>{"imprecise b 9900 2 offgrid @10000", "applied v1 o1 @10000"}));
    EXPECT_EQ(tee.ref.counts.offGrid, 1u);
}

std::string decimal(int64_t units, int decimals) {
    std::string s = std::to_string(units);
    if (s.size() <= size_t(decimals)) s.insert(0, size_t(decimals) + 1 - s.size(), '0');
    s.insert(s.size() - size_t(decimals), ".");
    return s;
}
RecorderSink::Precision judge(const std::string &text, double scale, int64_t *unitsOut = nullptr) {
    const double price = std::strtod(text.c_str(), nullptr);
    const auto units = *priceUnits(price, scale);
    if (unitsOut) *unitsOut = units;
    return pricePrecision(price, units, scale);
}
TEST(PricePrecision, DecimalBoundaryLargeUnitsAndPepeScale) {
    using P = RecorderSink::Precision;
    EXPECT_EQ(judge("99.01", 100), P::OnGrid);
    EXPECT_EQ(judge("99.005", 100), P::OffGrid); // 9900.499999999998 rounds to 9900
    EXPECT_EQ(judge("99.0001", 100), P::OffGrid);
    EXPECT_EQ(judge("97.1", 100), P::OnGrid);    // product is 9710.000000000002
    int64_t units = 0;
    EXPECT_EQ(judge("10995116.27775", 1e5, &units), P::OnGrid);
    EXPECT_EQ(units, kPriceUnitsDomain - 1);
    EXPECT_EQ(judge("10995116.277741", 1e5), P::OffGrid); // 0.1 unit off, top of the domain
    EXPECT_EQ(judge("10995116.27776", 1e5), P::OutsideDomain);
    // Above the domain an on-grid decimal can round to the wrong key: unknown, never OnGrid.
    EXPECT_EQ(judge("4357.433857363632", 1e12), P::OutsideDomain);
    EXPECT_EQ(judge("0.00000428", 1e8), P::OnGrid);
    EXPECT_EQ(judge("0.000004285", 1e8), P::OffGrid);
}
// No false positive on any on-grid decimal, and every decimal at least 0.01 unit
// off the grid is flagged, across the domain at BTC and PEPE scales.
TEST(PricePrecision, RandomDecimalsInTheDomain) {
    std::mt19937_64 rng(11);
    for (const int decimals : {2, 8}) {
        const double scale = std::pow(10.0, decimals);
        for (int i = 0; i < 200'000; ++i) {
            const int64_t n = int64_t(rng() % uint64_t(kPriceUnitsDomain - 1)) + 1;
            ASSERT_EQ(judge(decimal(n, decimals), scale), RecorderSink::Precision::OnGrid) << decimal(n, decimals);
            const auto off = decimal(n, decimals) + std::to_string(1 + rng() % 98); // >= 0.01 unit off
            if (judge(off, scale) != RecorderSink::Precision::OffGrid) ADD_FAILURE() << off;
        }
    }
}

// A minute whose finalisation throws is reported once, after it succeeds; the
// failure itself is an onInvalid.
TEST_F(RecorderSinkTest, FailedMinuteFinalisationIsNotReported) {
    Tee tee;
    auto c = config(dirOn.path().toStdString());
    c.sink = &tee;
    int failures = 1;
    c.beforeMinuteFinalizeForTest = [&](int64_t bucket) {
        if (bucket == kEpoch && failures-- > 0) throw std::runtime_error("injected finalisation failure");
    };
    BookRecorder rec(c);
    for (const auto &s : std::vector<Step>{snap(0, kBook), upd(30'000, {{true, 99, 5}}), tick(65'000),
                                           snap(70'000, kBook), tick(200'000)}) {
        feed(rec, s);
        rec.drainForTest();
    }
    const auto count = [&](const std::string &prefix) {
        return std::count_if(tee.log.begin(), tee.log.end(), [&](const auto &l) { return l.rfind(prefix, 0) == 0; });
    };
    EXPECT_EQ(count("minute 0 "), 1) << "a failed minute must not be reported before it is finalised";
    EXPECT_EQ(count("invalid worker exception u0 @60000"), 1);
    EXPECT_EQ(count("minute 60000 "), 1);
    EXPECT_GT(rec.stats().diskErrors, 0u);
}

// The checker itself: an entry with a nonzero code where no native level has
// any integral is a failure, not an off-by-one tolerance.
TEST(RowCheckItself, ZeroIntegralWithNonzeroCodeFails) {
    Hmc2Record r;
    r.header.layer = "near";
    r.header.rowTickUnits = 100;
    r.observedMs = 60'000;
    r.bidRowLo = r.askRowLo = 0;
    r.bidRowHi = r.askRowHi = 1000;
    r.entries.push_back({99, false, 1, 1, 0});
    ReferenceConsumer::Minute m;
    m.coveredMs = m.eventObservedMs = 60'000;
    RowCheck rc;
    rc.check(r, m);
    EXPECT_EQ(rc.zeroNonzeroMismatches, 1u);
    EXPECT_FALSE(rc.clean());
}
} // namespace
