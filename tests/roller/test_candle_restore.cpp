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
    TimeframeAggregator aggregator{Tfs, Caps};
    std::vector<TimeframeAggregator*> also;
    int64_t sent = INT64_MIN;
    explicit Reference(const fs::path& root) : reader(root, "BTC-USD") {
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
    EXPECT_EQ(restore->startFile, 29u);
    EXPECT_EQ(restore->files, 1u);
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    Reference ref(temp.root());
    ref.until(through);
    expectRestoreEquals(*restore, ref.aggregator);
}

// A file whose header shows the clock already past the cutoff precedes the
// newest file opened before it: start one file before the oldest such file.
TEST(CandleRestore, RegressionGuardStartsBeforeAClockAheadFile) {
    const int64_t ahead = 40 * Minute;
    // One run, four files: 09:00, 11:00 (clock 40 min ahead), 10:30
    // (regressed, before the cutoff) and 11:00 again.
    Temp guarded;
    {
        Journal j(guarded.root());
        int id = tradeRun(j, 9 * Hour + Second, 9 * Hour + 59 * Minute, 10 * Second, 1);
        for (int64_t t = 10 * Hour + 20 * Minute; t < 10 * Hour + 30 * Minute; t += 10 * Second, ++id)
            j.trade(t + ahead, std::to_string(id), 120, t); // receive clock ahead, exchange time true
        id = tradeRun(j, 10 * Hour + 30 * Minute + Second, 11 * Hour + 21 * Minute, 10 * Second, id);
    }
    const auto through = positionAt(guarded.root(), 11 * Hour + 20 * Minute);
    std::string failure;
    auto in = input(guarded.root(), through);
    in.failure = &failure;
    const auto restore = rebuildCandles(in);
    ASSERT_TRUE(restore.has_value()) << failure;
    JournalReader inventory(guarded.root(), "BTC-USD");
    ASSERT_EQ(inventory.files().size(), 4u);
    EXPECT_EQ(restore->startFile, 0u); // without the guard: file 2 (10:30, before the cutoff)
    EXPECT_EQ(restore->files, 4u);
    EXPECT_EQ(restore->outcome, RestoreOutcome::complete);
    EXPECT_GT(restore->deepRegressions, 0u);
    EXPECT_LE(restore->deepRegressions, restore->clockRegressions);
    Reference ref(guarded.root());
    ref.until(through);
    expectRestoreEquals(*restore, ref.aggregator);
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
        EXPECT_EQ(restore->startFile, 0u); // extended one file back from the 11:00 file
        EXPECT_EQ(restore->files, 2u);
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
