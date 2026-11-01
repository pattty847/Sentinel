// Architecture C (libs/core/levelstore): exact decimal identity, the interim
// unsupported-price policy, message-level states, lifecycle, and open-state
// export/import through bytes. Every scenario drives a real BookRecorder.
#include "LevelTestSupport.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "Crc32.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <random>

using namespace recording;
using namespace levelstore_test;

namespace {
constexpr int64_t kEpoch = kHmc2MinMs;
constexpr int64_t kMin = 60'000;
constexpr int64_t kFloor = kEpoch + 5 * kMin, kCeiling = kFloor + 15 * kMin;
const std::string kSym = "BTC-USD";

struct Step {
    enum Kind { Snap, Upd, Inv, Tick } kind;
    int64_t t; // relative to kFloor
    std::vector<Level> levels = {};
    int64_t local = INT64_MIN; // receive time relative to kFloor; defaults to t
    uint64_t sequence = 0;
};
Step snap(int64_t t, std::vector<Level> l) { return {Step::Snap, t, std::move(l)}; }
Step upd(int64_t t, std::vector<Level> l, int64_t local = INT64_MIN) { return {Step::Upd, t, std::move(l), local}; }
Step inv(int64_t t) { return {Step::Inv, t}; }
Step tick(int64_t t) { return {Step::Tick, t}; }
std::vector<Level> book4() { return {L(true, "99.00", 2), L(true, "98", 3), L(false, "101.0", 4), L(false, "102", 1)}; }

RecorderConfig config(const fs::path &root) {
    RecorderConfig c;
    c.root = root;
    c.priceScale = 100;
    c.layers = {{"near", 100, 0.5, 2, false}, {"deep", 500, 0.25, 4, true}};
    c.latenessMs = 2000;
    c.deterministicResume = true; // as roll() (Grid::config): a reopened store keeps its delta base
    return c;
}
DayBinding binding() { return {kSym, kFloor, kCeiling, 2}; }

std::map<std::string, std::string> hmc2Files(const fs::path &root) {
    std::map<std::string, std::string> out;
    for (const auto &e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".hmc2") {
            std::ifstream in(e.path(), std::ios::binary);
            out[fs::relative(e.path(), root).string()] = {std::istreambuf_iterator<char>(in), {}};
        }
    return out;
}

// One history recorder with C as its sink, and the in-process checkpoint.
struct Pipeline {
    RecorderConfig cfg;
    DayBinding bind;
    Capture &out;
    std::unique_ptr<LevelRecorder> c;
    std::unique_ptr<BookRecorder> r;
    Pipeline(const fs::path &root, Capture &o, DayBinding b = binding(), bool attach = true)
        : cfg(config(root)), bind(std::move(b)), out(o) {
        if (attach) c = std::make_unique<LevelRecorder>(bind, &out);
        cfg.sink = c.get();
        r = std::make_unique<BookRecorder>(cfg);
    }
    void step(const Step &s) {
        const int64_t local = kFloor + (s.local == INT64_MIN ? s.t : s.local);
        switch (s.kind) {
        case Step::Snap: r->onSnapshotAt(kSym, kFloor + s.t, local, s.levels, s.sequence); break;
        case Step::Upd: r->onUpdatesAt(kSym, kFloor + s.t, local, s.levels, s.sequence); break;
        case Step::Inv: r->onInvalid(kSym, local, "disconnect"); break;
        case Step::Tick: r->onTick(local); break;
        }
        r->drainForTest();
    }
    // Export both at this drained position, destroy both, import into fresh ones.
    LevelRecorder::ImportResult restore(const std::function<void(std::vector<uint8_t> &)> &corrupt = {}) {
        const auto bytes = r->exportState();
        const auto clock = r->checkpointClock(kSym);
        auto blob = c->exportState(clock, bytes);
        if (corrupt) corrupt(blob);
        r.reset();
        c.reset();
        c = std::make_unique<LevelRecorder>(bind, &out);
        cfg.sink = c.get();
        r = std::make_unique<BookRecorder>(cfg);
        r->importState(bytes);
        return c->importState(blob, r->checkpointClock(kSym), bytes);
    }
};

// Warmup, floor crossing, zero-duration and intra-message states, tiny and
// short-lived levels, a reset, an invalid period, a one-sided suspension, an
// unsupported-price episode with both refused recoveries, a gap, a backward
// envelope and events past the ceiling.
std::vector<Step> scenario() {
    std::vector<Step> s = {
        snap(-90'000, book4()),
        upd(-60'000, {L(true, "99", 2.5)}),
        upd(-1, {L(false, "101", 4.5)}),
        upd(0, {L(true, "98", 3.5)}),
        upd(2'000, {L(true, "99", 5)}),
        upd(2'000, {L(true, "99", 2)}), // same millisecond: a zero-duration state 5
        upd(3'000, {L(true, "99", 9), L(true, "99", 3)}), // one message: 9 never existed
        upd(4'000, {L(true, "98.5", 0.25)}),
        upd(4'000, {L(true, "98.5", 0)}), // a 0 ms level
        upd(5'000, {L(false, "101.5", 1e-12)}),
        upd(5'001, {L(false, "101.5", 0)}), // tiny, 1 ms
        tick(59'999),
        tick(60'000),
        snap(60'500, {L(true, "99", 3), L(true, "98", 7), L(false, "101", 4.5), L(false, "103", 2)}),
        inv(120'000),
        upd(125'000, {L(true, "99", 1)}), // dropped while invalid
        snap(130'000, book4()),
        upd(180'000, {L(false, "101", 0), L(false, "102", 0)}), // one-sided: unobserved, still valid
        upd(190'000, {L(false, "101.25", 6)}),
        upd(240'000, {L(true, "97.10000000000001", 1)}), // unsupported: episode
        upd(245'000, {L(true, "98", 8)}),
        snap(250'000, {L(true, "99", 2), L(true, "99.001", 1), L(false, "101", 4)}), // keeps the episode
        snap(255'000, {L(true, "99", 2)}), // one-sided: the recorder invalidates
        snap(300'000, book4()),             // valid and exact: observed again
        upd(310'000, {L(true, "99", 2.75)}, 310'000 + 10),
        upd(309'000, {L(true, "98", 1.5)}, 311'000), // backward envelope: clamped
        inv(360'000),
        snap(600'000, book4()), // minutes 6..9 skipped unobserved
        upd(601'000, {L(false, "102", 1.75)}),
        tick(14 * kMin + 59'000),
        upd(15 * kMin + 1'000, {L(true, "99", 4)}), // past the ceiling
        tick(17 * kMin),
    };
    return s;
}

} // namespace

// ---------------------------------------------------------------- decimal identity

TEST(PriceDecimal, ParsesPlainDecimalsExactly) {
    const auto p = [](const char *t) { return parsePriceDecimal(t); };
    auto d = p("97.1");
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 971u);
    EXPECT_EQ(d.fraction, 1u);
    d = p("97.10000000000001");
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 9710000000000001u);
    EXPECT_EQ(d.fraction, 14u);
    d = p("0.00000428");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(428u, 8u, true));
    d = p("100.500");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction)), std::tuple(1005u, 1u));
    d = p("1.000000000000000000000000000");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(1u, 0u, true));
    d = p("12345678901234567"); // 17 significant digits
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 12345678901234567u);
    d = p("0.000000000000000000001"); // one significant digit, 21 places
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(1u, 21u, true));
    d = p("000123.4500");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction)), std::tuple(12345u, 2u));
}

TEST(PriceDecimal, MalformedLongAndExponentTextIsNeverExact) {
    // Coinbase sends plain decimals only; anything else has no guessed identity.
    for (const char *t : {"", ".5", "5.", "1e5", "1E-7", "1.5e2", "-1", "+1", " 1", "1 ", "1.2.3", "1,5", "0x1p3",
                          "inf", "nan", "1_000", "\xD9\xA1"})
        EXPECT_FALSE(parsePriceDecimal(t).exact) << t;
    EXPECT_FALSE(parsePriceDecimal("123456789012345678").exact);    // 18 significant digits
    EXPECT_FALSE(parsePriceDecimal("1.00000000000000001").exact);   // 18 significant digits
    EXPECT_FALSE(parsePriceDecimal(("0." + std::string(63, '0') + "1").c_str()).exact); // 64 places
    EXPECT_TRUE(parsePriceDecimal(("0." + std::string(62, '0') + "1").c_str()).exact);  // 63 places
}

TEST(PriceDecimal, IdentityAgainstTheRecorderKeyAndCheckedOverflow) {
    EXPECT_EQ(priceScaleDigits(100), 2);
    EXPECT_EQ(priceScaleDigits(1e8), 8);
    EXPECT_EQ(priceScaleDigits(1), 0);
    EXPECT_EQ(priceScaleDigits(1e18), 18);
    EXPECT_EQ(priceScaleDigits(1e19), -1);
    EXPECT_EQ(priceScaleDigits(250), -1);
    // The double hides it, the text does not: 97.10000000000001 passes the
    // binary64 OnGrid test at scale 100 and is still finer than the tick.
    const double hidden = std::strtod("97.10000000000001", nullptr);
    EXPECT_EQ(pricePrecision(hidden, 9710, 100), RecorderSink::Precision::OnGrid);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.10000000000001"), 2, 9710), DecimalIdentity::Finer);
    EXPECT_EQ(std::strtod("99.000000000000001", nullptr), 99.0); // identical double, 17 digits
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("99.000000000000001"), 2, 9900), DecimalIdentity::Finer);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("99.0000000000000001"), 2, 9900), DecimalIdentity::NoText); // 18 digits
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), 2, 9710), DecimalIdentity::Exact);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), 2, 9711), DecimalIdentity::Mismatch);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("0.00000428"), 8, 428), DecimalIdentity::Exact);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("0.000004285"), 8, 428), DecimalIdentity::Finer);
    EXPECT_EQ(decimalIdentity(PriceDecimal{}, 2, 9710), DecimalIdentity::NoText);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), -1, 9710), DecimalIdentity::NoText);
    // Checked arithmetic: 9.2 at 10^18 fits int64; 9.3 overflows and never wraps.
    EXPECT_EQ(decimalUnits(parsePriceDecimal("9.2"), 18), std::optional<int64_t>(9'200'000'000'000'000'000));
    EXPECT_EQ(decimalUnits(parsePriceDecimal("9.3"), 18), std::nullopt);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("9.3"), 18, 0), DecimalIdentity::Mismatch);
    EXPECT_EQ(decimalUnits(parsePriceDecimal("92233720368547758"), 2), std::optional<int64_t>(9223372036854775800));
    EXPECT_EQ(decimalUnits(parsePriceDecimal("92233720368547759"), 2), std::nullopt); // 2^63 - 1 + 93
    EXPECT_EQ(decimalUnits(parsePriceDecimal("97.1"), 19), std::nullopt);
    // Random on-grid decimals at both production scales are always Exact against
    // the recorder's own key; one more digit is always Finer.
    std::mt19937_64 rng(11);
    for (const int k : {2, 8})
        for (int i = 0; i < 100'000; ++i) {
            const int64_t units = int64_t(rng() % (int64_t{1} << 40)) + 1;
            std::string text = std::to_string(units);
            if (text.size() <= size_t(k)) text.insert(0, size_t(k) + 1 - text.size(), '0');
            text.insert(text.size() - size_t(k), 1, '.');
            const auto key = *priceUnits(std::strtod(text.c_str(), nullptr), double(kPow10[size_t(k)]));
            ASSERT_EQ(decimalIdentity(parsePriceDecimal(text), k, key), DecimalIdentity::Exact) << text;
            ASSERT_EQ(decimalIdentity(parsePriceDecimal(text + "1"), k, key), DecimalIdentity::Finer) << text;
        }
}

