#include "roller/CandleRestore.hpp"
#include "roller/JournalFeed.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <tuple>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int64_t Epoch = 1'798'761'600'000; // 2027-01-01 UTC
constexpr int64_t Second = 1'000, Minute = 60'000, Hour = 3'600'000;
const std::vector<int64_t> Tfs{100, 1'000};
const std::map<int64_t, int> Caps{{100, 60'000}, {1'000, 6'000}};

json metadata() {
    return {{"product_metadata", {{"product_id", "BTC-USD"}, {"quote_increment", "0.01"},
                                  {"base_increment", "0.00000001"}}}};
}
std::string iso(int64_t rel) {
    const int64_t t = Epoch + rel;
    const time_t s = static_cast<time_t>(t / 1000);
    tm g{};
    gmtime_r(&s, &g);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900,
                  g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, int(t % 1000));
    return buf;
}
json tradeJson(int64_t exchangeRel, const std::string& id, double price) {
    return {{"product_id", "BTC-USD"}, {"trade_id", id}, {"time", iso(exchangeRel)},
            {"side", "BUY"}, {"price", std::to_string(price)}, {"size", "1"}};
}

// One capture journal; times are ms after Epoch on the receive clock.
struct Journal {
    capture::WriterConfig config;
    std::unique_ptr<capture::Writer> writer;
    int64_t steady = 0;
    uint64_t connection = 1;
    explicit Journal(const fs::path& root) {
        config.root = QString::fromStdString(root.string());
        config.fsyncBlocks = 0;
        writer = std::make_unique<capture::Writer>(config, metadata());
    }
    ~Journal() { if (writer) writer->close(); }
    void append(int64_t rel, capture::Kind kind, std::string payload) {
        // Steady time advances per record so clock regressions keep a valid file.
        steady += 100'000'000;
        writer->append({kind, {(Epoch + rel) * 1'000'000, steady}, connection, std::move(payload)});
    }
    void heartbeat(int64_t rel) { append(rel, capture::Kind::Frame, "{\"channel\":\"heartbeats\"}"); }
    void trades(int64_t rel, const std::vector<json>& list) {
        append(rel, capture::Kind::Frame,
               json{{"channel", "market_trades"},
                    {"events", json::array({{{"type", "update"}, {"trades", list}}})}}.dump());
    }
    // Received 150 ms after the exchange time unless stated.
    void trade(int64_t rel, const std::string& id, double price, int64_t exchangeRel = INT64_MIN) {
        trades(rel, {tradeJson(exchangeRel == INT64_MIN ? rel - 150 : exchangeRel, id, price)});
    }
    void resend(int64_t rel, const std::vector<json>& list) {
        append(rel, capture::Kind::TransportDown, "{}");
        ++connection;
        append(rel, capture::Kind::TransportUp, "{}");
        append(rel, capture::Kind::Frame,
               json{{"channel", "market_trades"},
                    {"events", json::array({{{"type", "snapshot"}, {"trades", list}}})}}.dump());
    }
    void newRun() {
        writer->close();
        writer = std::make_unique<capture::Writer>(config, metadata());
    }
};

int64_t receiveRel(const JournalRecord& r) { return r.record.time.systemNs / 1'000'000 - Epoch; }

// The last record received at or before rel.
JournalPos positionAt(const fs::path& root, int64_t rel) {
    JournalReader reader(root, "BTC-USD");
    JournalRecord r;
    std::optional<JournalPos> found;
    while (reader.next(r))
        if (receiveRel(r) <= rel) found = r.pos;
    EXPECT_TRUE(found.has_value());
    return found.value_or(JournalPos{});
}

// Uninterrupted path from the journal's first record: reader -> feed ->
// aggregator, one lagged watermark per journal second.
struct Reference {
    JournalReader reader;
    JournalFeed feed{"BTC-USD"};
    TimeframeAggregator aggregator;
    std::vector<TimeframeAggregator*> also;
    int64_t sent = INT64_MIN;
    explicit Reference(const fs::path& root, const std::vector<int64_t>& tfs = Tfs)
        : reader(root, "BTC-USD"), aggregator(tfs, Caps) {
        feed.onTrade = [this](const Trade& t) {
            aggregator.onTrade(t);
            for (auto* a : also) a->onTrade(t);
        };
    }
    bool step(JournalRecord& r) {
        if (!reader.next(r)) return false;
        feed.apply(r);
        const int64_t ms = r.record.time.systemNs / 1'000'000;
        const int64_t second = ms / 1000 - (ms % 1000 < 0);
        if (second > sent) {
            sent = second;
            aggregator.advance("BTC-USD", ms - 1'000);
            for (auto* a : also) a->advance("BTC-USD", ms - 1'000);
        }
        return true;
    }
    void until(const JournalPos& p) {
        JournalRecord r;
        while (step(r))
            if (r.pos == p) return;
        ADD_FAILURE() << "reference never reached the position";
    }
    void toEnd() {
        JournalRecord r;
        while (step(r)) {}
    }
};

bool sameBar(const OHLCVBar& a, const OHLCVBar& b) {
    return std::tie(a.timestamp_ms, a.open, a.high, a.low, a.close, a.volume, a.count, a.is_closed) ==
           std::tie(b.timestamp_ms, b.open, b.high, b.low, b.close, b.volume, b.count, b.is_closed);
}

void expectWindowEqual(const TimeframeAggregator& actual, const TimeframeAggregator& expected,
                       int64_t windowStartMs) {
    for (const int64_t tf : Tfs) {
        const auto keep = [&](std::vector<OHLCVBar> bars) {
            std::erase_if(bars, [&](const OHLCVBar& b) { return b.timestamp_ms < windowStartMs; });
            return bars;
        };
        const auto a = actual.exportSeries("BTC-USD", tf), e = expected.exportSeries("BTC-USD", tf);
        const auto ac = keep(a.closed), ec = keep(e.closed);
        ASSERT_FALSE(ec.empty()) << tf;
        ASSERT_EQ(ac.size(), ec.size()) << tf;
        for (size_t i = 0; i < ac.size(); ++i) EXPECT_TRUE(sameBar(ac[i], ec[i])) << tf << " bar " << i;
        EXPECT_TRUE(sameBar(a.active, e.active)) << tf;
    }
}

void expectRestoreEquals(const CandleRestore& restore, const TimeframeAggregator& expected) {
    TimeframeAggregator installed(Tfs, Caps);
    for (const auto& [tf, series] : restore.series)
        ASSERT_TRUE(installed.restoreSeries("BTC-USD", tf, series));
    expectWindowEqual(installed, expected, restore.windowStartMs);
}

CandleRestoreInput input(const fs::path& root, const JournalPos& through) {
    CandleRestoreInput in;
    in.root = root;
    in.product = "BTC-USD";
    in.through = through;
    in.timeframesMs = Tfs;
    in.historyBars = Caps;
    return in;
}

struct Temp {
    QTemporaryDir dir{QStringLiteral(ROLLER_TEST_ROOT "/restore-XXXXXX")};
    fs::path root() const { return dir.path().toStdString(); }
};

// Trades every `every` ms in [from, to) at a price that walks with the index.
int tradeRun(Journal& j, int64_t from, int64_t to, int64_t every, int id, double base = 100) {
    for (int64_t t = from; t < to; t += every, ++id) j.trade(t, std::to_string(id), base + id % 7);
    return id;
}
} // namespace

TEST(CandleRestore, RebuildsTheWindowAndContinuesLikeTheContinuousPath) {
    Temp temp;
    const int64_t h = 10 * Hour;
    std::vector<json> recent;
    {
        Journal j(temp.root());
        uint32_t seed = 7;
        int id = 1;
        int64_t t = h;
        int64_t nextBeat = h;
        bool resent = false, sealed = false;
        while (t < h + 40 * Minute) {
            seed = seed * 1'103'515'245u + 12'345u;
            t += 50 + int64_t(seed >> 16) % 2'950; // irregular, 50 ms .. 3 s
            if (t >= h + 15 * Minute && t < h + 15 * Minute + 35 * Second) t = h + 15 * Minute + 35 * Second;
            for (; nextBeat < t; nextBeat += Second)
                if (nextBeat < h + 15 * Minute || nextBeat >= h + 15 * Minute + 35 * Second) j.heartbeat(nextBeat);
            if (!sealed && t >= h + 20 * Minute) { j.writer->sealSegment(); sealed = true; }
            if (!resent && t >= h + 25 * Minute) {
                j.resend(t, recent); // reconnect snapshot of already delivered trades
                resent = true;
            }
            const auto trade = tradeJson(t - 150, std::to_string(id++), 100 + double(seed % 500) / 10);
            j.trades(t, {trade});
            recent.push_back(trade);
            if (recent.size() > 5) recent.erase(recent.begin());
        }
    }
    const auto through = positionAt(temp.root(), h + 38 * Minute);
    std::string failure;
    auto in = input(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_EQ(restore->files, 2u);
    EXPECT_EQ(restore->startFile, 0u);
    EXPECT_EQ(restore->clockRegressions, 0u);
    Reference ref(temp.root());
    ref.until(through);
    expectRestoreEquals(*restore, ref.aggregator);
    EXPECT_EQ(restore->tradeIds, ref.feed.tradeIds());
    EXPECT_EQ(restore->sentSecond, ref.sent);
    EXPECT_GT(restore->trades, 0u);
    EXPECT_EQ(restore->windowStartMs, restore->throughMs - kRestoreWindowMs);

    // Installed into a fresh live aggregator, the records after `through`
    // extend it exactly as they extend the continuous one.
    TimeframeAggregator live({100, 1'000, 60'000}, Caps);
    for (const auto& [tf, series] : restore->series) ASSERT_TRUE(live.restoreSeries("BTC-USD", tf, series));
    ref.also.push_back(&live);
    ref.toEnd();
    expectWindowEqual(live, ref.aggregator, restore->windowStartMs);
}

TEST(CandleRestore, SelectsTheNewestFileOpenedBeforeTheCutoff) {
    Temp temp;
    {
        Journal j(temp.root());
        for (int64_t t = Minute; t < 29 * Hour; t += Minute) j.heartbeat(t); // 29 older hourly files
        const int64_t h = 29 * Hour;
        j.heartbeat(h);
        for (int64_t t = h + Second; t < h + 50 * Minute; t += Second) {
            j.heartbeat(t);
            if (t % (3 * Second) == 0) j.trade(t + 10, std::to_string(t), double(100 + (t / Second) % 11));
        }
    }
    const auto through = positionAt(temp.root(), 29 * Hour + 50 * Minute);
    std::string failure;
    auto in = input(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->startFile, 27u); // the cutoff less the safety margin falls in 27:00
    EXPECT_EQ(restore->files, 3u);
    EXPECT_EQ(restore->reads, 1u);
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    Reference ref(temp.root());
    ref.until(through);
    expectRestoreEquals(*restore, ref.aggregator);
}

// A file whose header shows the clock already past the cutoff precedes the
// newest file opened before it: start one file before the oldest such file.
// (With 1 m in the list, so the 32-file cap, not the 4-file one, applies.)
TEST(CandleRestore, RegressionGuardStartsBeforeAClockAheadFile) {
    const int64_t ahead = 40 * Minute;
    // One run: 07:00, 09:00 (clock 40 min ahead), 08:30 (regressed, before
    // the cutoff less the margin, 08:49), then 09:00, 10:00 and 11:00.
    Temp guarded;
    {
        Journal j(guarded.root());
        int id = tradeRun(j, 7 * Hour + Second, 7 * Hour + 59 * Minute, 10 * Second, 1);
        for (int64_t t = 8 * Hour + 20 * Minute; t < 8 * Hour + 30 * Minute; t += 10 * Second, ++id)
            j.trade(t + ahead, std::to_string(id), 120, t); // receive clock ahead, exchange time true
        id = tradeRun(j, 8 * Hour + 30 * Minute + Second, 11 * Hour + 21 * Minute, 10 * Second, id);
    }
    const std::vector<int64_t> tfs{100, 1'000, 60'000};
    const auto through = positionAt(guarded.root(), 11 * Hour + 20 * Minute);
    std::string failure;
    auto in = input(guarded.root(), through);
    in.timeframesMs = tfs;
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    JournalReader inventory(guarded.root(), "BTC-USD");
    ASSERT_EQ(inventory.files().size(), 6u);
    EXPECT_EQ(restore->startFile, 0u); // without the guard: file 2 (08:30, before the cutoff)
    EXPECT_EQ(restore->files, 6u);
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_GT(restore->deepRegressions, 0u);
    EXPECT_LE(restore->deepRegressions, restore->clockRegressions);
    Reference ref(guarded.root(), tfs);
    ref.until(through);
    TimeframeAggregator window(Tfs, Caps);
    for (const int64_t tf : Tfs) ASSERT_TRUE(window.restoreSeries("BTC-USD", tf, restore->series.at(tf)));
    expectWindowEqual(window, ref.aggregator, restore->windowStartMs);
}

TEST(CandleRestore, FailsWhenTheSpanExceedsTheFileCap) {
    Temp temp;
    {
        Journal j(temp.root());
        int id = tradeRun(j, 10 * Hour, 10 * Hour + 40 * Minute, 10 * Second, 1);
        for (int restart = 0; restart < 4; ++restart) { // capture restarts inside the window
            j.newRun();
            const int64_t from = 10 * Hour + (40 + 4 * restart) * Minute;
            id = tradeRun(j, from, from + 4 * Minute, 10 * Second, id);
        }
    }
    const auto through = positionAt(temp.root(), 10 * Hour + 55 * Minute);
    std::string failure;
    auto in = input(temp.root(), through);
    in.failure = &failure;
    EXPECT_FALSE(rebuildCandles(in).has_value());
    EXPECT_EQ(failure, "file span 5");
}

TEST(CandleRestore, FailsWhenThroughIsBeyondTheJournal) {
    Temp temp;
    { Journal j(temp.root()); tradeRun(j, 10 * Hour, 10 * Hour + 5 * Minute, Second, 1); }
    auto through = positionAt(temp.root(), 10 * Hour + 5 * Minute);
    through.block += 1'000;
    std::string failure;
    auto in = input(temp.root(), through);
    in.failure = &failure;
    EXPECT_FALSE(rebuildCandles(in).has_value());
    EXPECT_TRUE(failure.starts_with("through unreadable")) << failure;
}

TEST(CandleRestore, StoppingAbortsWithinOneRecord) {
    Temp temp;
    { Journal j(temp.root()); tradeRun(j, 10 * Hour, 10 * Hour + 40 * Minute, Second, 1); }
    std::atomic<bool> stopping{false};
    int calls = 0;
    std::string failure;
    auto in = input(temp.root(), positionAt(temp.root(), 10 * Hour + 39 * Minute));
    in.stopping = &stopping;
    in.failure = &failure;
    in.afterRecordForTest = [&](const JournalRecord&) {
        ++calls;
        stopping = true;
    };
    EXPECT_FALSE(rebuildCandles(in).has_value());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(failure, "cancelled");

    // Cancelled while the terminal record is processed: still no result.
    stopping = false;
    failure.clear();
    calls = 0;
    in.afterRecordForTest = [&](const JournalRecord& r) {
        ++calls;
        if (r.pos == in.through) stopping = true;
    };
    EXPECT_FALSE(rebuildCandles(in).has_value());
    EXPECT_GT(calls, 1);
    EXPECT_EQ(failure, "cancelled");

    // Cancelled inside the replay (past the lead-in probe): no further record.
    stopping = false;
    failure.clear();
    bool set = false;
    int after = 0;
    in.afterRecordForTest = [&](const JournalRecord& r) {
        if (set) ++after;
        if (!set && receiveRel(r) >= 10 * Hour + 20 * Minute) set = stopping = true;
    };
    EXPECT_FALSE(rebuildCandles(in).has_value());
    EXPECT_TRUE(set);
    EXPECT_EQ(after, 0);
    EXPECT_EQ(failure, "cancelled");
}

TEST(CandleRestore, CountsReceiveClockRegressionsWithoutChangingTheOutcome) {
    struct Case {
        std::vector<int64_t> behind;
        uint64_t clock, deep;
    };
    for (const auto& c : std::vector<Case>{{{2 * Second}, 1, 0},
                                           {{kRestoreLeadInMs}, 1, 0},
                                           {{kRestoreLeadInMs + 1}, 1, 1},
                                           {{2 * Second, 2 * Second}, 2, 0}}) {
        SCOPED_TRACE(c.behind.front());
        Temp temp;
        const int64_t h = 10 * Hour;
        {
            Journal j(temp.root());
            int id = tradeRun(j, h + Second, h + 30 * Minute, 5 * Second, 1);
            const int64_t newest = h + 30 * Minute;
            j.heartbeat(newest);
            for (const int64_t behind : c.behind) j.heartbeat(newest - behind);
            tradeRun(j, newest + Second, h + 50 * Minute, 5 * Second, id);
        }
        const auto through = positionAt(temp.root(), h + 45 * Minute);
        std::string failure;
        auto in = input(temp.root(), through);
        in.failure = &failure;
        const auto restore = rebuildCandles(in);
        ASSERT_TRUE(restore.has_value()) << failure;
        EXPECT_EQ(restore->clockRegressions, c.clock);
        EXPECT_EQ(restore->deepRegressions, c.deep);
        EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
        Reference ref(temp.root());
        ref.until(through);
        expectRestoreEquals(*restore, ref.aggregator);
    }
}

// The last trade precedes the start file by two minutes; a delayed pre-window
// trade at another price arrives inside the read.
TEST(CandleRestore, InitialStateExtendsOneFileBackOrIsPartial) {
    const int64_t h = 11 * Hour;
    const auto lateFile = [&](Journal& j) {
        for (int64_t t = h; t < h + 32 * Minute; t += Second) {
            j.heartbeat(t);
            if (t == h + 5 * Minute) j.trade(t + 10, "late", 200, h - 30 * Second);
            if (t >= h + 20 * Minute && t % (10 * Second) == 0) j.trade(t + 10, std::to_string(t), 300, t);
        }
    };
    {
        Temp temp;
        {
            Journal j(temp.root());
            tradeRun(j, h - Hour, h - 2 * Minute + 1, 10 * Second, 1, 100);
            for (int64_t t = h - 2 * Minute + 10 * Second; t < h; t += 10 * Second) j.heartbeat(t);
            lateFile(j);
        }
        const auto through = positionAt(temp.root(), h + 31 * Minute);
        std::string failure;
        auto in = input(temp.root(), through);
        in.failure = &failure;
        const auto restore = rebuildCandles(in);
        ASSERT_TRUE(restore.has_value()) << failure;
        EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
        EXPECT_EQ(restore->startFile, 0u); // the safety margin reaches the 10:00 file
        EXPECT_EQ(restore->files, 2u);
        EXPECT_EQ(restore->reads, 1u);
        EXPECT_EQ(restore->lateDrops, 1u);
        const auto& second = restore->series.at(1'000).closed;
        ASSERT_FALSE(second.empty());
        EXPECT_EQ(second.front().timestamp_ms, restore->windowStartMs - restore->windowStartMs % 1'000);
        EXPECT_DOUBLE_EQ(second.front().close, 100 + 349 % 7); // carried from trade 349 (10:58)
        Reference ref(temp.root());
        ref.until(through);
        expectRestoreEquals(*restore, ref.aggregator);
    }
    {
        Temp temp;
        { Journal j(temp.root()); lateFile(j); }
        const auto through = positionAt(temp.root(), h + 31 * Minute);
        std::string failure;
        auto in = input(temp.root(), through);
        in.failure = &failure;
        const auto restore = rebuildCandles(in);
        ASSERT_TRUE(restore.has_value()) << failure;
        EXPECT_EQ(restore->outcome, RestoreOutcome::partial);
        EXPECT_EQ(restore->lateDrops, 1u);
        for (const int64_t tf : Tfs) {
            const auto& series = restore->series.at(tf);
            ASSERT_FALSE(series.closed.empty()) << tf;
            EXPECT_EQ(series.closed.front().timestamp_ms, Epoch + h + 20 * Minute) << tf;
            EXPECT_DOUBLE_EQ(series.closed.front().open, 300) << tf;
            EXPECT_EQ(series.floorMs, std::optional<int64_t>(Epoch + h + 20 * Minute)) << tf;
        }
    }
}

namespace {
constexpr int64_t Day = 24 * Hour;
const std::vector<int64_t> OpenTfs = restoreTimeframes({100, 1'000, 300'000, 3'600'000, 86'400'000});

// Irregular trades 0.4 .. 6 s apart at a pseudo-random price over [from, to).
int irregularRun(Journal& j, int64_t from, int64_t to, int id, uint32_t seed = 11) {
    for (int64_t t = from;;) {
        seed = seed * 1'103'515'245u + 12'345u;
        t += 400 + int64_t(seed >> 16) % 5'600;
        if (t >= to) return id;
        j.trade(t, std::to_string(id++), 100 + double(seed % 900) / 10);
    }
}

CandleRestoreInput openBarInput(const fs::path& root, const JournalPos& through,
                                std::vector<int64_t> tfs = OpenTfs) {
    auto in = input(root, through);
    in.timeframesMs = std::move(tfs);
    return in;
}

void expectOpenBarsEqual(const CandleRestore& restore, const TimeframeAggregator& expected) {
    for (const auto& [tf, series] : restore.series) {
        if (tf < Minute) continue;
        EXPECT_TRUE(series.closed.empty()) << tf; // closed 1 m+ bars come from REST
        EXPECT_TRUE(sameBar(series.active, expected.exportSeries("BTC-USD", tf).active)) << tf;
    }
}

// Every 1 m+ bar the live aggregator closed after the restart, and its open
// bars, equal the continuous path's.
void expectLiveOpenBarsEqual(const CandleRestore& restore, const TimeframeAggregator& live,
                             const TimeframeAggregator& expected) {
    for (const auto& [tf, series] : restore.series) {
        if (tf < Minute) continue;
        auto want = expected.getHistory("BTC-USD", tf, 100'000);
        std::erase_if(want, [&](const OHLCVBar& b) { return b.timestamp_ms < series.active.timestamp_ms; });
        const auto got = live.getHistory("BTC-USD", tf, 100'000);
        if (tf == Minute) ASSERT_FALSE(want.empty()); // a bucket that stays open closes none
        ASSERT_EQ(got.size(), want.size()) << tf;
        for (size_t i = 0; i < got.size(); ++i) EXPECT_TRUE(sameBar(got[i], want[i])) << tf << " bar " << i;
        EXPECT_TRUE(sameBar(live.exportSeries("BTC-USD", tf).active, expected.exportSeries("BTC-USD", tf).active))
            << tf;
    }
}
} // namespace

// Restarts inside an open 1 m, 5 m, 1 h and 1 d bucket, and one minute into a
// new day: every open bar equals the continuous path's, and live trades after
// `through` close it exactly as they close the continuous one.
TEST(CandleRestore, OpenBarsFromMinuteToDayEqualTheContinuousPath) {
    ASSERT_EQ(OpenTfs, (std::vector<int64_t>{100, 1'000, 60'000, 300'000, 3'600'000, 86'400'000}));
    Temp temp;
    {
        Journal j(temp.root());
        irregularRun(j, Day - 90 * Minute, Day + 21 * Hour + 5 * Minute, 1);
    }
    for (const int64_t at : {Day + 20 * Hour + 17 * Minute + 31 * Second, Day + Minute + 5 * Second}) {
        SCOPED_TRACE(at);
        const auto through = positionAt(temp.root(), at);
        std::string failure;
        auto in = openBarInput(temp.root(), through);
        in.failure = &failure;
        const auto restore = rebuildCandles(in);
        ASSERT_TRUE(restore.has_value()) << failure;
        EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
        EXPECT_EQ(restore->openBarsFromMs, Epoch + Day);
        EXPECT_EQ(restore->startFile, 0u); // the journal's first file, inside the margin
        EXPECT_EQ(restore->reads, 1u);
        EXPECT_EQ(restore->series.size(), OpenTfs.size());
        Reference ref(temp.root(), OpenTfs);
        ref.until(through);
        expectOpenBarsEqual(*restore, ref.aggregator);
        for (const int64_t tf : {60'000, 300'000, 3'600'000, 86'400'000}) {
            const auto& open = restore->series.at(tf).active;
            EXPECT_GT(open.count, 0u) << tf;
            EXPECT_LT(open.timestamp_ms, Epoch + at) << tf;
            EXPECT_EQ(open.timestamp_ms % tf, 0) << tf;
        }
        // 100 ms and 1 s windows are unchanged by the longer read.
        TimeframeAggregator window(Tfs, Caps);
        for (const int64_t tf : Tfs) ASSERT_TRUE(window.restoreSeries("BTC-USD", tf, restore->series.at(tf)));
        expectWindowEqual(window, ref.aggregator, restore->windowStartMs);

        TimeframeAggregator live(OpenTfs, Caps);
        for (const auto& [tf, series] : restore->series) ASSERT_TRUE(live.restoreSeries("BTC-USD", tf, series)) << tf;
        ref.also.push_back(&live);
        ref.toEnd();
        expectLiveOpenBarsEqual(*restore, live, ref.aggregator);
    }
}

// `through` is a trade, resent by a reconnect right after it: the rebuild
// counts it once, the live gate (the rebuild's ids) drops the resend, and the
// open bars continue exactly.
TEST(CandleRestore, TradeOnTheHandoffPositionCountsOnce) {
    Temp temp;
    const int64_t h = Day + 13 * Hour;
    const int64_t at = h + 42 * Minute + 17 * Second + 300;
    const json handoff = tradeJson(at - 150, "handoff", 777);
    {
        Journal j(temp.root());
        int id = irregularRun(j, Day - Hour, at - Second, 1);
        j.trades(at, {handoff});
        j.resend(at + 200, {handoff});
        irregularRun(j, at + Second, h + 50 * Minute, id, 5);
    }
    const auto through = positionAt(temp.root(), at);
    std::string failure;
    auto in = openBarInput(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_TRUE(restore->tradeIds.contains("handoff"));
    const auto& minute = restore->series.at(60'000).active;
    EXPECT_EQ(minute.timestamp_ms, Epoch + h + 42 * Minute);
    EXPECT_DOUBLE_EQ(minute.close, 777);
    Reference ref(temp.root(), OpenTfs);
    ref.until(through);
    expectOpenBarsEqual(*restore, ref.aggregator);
    ref.toEnd();

    // The live side as the tap runs it: a fresh feed from the record after
    // `through`, the rebuild's ids as the gate, watermarks after its second.
    TimeframeAggregator live(OpenTfs, Caps);
    for (const auto& [tf, series] : restore->series) ASSERT_TRUE(live.restoreSeries("BTC-USD", tf, series));
    JournalFeed feed("BTC-USD");
    int gated = 0;
    feed.onTrade = [&](const Trade& t) {
        if (restore->tradeIds.contains(t.trade_id)) return void(++gated);
        live.onTrade(t);
    };
    JournalReader reader(temp.root(), "BTC-USD");
    JournalRecord r;
    bool after = false;
    int64_t sent = restore->sentSecond;
    while (reader.next(r)) {
        if (!after) {
            after = r.pos == through;
            continue;
        }
        feed.apply(r);
        const int64_t ms = r.record.time.systemNs / 1'000'000;
        if (ms / 1000 > sent) {
            sent = ms / 1000;
            live.advance("BTC-USD", ms - 1'000);
        }
    }
    EXPECT_GE(gated, 1);
    expectLiveOpenBarsEqual(*restore, live, ref.aggregator);
}

// The journal starts after the 1 h and 1 d buckets began: those open bars hold
// what the journal holds (from its first trade), nothing is synthesized for
// the rest, and the outcome is partial.
TEST(CandleRestore, OpenBucketBeforeTheJournalIsPartial) {
    Temp temp;
    const int64_t first = Day + 10 * Hour + 5 * Minute;
    {
        Journal j(temp.root());
        j.trade(first, "first", 150);
        irregularRun(j, first, Day + 10 * Hour + 45 * Minute, 1);
    }
    const auto through = positionAt(temp.root(), Day + 10 * Hour + 40 * Minute);
    std::string failure;
    auto in = openBarInput(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::partial);
    EXPECT_EQ(restore->startFile, 0u);
    for (const int64_t tf : {3'600'000, 86'400'000}) {
        const auto& open = restore->series.at(tf).active;
        EXPECT_EQ(open.timestamp_ms, Epoch + Day + (tf == 3'600'000 ? 10 * Hour : 0)) << tf;
        EXPECT_DOUBLE_EQ(open.open, 150) << tf;
    }
    Reference ref(temp.root(), OpenTfs);
    ref.until(through);
    expectOpenBarsEqual(*restore, ref.aggregator);
}

// More capture restarts inside the day than the open-bar file cap: the read
// starts at the cap and the day bar holds only what the capped read holds.
TEST(CandleRestore, OpenBarReadStopsAtTheFileCap) {
    Temp temp;
    {
        Journal j(temp.root());
        int id = irregularRun(j, Day - Hour, Day + Minute, 1);
        for (size_t run = 0; run < kRestoreOpenBarMaxFiles + 2; ++run) {
            j.newRun();
            const int64_t from = Day + Minute + int64_t(run) * 90 * Second;
            id = irregularRun(j, from, from + 90 * Second, id, uint32_t(run) + 3);
        }
    }
    const int64_t end = Day + Minute + int64_t(kRestoreOpenBarMaxFiles + 2) * 90 * Second;
    const auto through = positionAt(temp.root(), end - 10 * Second);
    std::string failure;
    auto in = openBarInput(temp.root(), through, {60'000, 86'400'000});
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::partial);
    EXPECT_EQ(restore->files, kRestoreOpenBarMaxFiles);
    Reference ref(temp.root(), {60'000, 86'400'000});
    ref.until(through);
    const auto& day = restore->series.at(86'400'000).active;
    const auto expected = ref.aggregator.exportSeries("BTC-USD", 86'400'000).active;
    EXPECT_EQ(day.timestamp_ms, expected.timestamp_ms);
    EXPECT_GT(day.count, 0u);
    EXPECT_LT(day.count, expected.count);
    EXPECT_TRUE(sameBar(restore->series.at(60'000).active,
                        ref.aggregator.exportSeries("BTC-USD", 60'000).active));
}

TEST(CandleRestore, TimeframeSelection) {
    EXPECT_EQ(restoreTimeframes({1'000, 300'000, 604'800'000, 90'000, 100, 300'000}),
              (std::vector<int64_t>{100, 1'000, 60'000, 300'000}));
    EXPECT_EQ(restoreTimeframes({100, 1'000}), (std::vector<int64_t>{100, 1'000}));
    Temp temp;
    { Journal j(temp.root()); tradeRun(j, 10 * Hour, 10 * Hour + 5 * Minute, Second, 1); }
    const auto through = positionAt(temp.root(), 10 * Hour + 4 * Minute);
    for (const auto& [tfs, reason] : std::vector<std::pair<std::vector<int64_t>, std::string>>{
             {{100, 300'000}, "coarse timeframe without 60000"},
             {{60'000, 604'800'000}, "unsupported timeframe 604800000"}}) {
        std::string failure;
        auto in = openBarInput(temp.root(), through, tfs);
        in.failure = &failure;
        EXPECT_FALSE(rebuildCandles(in).has_value());
        EXPECT_EQ(failure, reason);
    }
}

// A thin product: no trade in the whole safety margin before midnight. One
// replay from the margin's file; no minute predates the day bucket, so the
// outcome is partial and nothing is synthesized: the open bars begin at the
// first trade, equal to the continuous path's (the skipped files hold none).
TEST(CandleRestore, ThinProductWithoutATradeInTheMarginIsPartial) {
    Temp temp;
    {
        Journal j(temp.root());
        for (int64_t t = Day - 9 * Hour; t < Day; t += 10 * Second) j.heartbeat(t);
        irregularRun(j, Day + Minute, Day + 3 * Hour, 1);
    }
    const auto through = positionAt(temp.root(), Day + 2 * Hour + 50 * Minute);
    std::string failure;
    auto in = openBarInput(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    JournalReader inventory(temp.root(), "BTC-USD");
    ASSERT_EQ(inventory.files().size(), 12u); // 15:00 .. 23:00, then 00:00 .. 02:00
    EXPECT_EQ(restore->outcome, RestoreOutcome::partial);
    EXPECT_EQ(restore->reads, 1u);
    EXPECT_EQ(restore->startFile, 6u); // 21:00: 23:59 less the 2 h margin
    Reference ref(temp.root(), OpenTfs);
    ref.until(through);
    expectOpenBarsEqual(*restore, ref.aggregator);
    const auto& day = restore->series.at(86'400'000).active;
    EXPECT_EQ(day.timestamp_ms, Epoch + Day);
    EXPECT_GT(day.count, 0u);
}

// Review regression: restart at 00:10 with open bars. The window (from 23:40)
// is carried from a 22:10 trade; the first trade of the 23:00 file (23:50)
// alone would already give a minute before midnight. The 100 ms / 1 s bars
// 23:40 .. 23:50 must still equal the continuous path's.
TEST(CandleRestore, WindowBarsBeforeTheFirstTradeOfTheLeadInFileAreCarried) {
    Temp temp;
    {
        Journal j(temp.root());
        for (int64_t t = Day - 2 * Hour; t < Day - 10 * Second; t += 10 * Second) {
            j.heartbeat(t);
            if (t == Day - 110 * Minute) j.trade(t + 10, "early", 321);
            if (t == Day - 10 * Minute) j.trade(t + 10, "evening", 333);
        }
        irregularRun(j, Day, Day + 15 * Minute, 1);
    }
    const auto through = positionAt(temp.root(), Day + 10 * Minute);
    std::string failure;
    auto in = openBarInput(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_EQ(restore->startFile, 0u);
    Reference ref(temp.root(), OpenTfs);
    ref.until(through);
    TimeframeAggregator window(Tfs, Caps);
    for (const int64_t tf : Tfs) ASSERT_TRUE(window.restoreSeries("BTC-USD", tf, restore->series.at(tf)));
    expectWindowEqual(window, ref.aggregator, restore->windowStartMs);
    const auto& seconds = restore->series.at(1'000).closed;
    ASSERT_FALSE(seconds.empty());
    EXPECT_LT(seconds.front().timestamp_ms, Epoch + Day - 10 * Minute); // carried 23:40 .. 23:50
    EXPECT_DOUBLE_EQ(seconds.front().close, 321);
    expectOpenBarsEqual(*restore, ref.aggregator);
}

// Review regression: the 22:00 file ends at 22:58:55 with heartbeats; the
// 23:00 file's first record delivers a trade stamped 22:58:30. Read from the
// 23:00 file, the minute floor (22:59) drops it; the continuous path keeps
// it, and its carried price opens the day bar at midnight.
TEST(CandleRestore, DelayedTradeAcrossAFileBoundaryOpensTheDay) {
    Temp temp;
    {
        Journal j(temp.root());
        for (int64_t t = Day - 2 * Hour; t <= Day - Hour - 65 * Second; t += 5 * Second) j.heartbeat(t);
        j.trade(Day - Hour + 200, "delayed", 555, Day - Hour - 90 * Second);
        for (int64_t t = Day - Hour + 5 * Second; t < Day; t += 5 * Second) j.heartbeat(t);
        irregularRun(j, Day + 5 * Minute, Day + Hour + 5 * Minute, 1);
    }
    JournalReader inventory(temp.root(), "BTC-USD");
    ASSERT_EQ(inventory.files().size(), 4u); // 22:00, 23:00, 00:00, 01:00
    const auto through = positionAt(temp.root(), Day + Hour);
    std::string failure;
    auto in = openBarInput(temp.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_EQ(restore->startFile, 0u);
    Reference ref(temp.root(), OpenTfs);
    ref.until(through);
    expectOpenBarsEqual(*restore, ref.aggregator);
    EXPECT_DOUBLE_EQ(restore->series.at(86'400'000).active.open, 555);
}

// A restart exactly at UTC midnight (the open day bar is still the previous
// day's: its last minute closes a lag later) and one 30 s before it: the
// day bar is rebuilt from the previous midnight, then closes on live trades
// after `through` exactly as the continuous one, and the new day opens.
TEST(CandleRestore, MidnightRestartsRebuildAndRollTheDayBar) {
    Temp temp;
    {
        Journal j(temp.root());
        int id = irregularRun(j, Day - Hour, 2 * Day - Second, 1);
        j.heartbeat(2 * Day); // a record exactly at midnight
        irregularRun(j, 2 * Day, 2 * Day + 10 * Minute, id, 5);
    }
    for (const int64_t at : {2 * Day, 2 * Day - 30 * Second}) {
        SCOPED_TRACE(at);
        const auto through = positionAt(temp.root(), at);
        std::string failure;
        auto in = openBarInput(temp.root(), through);
        in.failure = &failure;
        const auto restore = rebuildCandles(in);
        ASSERT_TRUE(restore.has_value()) << failure;
        EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
        EXPECT_EQ(restore->reads, 1u);
        EXPECT_EQ(restore->openBarsFromMs, Epoch + Day);
        EXPECT_EQ(restore->series.at(86'400'000).active.timestamp_ms, Epoch + Day);
        Reference ref(temp.root(), OpenTfs);
        ref.until(through);
        expectOpenBarsEqual(*restore, ref.aggregator);
        TimeframeAggregator live(OpenTfs, Caps);
        for (const auto& [tf, series] : restore->series) ASSERT_TRUE(live.restoreSeries("BTC-USD", tf, series));
        ref.also.push_back(&live);
        ref.toEnd();
        expectLiveOpenBarsEqual(*restore, live, ref.aggregator);
        const auto days = live.getHistory("BTC-USD", 86'400'000, 10);
        ASSERT_EQ(days.size(), 1u);
        EXPECT_EQ(days[0].timestamp_ms, Epoch + Day);
        EXPECT_EQ(live.exportSeries("BTC-USD", 86'400'000).active.timestamp_ms, Epoch + 2 * Day);
    }
}

// Review regression: a capture file whose receive clock jumped to 10:00 of the
// target day, then a correction back to the evening before, and more files
// after it than the open-bar cap. The live aggregator saw the 10:00 watermark
// and dropped the morning trades as late; a capped read starting after that
// file would not. Such a rebuild fails (nothing installed); it is never
// reported complete with bars that differ from the continuous path.
TEST(CandleRestore, ClockRegressionBeyondTheFileCapFails) {
    Temp temp;
    {
        Journal j(temp.root());
        int id = tradeRun(j, Day - 3 * Hour, Day - 2 * Hour - 5 * Minute, 30 * Second, 1);
        for (int64_t t = Day - 2 * Hour - 4 * Minute; t < Day - 2 * Hour - 2 * Minute; t += 30 * Second, ++id)
            j.trade(t + 12 * Hour + 4 * Minute, std::to_string(id), 120, t); // receive clock 12 h ahead
        for (int run = 0; run < 8; ++run) { // capture restarts in the evening
            const int64_t from = Day - 115 * Minute + run * 12 * Minute;
            id = tradeRun(j, from, from + 10 * Minute, 30 * Second, id);
            j.newRun();
        }
        tradeRun(j, Day - 15 * Minute, Day + 23 * Hour + 55 * Minute, 60 * Second, id);
    }
    const std::vector<int64_t> tfs{60'000, 86'400'000};
    JournalReader inventory(temp.root(), "BTC-USD");
    ASSERT_GT(inventory.files().size(), kRestoreOpenBarMaxFiles + 2);
    const auto through = positionAt(temp.root(), Day + 23 * Hour + 50 * Minute);
    std::string failure;
    auto in = openBarInput(temp.root(), through, tfs);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    Reference ref(temp.root(), tfs);
    ref.until(through);
    if (restore && restore->outcome == RestoreOutcome::complete)
        for (const int64_t tf : tfs)
            EXPECT_TRUE(sameBar(restore->series.at(tf).active, ref.aggregator.exportSeries("BTC-USD", tf).active))
                << tf << ": divergent bars installed as complete";
    EXPECT_FALSE(restore.has_value());
    EXPECT_EQ(failure, "clock regression beyond the file cap");
}