// The recorder reports the identity, envelope and sequence with each level and
// fires onImprecise for a price the binary64 test misses.
TEST(RecorderIdentity, ReportedWithEveryLevel) {
    struct Sink final : RecorderSink {
        std::vector<std::pair<std::string, LevelChange>> events;
        void onReset(const std::string &, int64_t) noexcept override {}
        void onImprecise(const std::string &, int64_t, const LevelChange &l) noexcept override { events.push_back({"imprecise", l}); }
        void onLevel(const std::string &, int64_t, const LevelChange &l) noexcept override { events.push_back({"level", l}); }
        void onApplied(const std::string &, int64_t, bool, bool) noexcept override {}
        void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
        void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override {}
    } sink;
    TempDir dir;
    auto cfg = config(dir.path);
    cfg.sink = &sink;
    BookRecorder r(cfg);
    r.onSnapshotAt(kSym, kFloor, kFloor + 5, book4(), 41);
    r.onUpdatesAt(kSym, kFloor + 1000, kFloor + 1003,
                  {L(true, "97.10000000000001", 1), L(true, "97.1", 2), Level{true, 96, 1}}, 42);
    r.drainForTest();
    ASSERT_EQ(sink.events.size(), 4u + 5u);
    EXPECT_EQ(sink.events[0].second.identity, DecimalIdentity::Exact);
    EXPECT_EQ(sink.events[0].second.sequence, 41u);
    const auto &hidden = sink.events[4];
    EXPECT_EQ(hidden.first, "imprecise");
    EXPECT_EQ(hidden.second.precision, RecorderSink::Precision::OnGrid);
    EXPECT_EQ(hidden.second.identity, DecimalIdentity::Finer);
    EXPECT_EQ(hidden.second.envelopeMs, kFloor + 1000);
    EXPECT_EQ(hidden.second.sequence, 42u);
    EXPECT_EQ(sink.events[5].first, "level"); // HMC2 keeps applying it, rounded, as before
    EXPECT_EQ(sink.events[5].second.units, 9710);
    EXPECT_EQ(sink.events[6].first, "level");
    EXPECT_EQ(sink.events[6].second.identity, DecimalIdentity::Exact);
    EXPECT_EQ(sink.events[7].first, "imprecise"); // no text: never Exact
    EXPECT_EQ(sink.events[7].second.identity, DecimalIdentity::NoText);
}

// ---------------------------------------------------------------- unsupported prices

TEST(UnsupportedPrice, EpisodeKeepsEarlierDataAndRecoversOnlyAtAValidExactSnapshot) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    for (const auto &s : std::vector<Step>{
             snap(1'000, book4()),
             upd(2'000, {L(true, "99", 5)}),
             upd(3'000, {L(true, "97.10000000000001", 1)}), // episode starts
             upd(4'000, {L(true, "98", 7)}),                // ignored: book unknown
             upd(5'000, {L(false, "101.005", 2)}),          // counted, same episode
         })
        p.step(s);
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::Unsupported);
    EXPECT_FALSE(p.c->observed());
    EXPECT_EQ(p.c->liveLevels(), 0u);
    // (b) a snapshot with an unrepresentable price does not recover.
    p.step(snap(10'000, {L(true, "99", 2), L(true, "99.001", 1), L(false, "101", 4)}));
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::Unsupported);
    // (c) a snapshot the recorder does not accept as a valid book does not recover.
    p.step(snap(20'000, {L(true, "99", 2)}));
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::Unsupported);
    EXPECT_FALSE(p.c->recorderValid());
    // (a) a clean snapshot the recorder applies as a valid book recovers.
    p.step(snap(30'000, book4()));
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::None);
    EXPECT_TRUE(p.c->observed());
    p.step(tick(2 * kMin));
    const auto stats = p.c->stats();
    EXPECT_EQ(stats.unsupportedPrices, 3u);
    EXPECT_EQ(stats.unsupportedEpisodes, 1u);
    ASSERT_GE(out.blocks.size(), 1u);
    const auto &b = out.blocks[0];
    EXPECT_EQ(b.bucketStartMs, kFloor);
    // Messages: the reset at 1 s, 99 -> 5 at 2 s (earlier data intact), the clear at
    // 3 s, then nothing until the recovering reset at 30 s.
    ASSERT_EQ(b.messages.size(), 4u);
    EXPECT_EQ(b.messages[0], (TimelineMessage{kFloor + 1'000, true, true, ClearCause::None}));
    EXPECT_EQ(b.messages[1], (TimelineMessage{kFloor + 2'000, true, false, ClearCause::None}));
    EXPECT_EQ(b.messages[2], (TimelineMessage{kFloor + 3'000, false, false, ClearCause::Unsupported}));
    EXPECT_EQ(b.messages[3], (TimelineMessage{kFloor + 30'000, true, true, ClearCause::None}));
    EXPECT_EQ(b.changes.size(), 4u + 1u + 4u);
    // C observed 2 s + 30 s; the recorder 19 s + 30 s. HMC2 keeps its own view.
    const auto swept = sweep(out);
    EXPECT_EQ(swept[0].covered, 2'000 + 30'000);
    EXPECT_EQ(b.recorderObservedMs, 19'000u + 30'000u);
}

TEST(UnsupportedPrice, RoundedCollisionWithoutABookChangeStillStartsTheEpisode) {
    // 99.004 is off-grid as a double; 99.000000000000001 parses to exactly 99.0, so
    // only its decimal identity shows it. Both round onto 99.00 at the same size:
    // the book does not change and no onLevel is sent.
    for (const char *text : {"99.004", "99.000000000000001"}) {
        TempDir dir;
        Capture out;
        Pipeline p(dir.path, out);
        p.step(snap(1'000, book4()));
        p.step(upd(2'000, {L(true, text, 2)}));
        EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::Unsupported) << text;
        EXPECT_EQ(p.c->stats().unsupportedPrices, 1u) << text;
    }
}

TEST(UnsupportedPrice, AScaleThatIsNotDecimalSupportsNothing) {
    TempDir dir;
    Capture out;
    auto cfg = config(dir.path);
    cfg.priceScale = 250; // not 10^k: no exact decimal grid, every identity is NoText
    auto b = binding();
    b.priceDigits = -1;
    LevelRecorder c(b, &out);
    cfg.sink = &c;
    BookRecorder r(cfg);
    r.onSnapshotAt(kSym, kFloor, kFloor, {L(true, "99", 2), L(false, "101", 1)});
    r.drainForTest();
    EXPECT_EQ(c.unobserved(), LevelRecorder::Unobserved::Unsupported);
    EXPECT_EQ(c.stats().unsupportedPrices, 2u);
}

// ---------------------------------------------------------------- message-level states

TEST(MessageStates, ZeroDurationTinyAndShortLivedLevelsAndMessageAtomicPeaks) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    for (const auto &s : std::vector<Step>{
             snap(1'000, {L(true, "99", 1), L(false, "101", 1)}),
             upd(2'000, {L(true, "99", 5)}),
             upd(2'000, {L(true, "99", 2)}),                    // 5 lasted 0 ms
             upd(3'000, {L(true, "99", 9), L(true, "99", 3)}),  // 9 is inside one message
             upd(4'000, {L(true, "98.5", 0.25)}),
             upd(4'000, {L(true, "98.5", 0)}),                  // a 0 ms level
             upd(5'000, {L(false, "101.5", 1e-12)}),
             upd(5'001, {L(false, "101.5", 0)}),                // a tiny 1 ms level
             tick(kMin + 30'000),
         })
        p.step(s);
    ASSERT_GE(out.blocks.size(), 1u);
    const auto &b = out.blocks[0];
    ASSERT_EQ(b.messages.size(), 8u);
    EXPECT_EQ(b.messages[1].timeMs, b.messages[2].timeMs); // two distinct states in one millisecond
    const std::vector<TimelineChange> bid99 = [&] {
        std::vector<TimelineChange> v;
        for (const auto &c : b.changes)
            if (c.isBid && c.units == 9900) v.push_back(c);
        return v;
    }();
    ASSERT_EQ(bid99.size(), 4u);
    EXPECT_EQ(bid99[1].size, 5);
    EXPECT_EQ(bid99[2].size, 2);
    EXPECT_EQ(bid99[3].size, 3); // only the message's final size
    const auto swept = sweep(out);
    const auto row = [&](bool isBid, int64_t units) {
        for (const auto &r : swept[0].rows)
            if (r.isBid == isBid && r.bin == units) return r;
        ADD_FAILURE() << "no row " << units;
        return BinRow{};
    };
    const auto r99 = row(true, 9900);
    EXPECT_EQ(r99.peak, 5);       // the 0 ms state counts
    EXPECT_EQ(r99.presenceMs, 59'000);
    EXPECT_EQ(r99.integral, 1.0L * 1'000 + 2.0L * 1'000 + 3.0L * 57'000);
    EXPECT_EQ(r99.last, 3);
    const auto r985 = row(true, 9850);
    EXPECT_EQ(r985.peak, 0.25L);
    EXPECT_EQ(r985.presenceMs, 0);
    EXPECT_EQ(r985.integral, 0);
    const auto r1015 = row(false, 10150);
    EXPECT_EQ(r1015.peak, static_cast<long double>(1e-12));
    EXPECT_EQ(r1015.presenceMs, 1);
    EXPECT_EQ(r1015.integral, static_cast<long double>(1e-12));
    EXPECT_EQ(swept[0].covered, int64_t(b.recorderObservedMs));
}

// Native sweep metrics against a brute-force reading of the same timeline
// (state after every message, each held until the next): random messages,
// several per millisecond, resets, invalid periods and suspensions.
TEST(MessageStates, SweepMatchesBruteForceOnRandomTimelines) {
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        TempDir dir;
        Capture out;
        Pipeline p(dir.path, out);
        std::mt19937_64 rng(seed);
        const char *prices[] = {"97", "98", "98.5", "99", "100.25", "101", "101.5", "102", "103"};
        p.step(snap(-2'000, book4()));
        int64_t t = 0;
        for (int i = 0; i < 1500; ++i) {
            t += int64_t(rng() % 4) * 37; // often the same millisecond
            const auto kind = rng() % 40;
            if (kind == 0) p.step(inv(t));
            else if (kind == 1) p.step(snap(t, book4()));
            else {
                std::vector<Level> l;
                for (int k = 0, n = int(rng() % 3) + 1; k < n; ++k) {
                    const int idx = int(rng() % 9);
                    l.push_back(L(idx < 4 || (idx == 4 && rng() % 2), prices[idx], double(rng() % 4) * 0.25));
                }
                p.step(upd(t, l));
            }
        }
        p.step(tick(t + 3 * kMin));
        ASSERT_TRUE(out.image);
        // Brute force: per minute and level, the size after every message.
        std::map<std::pair<bool, int64_t>, double> book;
        for (const auto &l : out.image->levels) book[{l.isBid, l.units}] = l.size;
        bool observed = out.image->observed;
        const auto swept = sweep(out);
        ASSERT_EQ(swept.size(), out.blocks.size());
        for (size_t m = 0; m < out.blocks.size(); ++m) {
            const auto &b = out.blocks[m];
            std::map<std::pair<bool, int64_t>, BinRow> expect;
            struct State { std::map<std::pair<bool, int64_t>, double> book; bool observed; int64_t t; };
            std::vector<State> states{{book, observed, b.bucketStartMs}};
            size_t c = 0;
            for (uint32_t i = 0; i < b.messages.size(); ++i) {
                if (b.messages[i].clear != ClearCause::None) book.clear();
                for (; c < b.changes.size() && b.changes[c].message == i; ++c) {
                    const auto &ch = b.changes[c];
                    if (ch.size > 0) book[{ch.isBid, ch.units}] = ch.size;
                    else book.erase({ch.isBid, ch.units});
                }
                observed = b.messages[i].observed;
                states.push_back({book, observed, b.messages[i].timeMs});
            }
            int64_t covered = 0;
            for (size_t k = 0; k < states.size(); ++k) {
                const int64_t until = k + 1 < states.size() ? states[k + 1].t : b.bucketStartMs + kMin;
                if (states[k].observed) covered += until - states[k].t;
                for (const auto &[key, size] : states[k].book) {
                    auto &r = expect[key];
                    r.isBid = key.first;
                    r.bin = key.second;
                    if (states[k].observed) {
                        r.peak = std::max(r.peak, static_cast<long double>(size));
                        r.integral += size * static_cast<long double>(until - states[k].t);
                        r.presenceMs += until - states[k].t;
                    }
                }
            }
            for (const auto &[key, size] : states.back().book) expect[key].last = size;
            std::vector<BinRow> rows;
            for (auto &[key, r] : expect)
                if (r.integral > 0 || r.presenceMs > 0 || r.peak > 0 || r.last > 0) rows.push_back(r);
            std::sort(rows.begin(), rows.end(), [](const BinRow &a, const BinRow &b) {
                return a.isBid != b.isBid ? a.isBid : a.bin < b.bin;
            });
            ASSERT_EQ(swept[m].rows, rows) << "seed " << seed << " minute " << m;
            ASSERT_EQ(swept[m].covered, covered);
            if (p.c->stats().unsupportedEpisodes == 0) ASSERT_EQ(covered, int64_t(b.recorderObservedMs));
        }
    }
}

// ---------------------------------------------------------------- lifecycle

TEST(Lifecycle, WarmupBuildsTheBookAndTheDayStartsAtTheFloor) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(-3 * kMin, book4()));
    p.step(upd(-2 * kMin, {L(true, "99", 6)}));
    p.step(upd(-1, {L(false, "102", 0)}));
    EXPECT_FALSE(out.image);
    p.step(upd(1'000, {L(true, "98", 1)}));
    ASSERT_TRUE(out.image);
    EXPECT_EQ(out.image->atMs, kFloor);
    EXPECT_TRUE(out.image->observed);
    EXPECT_EQ(out.image->levels, (std::vector<BookLevel>{{true, 9800, 3}, {true, 9900, 6}, {false, 10100, 4}}));
    p.step(tick(kMin + 5'000));
    ASSERT_EQ(out.blocks.size(), 1u);
    EXPECT_EQ(out.blocks[0].bucketStartMs, kFloor); // no warmup minute is output
    EXPECT_TRUE(out.blocks[0].observedAtStart);
}

TEST(Lifecycle, ASnapshotAfterTheFloorStartsAnEmptyUnobservedDay) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(90'000, book4()));
    ASSERT_TRUE(out.image);
    EXPECT_EQ(*out.image, (BookImage{kFloor, false, {}}));
    p.step(tick(3 * kMin));
    ASSERT_EQ(out.blocks.size(), 2u);
    EXPECT_EQ(out.blocks[0].bucketStartMs, kFloor + kMin);
    EXPECT_TRUE(out.blocks[0].messages.front().reset);
}

TEST(Lifecycle, ResetsInvalidPeriodsSuspensionsAndTheCeiling) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    for (const auto &s : scenario()) p.step(s);
    ASSERT_TRUE(out.image);
    for (const auto &b : out.blocks) {
        EXPECT_GE(b.bucketStartMs, kFloor);
        EXPECT_LT(b.bucketStartMs, kCeiling); // the next day's minutes are dropped
        EXPECT_FALSE(b.damaged);
    }
    const auto find = [&](int64_t at) -> std::pair<const MinuteBlock *, uint32_t> {
        for (const auto &b : out.blocks)
            for (uint32_t i = 0; i < b.messages.size(); ++i)
                if (b.messages[i].timeMs == kFloor + at) return {&b, i};
        return {nullptr, 0};
    };
    // The reset at 60.5 s: 98 3.5 -> 7; 99 (3) and 101 (4.5) unchanged, so their runs
    // continue; 102 ends; 103 starts.
    const auto [rb, ri] = find(60'500);
    ASSERT_TRUE(rb);
    EXPECT_TRUE(rb->messages[ri].reset);
    std::vector<TimelineChange> rc;
    for (const auto &c : rb->changes)
        if (c.message == ri) rc.push_back(c);
    EXPECT_EQ(rc, (std::vector<TimelineChange>{{ri, true, 9800, 7}, {ri, false, 10200, 0}, {ri, false, 10300, 2}}));
    // The invalidation clears; the update while invalid reaches nobody.
    const auto [ib, ii] = find(120'000);
    ASSERT_TRUE(ib);
    EXPECT_EQ(ib->messages[ii].clear, ClearCause::Invalid);
    EXPECT_FALSE(ib->messages[ii].observed);
    EXPECT_FALSE(find(125'000).first);
    // One-sided: still valid, unobserved, levels still tracked; refill observes again.
    const auto [sb, si] = find(180'000);
    ASSERT_TRUE(sb);
    EXPECT_FALSE(sb->messages[si].observed);
    EXPECT_EQ(sb->messages[si].clear, ClearCause::None);
    const auto [fb, fi] = find(190'000);
    ASSERT_TRUE(fb);
    EXPECT_TRUE(fb->messages[fi].observed);
    // The backward envelope is applied at the clamped recorder clock.
    EXPECT_TRUE(find(310'000).first);
    EXPECT_FALSE(find(309'000).first);
    // The skipped unobserved minutes 7..9 have no block; minute 10 starts unobserved.
    for (const auto &b : out.blocks) EXPECT_FALSE(b.bucketStartMs >= kFloor + 6 * kMin && b.bucketStartMs < kFloor + 10 * kMin);
    const auto swept = sweep(out); // the timeline is self-consistent end to end
    EXPECT_EQ(swept.size(), out.blocks.size());
}

TEST(Lifecycle, ForkLeadNeverFeedsC) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    p.step(upd(2'000, {L(true, "99", 5)}));
    const auto before = p.c->exportState(p.r->checkpointClock(kSym), p.r->exportState());
    const auto stats = p.c->stats();
    auto lead = p.r->forkLead({}, 250);
    lead->onUpdatesAt(kSym, kFloor + 3'000, kFloor + 3'000, {L(true, "99", 8), L(true, "97.10000000000001", 1)});
    lead->onTick(kFloor + 3 * kMin);
    lead->drainForTest();
    EXPECT_EQ(p.c->stats(), stats);
    EXPECT_EQ(p.c->exportState(p.r->checkpointClock(kSym), p.r->exportState()), before);
    EXPECT_TRUE(out.blocks.empty());
}

// ---------------------------------------------------------------- checkpoint export/import

// The core equivalence: export -> bytes -> import into fresh instances after every
// message (warmup, floor, inside an invalid period, right after resets, inside an
// unsupported episode, at minute ends, past the ceiling) gives exactly the output
// and state of an uninterrupted run, and leaves HMC2 bytes unchanged.
TEST(Checkpoint, RestoreAfterEveryMessageIsExactlyTheUninterruptedRun) {
    TempDir a, b;
    Capture outA, outB;
    Pipeline pa(a.path, outA), pb(b.path, outB);
    size_t restores = 0;
    for (const auto &s : scenario()) {
        pa.step(s);
        pb.step(s);
        const auto imported = pb.restore();
        ASSERT_TRUE(imported.ok) << imported.reason;
        ++restores;
        ASSERT_EQ(outA.bytes(), outB.bytes()) << "after step at " << s.t;
    }
    EXPECT_EQ(restores, scenario().size());
    EXPECT_EQ(pa.c->stats(), pb.c->stats());
    const auto ra = pa.r->exportState(), rb = pb.r->exportState();
    EXPECT_EQ(ra, rb);
    EXPECT_EQ(pa.c->exportState(pa.r->checkpointClock(kSym), ra), pb.c->exportState(pb.r->checkpointClock(kSym), rb));
    pa.r.reset();
    pb.r.reset();
    EXPECT_EQ(hmc2Files(a.path), hmc2Files(b.path));
    EXPECT_GT(outA.blocks.size(), 8u);
}

// A tick moves the recorder clock without any sink event: the checkpoint binds C
// to the recorder's exported clock (INV-192), not to C's last event.
TEST(Checkpoint, BindsToTheRecorderClockAndStateNotToCsLastEvent) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    p.step(tick(20'000)); // silent clock movement
    const auto bytes = p.r->exportState();
    const auto clock = p.r->checkpointClock(kSym);
    ASSERT_EQ(clock, std::optional<int64_t>(kFloor + 20'000));
    EXPECT_THROW((void)p.c->exportState(kFloor + 500, bytes), std::logic_error); // behind C
    const auto blob = p.c->exportState(clock, bytes);
    LevelRecorder fresh1(binding(), &out);
    EXPECT_FALSE(fresh1.importState(blob, kFloor + 1'000, bytes).ok); // C's last event clock
    LevelRecorder fresh2(binding(), &out);
    auto other = bytes;
    other[other.size() / 2] ^= 1;
    EXPECT_FALSE(fresh2.importState(blob, clock, other).ok); // not this recorder state
    LevelRecorder fresh3(binding(), &out);
    EXPECT_TRUE(fresh3.importState(blob, clock, bytes).ok);
}

TEST(Checkpoint, CorruptVersionedForeignAndUnpairedBlobsAreRefusedAndFailClosed) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    for (const auto &s : std::vector<Step>{snap(-10'000, book4()), upd(1'000, {L(true, "99", 5)}),
                                           upd(2'000, {L(false, "101", 3)})})
        p.step(s);
    const auto bytes = p.r->exportState();
    const auto clock = p.r->checkpointClock(kSym);
    const auto blob = p.c->exportState(clock, bytes);
    const auto refused = [&](const std::vector<uint8_t> &b, const std::string &why, DayBinding bind = binding(),
                             std::optional<int64_t> at = std::nullopt, const std::vector<uint8_t> *state = nullptr) {
        Capture o;
        LevelRecorder c(bind, &o);
        const auto r = c.importState(b, at ? at : clock, state ? *state : bytes);
        EXPECT_FALSE(r.ok) << why;
        if (!why.empty()) EXPECT_NE(r.reason.find(why), std::string::npos) << r.reason;
        EXPECT_EQ(c.unobserved(), LevelRecorder::Unobserved::RestoreRefused);
        EXPECT_EQ(c.liveLevels(), 0u);
        EXPECT_EQ(c.stats().restoreRefusals, 1u);
        // Once its day has started, the reader is told to drop its book at the checkpoint.
        const int64_t checkpoint = at ? *at : *clock;
        if (checkpoint >= bind.floorMs)
            EXPECT_EQ(c.openBlock().messages,
                      (std::vector<TimelineMessage>{{checkpoint, false, false, ClearCause::RestoreRefused}}));
        else
            EXPECT_TRUE(c.openBlock().messages.empty());
    };
    for (size_t i = 0; i < blob.size(); ++i) {
        auto b = blob;
        b[i] ^= uint8_t(1u << (i % 8));
        refused(b, "CRC");
    }
    refused({}, "no state");
    refused(std::vector<uint8_t>(blob.begin(), blob.end() - 1), "");
    auto v = blob; // a future version with a valid CRC
    v[4] = uint8_t(LevelRecorder::kStateVersion + 1);
    const auto crc = sentinel::crc32(v.data(), v.size() - 4);
    for (int i = 0; i < 4; ++i) v[v.size() - 4 + size_t(i)] = uint8_t(crc >> (8 * i));
    refused(v, "version");
    auto other = binding();
    other.floorMs += kMin;
    refused(blob, "binding", other);
    other = binding();
    other.priceDigits = 8;
    refused(blob, "binding", other);
    refused(blob, "clock", binding(), *clock + 1);
    auto changed = bytes;
    changed[10] ^= 1;
    refused(blob, "paired", binding(), std::nullopt, &changed);
    // Importing into an instance that already processed events is refused too.
    EXPECT_FALSE(p.c->importState(blob, clock, bytes).ok);
}

TEST(Checkpoint, ARefusedRestoreStaysUnobservedUntilAValidExactSnapshot) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    p.step(upd(2'000, {L(true, "99", 5)}));
    const auto r = p.restore([](std::vector<uint8_t> &b) { b[7] ^= 0x40; });
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("CRC"), std::string::npos);
    p.step(upd(3'000, {L(true, "98", 9)})); // ignored: the book is unknown
    EXPECT_EQ(p.c->liveLevels(), 0u);
    p.step(snap(4'000, {L(true, "99", 1), L(true, "99.005", 1), L(false, "101", 1)})); // not exact
    EXPECT_FALSE(p.c->observed());
    p.step(snap(5'000, book4()));
    EXPECT_TRUE(p.c->observed());
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::None);
    p.step(tick(2 * kMin));
    ASSERT_EQ(out.blocks.size(), 2u);
    const auto &b = out.blocks[0];
    // The pre-checkpoint messages were in the lost open minute (refused state); the
    // block starts with the clear at the checkpoint clock, then the recovering reset.
    ASSERT_EQ(b.messages.size(), 2u);
    EXPECT_EQ(b.messages[0], (TimelineMessage{kFloor + 2'000, false, false, ClearCause::RestoreRefused}));
    EXPECT_EQ(b.messages[1], (TimelineMessage{kFloor + 5'000, true, true, ClearCause::None}));
    // The minute's part before the checkpoint is lost: the block says so and is
    // read as unobserved; the next minute continues normally.
    EXPECT_TRUE(b.damaged);
    EXPECT_FALSE(b.observedAtStart);
    EXPECT_FALSE(out.blocks[1].damaged);
    const auto swept = sweep(out);
    ASSERT_EQ(swept.size(), 2u);
    EXPECT_EQ(swept[0].covered, 0);
    for (const auto &r : swept[0].rows) EXPECT_EQ(std::tuple(r.integral, r.presenceMs, r.peak), std::tuple(0.0L, int64_t{0}, 0.0L));
    EXPECT_EQ(swept[1].covered, kMin);
}

TEST(Failure, OutputFailureLosesTheMinuteClearsAndRecoversAtAValidExactSnapshot) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    out.failNextBlock = true;
    p.step(tick(kMin + 1'000)); // minute 0's output fails
    EXPECT_EQ(p.c->unobserved(), LevelRecorder::Unobserved::Failed);
    EXPECT_EQ(p.c->stats().failures, 1u);
    p.step(upd(kMin + 2'000, {L(true, "99", 5)}));
    p.step(snap(kMin + 3'000, book4()));
    EXPECT_TRUE(p.c->observed());
    p.step(tick(3 * kMin));
    ASSERT_EQ(out.blocks.size(), 2u);
    EXPECT_EQ(out.blocks[0].bucketStartMs, kFloor + kMin); // minute 0 is missing, never fabricated
    EXPECT_EQ(out.blocks[0].messages[0], (TimelineMessage{kFloor + kMin, false, false, ClearCause::Failed}));
    EXPECT_TRUE(out.blocks[0].messages[1].reset);
    // The reader's book is stale after the lost minute: the next one starts unobserved.
    EXPECT_FALSE(out.blocks[0].observedAtStart);
    EXPECT_FALSE(out.blocks[0].damaged);
    const auto swept = sweep(out);
    ASSERT_EQ(swept.size(), 2u);
    EXPECT_EQ(swept[0].covered, kMin - 3'000); // observed from the recovering snapshot
}

// A reader enforces observation continuity between consecutive whole minutes; it
// accepts a block's own start state only after a damaged block or a missing minute.
TEST(MessageStates, SweepContinuityDamagedBlocksAndGaps) {
    BookImage image{kFloor, true, {{true, 9900, 2}}};
    MinuteBlock a{kFloor, 60'000, 0, true, false, {}, {}};
    MinuteBlock b = a;
    b.bucketStartMs = kFloor + kMin;
    b.observedAtStart = false; // does not continue a
    std::vector<BinRow> rows;
    int64_t covered = 0;
    MinuteSweep s;
    s.start(image);
    s.apply(a, rows, covered);
    EXPECT_EQ(covered, kMin);
    EXPECT_THROW(s.apply(b, rows, covered), std::runtime_error);
    MinuteSweep gap; // a missing minute in between: b's start state is taken as given
    gap.start(image);
    gap.apply(a, rows, covered);
    b.bucketStartMs = kFloor + 2 * kMin;
    gap.apply(b, rows, covered);
    EXPECT_EQ(covered, 0);
    MinuteSweep damaged; // a damaged block is unobserved and frees the next start state
    damaged.start(image);
    a.damaged = true;
    damaged.apply(a, rows, covered);
    EXPECT_EQ(covered, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(std::tuple(rows[0].integral, rows[0].presenceMs, rows[0].peak, rows[0].last),
              std::tuple(0.0L, int64_t{0}, 0.0L, 2.0L));
    b.bucketStartMs = kFloor + kMin;
    damaged.apply(b, rows, covered);
    EXPECT_FALSE(damaged.observed());
}

TEST(Failure, AClockRegressionFailsClosedAndForeignSymbolsAreIgnored) {
    Capture out;
    LevelRecorder c(binding(), &out);
    RecorderSink::LevelChange l{true, 9900, 99, 1, RecorderSink::Precision::OnGrid, parsePriceDecimal("99"),
                                DecimalIdentity::Exact, 0, 0};
    c.onReset(kSym, kFloor + 1'000);
    c.onLevel(kSym, kFloor + 1'000, l);
    c.onApplied(kSym, kFloor + 1'000, true, true);
    EXPECT_TRUE(c.observed());
    c.onApplied("ETH-USD", kFloor + 2'000, false, false);
    EXPECT_EQ(c.stats().foreignEvents, 1u);
    EXPECT_TRUE(c.observed());
    c.onApplied(kSym, kFloor + 500, true, true); // behind
    EXPECT_EQ(c.unobserved(), LevelRecorder::Unobserved::Failed);
    EXPECT_EQ(c.openBlock().messages.back().clear, ClearCause::Failed);
}

// Round 1 (review B1): two genuinely valid recorder exports at the same clock with
// different books must not pair with each other's C state.
TEST(Checkpoint, TwoValidRecorderStatesAtTheSameClockAreNotInterchangeable) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    const auto bytesA = p.r->exportState();
    const auto clockA = p.r->checkpointClock(kSym);
    const auto blobA = p.c->exportState(clockA, bytesA);
    p.step(upd(1'000, {L(true, "99", 5)})); // same millisecond: the clock does not move
    const auto bytesB = p.r->exportState();
    const auto clockB = p.r->checkpointClock(kSym);
    const auto blobB = p.c->exportState(clockB, bytesB);
    ASSERT_EQ(clockA, clockB);
    ASSERT_NE(bytesA, bytesB);
    // Both are valid exports: their CRC over the whole blob is the same residue.
    EXPECT_EQ(sentinel::crc32(bytesA.data(), bytesA.size()), sentinel::crc32(bytesB.data(), bytesB.size()));
    const auto import = [&](const std::vector<uint8_t> &blob, const std::vector<uint8_t> &state) {
        Capture o;
        LevelRecorder c(binding(), &o);
        return c.importState(blob, clockA, state);
    };
    EXPECT_TRUE(import(blobA, bytesA).ok);
    EXPECT_TRUE(import(blobB, bytesB).ok);
    EXPECT_FALSE(import(blobA, bytesB).ok);
    EXPECT_FALSE(import(blobB, bytesA).ok);
    // An invalid recorder state can be neither exported against nor paired with.
    auto broken = bytesA;
    broken.back() ^= 1;
    EXPECT_THROW((void)p.c->exportState(clockA, broken), std::logic_error);
    const auto r = import(blobA, broken);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("not a valid export"), std::string::npos) << r.reason;
}

// Round 1 (review B2): the reader never sees a lost minute, which may have held the
// clear that emptied C's book. The first retained output must clear the reader's
// book whatever C's own book is, so levels a later snapshot omits never come back:
// after an invalidation or an unsupported price, after two lost minutes in a row,
// and across a checkpoint export/import.
TEST(Failure, ALostMinuteThatClearedTheBookNeverResurrectsItsLevels) {
    enum Variant { Invalidation, Unsupported, TwoLost, AcrossRestore };
    for (const Variant v : {Invalidation, Unsupported, TwoLost, AcrossRestore}) {
        TempDir dir;
        Capture out;
        Pipeline p(dir.path, out);
        p.step(snap(1'000, book4()));
        p.step(tick(kMin + 1'000)); // minute 0 emitted with all four levels
        if (v == Unsupported) p.step(upd(kMin + 5'000, {L(true, "97.10000000000001", 1)}));
        else p.step(inv(kMin + 5'000));
        EXPECT_EQ(p.c->liveLevels(), 0u);
        out.failBlocks = v == TwoLost ? 2 : 1;
        p.step(tick(2 * kMin + 1'000)); // minute 1 (the clear) is lost
        if (v == TwoLost) p.step(tick(3 * kMin + 1'000)); // and minute 2
        if (v == AcrossRestore) ASSERT_TRUE(p.restore().ok);
        const int64_t at = v == TwoLost ? 3 * kMin + 5'000 : 2 * kMin + 5'000;
        p.step(snap(at, {L(true, "99", 2), L(false, "101", 4)})); // 98 and 102 omitted
        EXPECT_TRUE(p.c->observed()) << v;
        p.step(tick(6 * kMin));
        ASSERT_FALSE(out.blocks.empty());
        const auto &first = out.blocks[1]; // the first retained block after the loss
        ASSERT_FALSE(first.messages.empty()) << v;
        EXPECT_EQ(first.messages.front().clear, ClearCause::Failed) << v;
        EXPECT_EQ(first.messages.front().timeMs, first.bucketStartMs) << v;
        const auto swept = sweep(out);
        ASSERT_EQ(swept.size(), out.blocks.size());
        for (size_t m = 1; m < swept.size(); ++m)
            for (const auto &r : swept[m].rows) {
                EXPECT_FALSE(r.bin == 9800 || r.bin == 10200)
                    << "variant " << v << " minute " << m << " resurrected " << r.bin << " presence " << r.presenceMs;
            }
        // The recovered minute observes exactly the snapshot's two levels.
        const auto &last = swept.back();
        EXPECT_EQ(last.covered, kMin);
        EXPECT_EQ(last.rows.size(), 2u) << v;
    }
}

namespace {
// A checkpoint blob in the v2 layout with a recomputed checksum (malformed payloads).
std::vector<uint8_t> forge(int64_t clock, const std::vector<uint8_t> &recorder,
                           const std::function<void(ByteWriter &)> &bookAndOpen) {
    ByteWriter w;
    for (char m : {'L', 'S', 'C', '1'}) w.u8(uint8_t(m));
    w.u8(uint8_t(LevelRecorder::kStateVersion));
    w.u8(uint8_t(LevelRecorder::kStateVersion >> 8));
    const auto b = binding();
    w.str(b.symbol);
    w.svarint(b.floorMs);
    w.svarint(b.ceilingMs);
    w.svarint(b.priceDigits);
    w.u8(1);
    w.svarint(clock);
    uint32_t trailer = 0;
    for (int i = 0; i < 4; ++i) trailer |= uint32_t(recorder[recorder.size() - 4 + size_t(i)]) << (8 * i);
    w.fixed64(recorder.size());
    w.fixed32(trailer);
    w.u8(1 | 2 | 4 | 8); // day started, valid, recorder observing, observed
    w.u8(0);             // episode
    w.u8(0);             // pending reader clear
    w.u8(1);
    w.svarint(clock);
    for (int i = 0; i < 10; ++i) w.varint(0);
    bookAndOpen(w);
    w.fixed32(sentinel::crc32(w.bytes.data(), w.bytes.size()));
    return std::move(w.bytes);
}
void emptyOpen(ByteWriter &w, int64_t clock) { encodeBlock(w, MinuteBlock{clock - clock % kMin, 0, 0, true, false, {}, {}}); }
} // namespace

// Round 1 (review M1): a payload with a valid checksum is still validated: every
// decoded delta is added with overflow checks and the results are range-checked.
TEST(Checkpoint, MalformedDeltasWithAValidChecksumAreRefused) {
    TempDir dir;
    Capture out;
    Pipeline p(dir.path, out);
    p.step(snap(1'000, book4()));
    const auto bytes = p.r->exportState();
    const int64_t clock = *p.r->checkpointClock(kSym);
    const auto import = [&](const std::vector<uint8_t> &blob) {
        Capture o;
        LevelRecorder c(binding(), &o);
        return c.importState(blob, clock, bytes);
    };
    // Control: the forger writes an acceptable blob.
    const auto good = import(forge(clock, bytes, [&](ByteWriter &w) {
        w.varint(2); w.svarint(9800); w.f64(3); w.svarint(100); w.f64(2);
        w.varint(1); w.svarint(10100); w.f64(4);
        emptyOpen(w, clock);
    }));
    ASSERT_TRUE(good.ok) << good.reason;
    // Book keys: INT64_MAX then +1 overflows.
    auto r = import(forge(clock, bytes, [&](ByteWriter &w) {
        w.varint(2); w.svarint(INT64_MAX); w.f64(3); w.svarint(1); w.f64(2);
        w.varint(0);
        emptyOpen(w, clock);
    }));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("overflow"), std::string::npos) << r.reason;
    // Open-minute message times: a delta that overflows.
    r = import(forge(clock, bytes, [&](ByteWriter &w) {
        w.varint(0); w.varint(0);
        w.svarint(kFloor); w.varint(0); w.varint(0); w.u8(1);
        w.varint(2);
        w.svarint(1'000); w.u8(1);
        w.svarint(INT64_MAX); w.u8(1);
        w.varint(0);
    }));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("overflow"), std::string::npos) << r.reason;
    // Open-minute change keys: INT64_MAX then +1.
    r = import(forge(clock, bytes, [&](ByteWriter &w) {
        w.varint(0); w.varint(0);
        w.svarint(kFloor); w.varint(0); w.varint(0); w.u8(1);
        w.varint(1);
        w.svarint(1'000); w.u8(1);
        w.varint(2);
        w.varint(0); w.u8(1); w.svarint(INT64_MAX); w.f64(1);
        w.varint(0); w.u8(1); w.svarint(1); w.f64(1);
    }));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("overflow"), std::string::npos) << r.reason;
    // A message time below any recorder time (a negative delta is refused before).
    r = import(forge(clock, bytes, [&](ByteWriter &w) {
        w.varint(0); w.varint(0);
        w.svarint(0); w.varint(0); w.varint(0); w.u8(1);
        w.varint(1);
        w.svarint(5); w.u8(1);
        w.varint(0);
    }));
    EXPECT_FALSE(r.ok);
    // The day image decoder checks its deltas too.
    ByteWriter img;
    img.svarint(kFloor); img.u8(1); img.varint(2);
    img.u8(1); img.svarint(INT64_MAX); img.f64(1);
    img.u8(1); img.svarint(1); img.f64(1);
    ByteReader reader(img.bytes);
    EXPECT_THROW(decodeImage(reader), std::runtime_error);
}

// Round 2 (review B2): the recorder can skip unobserved minutes without any sink
// callback, so a lost minute's clear cannot be timestamped in advance. It is put at
// the start of the next block actually output: output resumes, omitted levels stay
// absent and no failure repeats. Also with a checkpoint between the loss and the
// recovery, and with the first block after the gap lost as well.
TEST(Failure, ALostMinuteBeforeSkippedMinutesClearsTheNextOutputAndOutputResumes) {
    enum Variant { Plain, AcrossRestore, LoseAgain };
    for (const Variant v : {Plain, AcrossRestore, LoseAgain}) {
        TempDir dir;
        Capture out;
        Pipeline p(dir.path, out);
        p.step(snap(1'000, book4()));
        p.step(tick(kMin + 1'000)); // minute 0 emitted
        p.step(inv(65'000));
        out.failBlocks = 1;
        p.step(tick(5 * kMin + 1'000)); // minute 1 lost; minutes 2-4 skipped (unobserved)
        EXPECT_EQ(p.c->pendingClear(), ClearCause::Failed) << v;
        if (v == AcrossRestore) ASSERT_TRUE(p.restore().ok);
        p.step(snap(5 * kMin + 5'000, {L(true, "99", 2), L(false, "101", 4)})); // 98 and 102 omitted
        int64_t resumed = 5 * kMin;
        if (v == LoseAgain) {
            out.failBlocks = 1;
            p.step(tick(6 * kMin + 1'000)); // minute 5 lost too: C fails again, unobserved
            p.step(snap(6 * kMin + 5'000, {L(true, "99", 2), L(false, "101", 4)}));
            resumed = 6 * kMin;
        }
        p.step(tick(9 * kMin));
        ASSERT_GE(out.blocks.size(), 3u) << v;
        const auto &first = out.blocks[1];
        EXPECT_EQ(first.bucketStartMs, kFloor + resumed) << v;
        ASSERT_FALSE(first.messages.empty());
        EXPECT_EQ(first.messages.front(), (TimelineMessage{kFloor + resumed, false, false, ClearCause::Failed})) << v;
        EXPECT_EQ(out.blocks.back().bucketStartMs, kFloor + 8 * kMin) << v; // output resumed through minute 8
        EXPECT_EQ(p.c->stats().failures, v == LoseAgain ? 2u : 1u) << v;
        EXPECT_EQ(p.c->pendingClear(), ClearCause::None);
        const auto swept = sweep(out);
        for (size_t m = 1; m < swept.size(); ++m)
            for (const auto &r : swept[m].rows)
                EXPECT_FALSE(r.bin == 9800 || r.bin == 10200) << "variant " << v << " resurrected " << r.bin;
        EXPECT_EQ(swept.back().covered, kMin);
        EXPECT_EQ(swept.back().rows.size(), 2u);
    }
}

// Round 2: a minute that cannot be output for any reason is dropped whole, so none
// of its entries can reach a later minute; the next block starts with the clear.
TEST(Failure, AnInconsistentMinuteIsDroppedAndOutputResumes) {
    Capture out;
    LevelRecorder c(binding(), &out);
    const auto change = [](const char *price, double size) {
        RecorderSink::LevelChange l{true, 0, 0, size, RecorderSink::Precision::OnGrid, parsePriceDecimal(price),
                                    DecimalIdentity::Exact, 0, 0};
        l.units = *decimalUnits(l.decimal, 2);
        return l;
    };
    c.onReset(kSym, kFloor + 1'000);
    c.onLevel(kSym, kFloor + 1'000, change("99", 1));
    c.onApplied(kSym, kFloor + 1'000, true, true);
    c.onMinute(kSym, kFloor, 59'000, 0);
    c.onLevel(kSym, kFloor + 70'000, change("99", 2));
    c.onApplied(kSym, kFloor + 70'000, true, true);
    // A finish for a later minute while the open minute holds a message of minute 1.
    c.onMinute(kSym, kFloor + 3 * kMin, 60'000, 0);
    EXPECT_EQ(c.stats().failures, 1u);
    EXPECT_EQ(c.pendingClear(), ClearCause::Failed);
    EXPECT_TRUE(c.openBlock().messages.empty());
    c.onReset(kSym, kFloor + 3 * kMin + 5'000);
    c.onLevel(kSym, kFloor + 3 * kMin + 5'000, change("98", 1));
    c.onApplied(kSym, kFloor + 3 * kMin + 5'000, true, true);
    c.onMinute(kSym, kFloor + 3 * kMin, 55'000, 0);
    c.onMinute(kSym, kFloor + 4 * kMin, 60'000, 0);
    EXPECT_EQ(c.stats().failures, 1u);
    ASSERT_EQ(out.blocks.size(), 3u);
    EXPECT_EQ(out.blocks[1].bucketStartMs, kFloor + 3 * kMin);
    EXPECT_EQ(out.blocks[1].messages.front().clear, ClearCause::Failed);
    EXPECT_TRUE(out.blocks[1].messages[1].reset);
    const auto swept = sweep(out);
    ASSERT_EQ(swept.size(), 3u);
    EXPECT_EQ(swept[2].rows.size(), 1u); // only 98: 99 is gone
}

// Round 2 (review m2): the diagnostic text is whole up to 256 characters, else its
// head and tail with an explicit marker and the original length.
TEST(PriceDecimal, DiagnosticTextKeepsHeadTailAndLength) {
    EXPECT_EQ(diagnosticPriceText("99999.5abc"), "99999.5abc");
    const std::string longText = "99999." + std::string(600, '1') + "XYZ";
    const auto d = diagnosticPriceText(longText);
    EXPECT_LE(d.size(), kMaxPriceText + 48);
    EXPECT_EQ(d.substr(0, 6), "99999.");
    EXPECT_NE(d.find("[609 chars, middle omitted]"), std::string::npos) << d;
    EXPECT_EQ(d.substr(d.size() - 3), "XYZ");
}
