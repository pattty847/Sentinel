// Raw TLS peers count candle frames before any client-side filter. Selection
// tests also exercise quiet snapshots, replacement and reconnect boundaries.
#include "CandleWireFixture.hpp"
#include "trading/MarketEventSource.hpp"
#include <QScopeGuard>
#include <gtest/gtest.h>

namespace {
std::atomic<bool> fetchFailed{false}, unsupported{false}, writeOverflow{false};
void errorLogger(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.contains("Server error:") && message.contains("fetch failed")) fetchFailed = true;
    if (message.contains("Server error:") && message.contains("unsupported timeframe_ms")) unsupported = true;
    if (message.contains("write backlog exceeded")) writeOverflow = true;
}
struct Event {
    bool closed;
    int64_t tf, start, end, seq;
    double volume, open, high, low, close;
};
struct OfflineCandle {
    int argc = 1;
    char name[20] = "candle-offline";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    ServerConfig config;
    ServerDataModel model{config};
    Authenticator auth{"/private/tmp/sentinel-candle-no-credentials"};
    SentinelStreamServer server{model, auth, config, 0};
    std::vector<Event> events;
    std::shared_ptr<Session> session = RecordingServerStopTest::offline(
        server, model, [this](std::string payload) {
            const auto frame = Json::parse(payload);
            if (frame.value("type", "") != "candle_bar_update") return;
            const auto& bar = frame.at("candle");
            events.push_back({false, frame.at("timeframe_ms").get<int64_t>(),
                frame.at("bucket_start_ms").get<int64_t>(), bar.at("time_end_ms").get<int64_t>(),
                frame.at("seq").get<int64_t>(), bar.at("volume").get<double>(),
                bar.at("open").get<double>(), bar.at("high").get<double>(),
                bar.at("low").get<double>(), bar.at("close").get<double>()});
        });
    OfflineCandle() { RecordingServerStopTest::offlineBookTick(*session, "BTC-USD", 0.1); }
    std::string selectedSymbol;
    int64_t selectedTf = 0;
    uint64_t selection = 0;
    void select(const char* symbol, int64_t tf) {
        if (tf < 60000 || (selectedSymbol == symbol && selectedTf == tf)) return;
        selectedSymbol = symbol;
        selectedTf = tf;
        RecordingServerStopTest::offlineSelect(*session, symbol, tf, ++selection);
    }
    void send(int64_t tf, const OHLCVBar& bar) {
        select("BTC-USD", tf);
        RecordingServerStopTest::offlineBar(*session, tf, bar);
    }
    void send(const char* symbol, int64_t tf, const OHLCVBar& bar) {
        select(symbol, tf);
        RecordingServerStopTest::offlineBar(*session, symbol, tf, bar);
    }
    size_t count(int64_t tf) const {
        return std::count_if(events.begin(), events.end(), [tf](const Event& event) { return event.tf == tf; });
    }
    void runTimer() {
        auto& io = RecordingServerStopTest::io(server);
        io.restart();
        io.run_for(300ms);
    }
};

TEST(CandleGateOffline, ImmediateTickAndTimerReplayWithoutNetworkSockets) {
    OfflineCandle fixture;
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(100, bar);
    fixture.send(1000, bar);
    bar.volume = 2;
    bar.count = 2;
    fixture.send(100, bar);
    fixture.send(1000, bar);
    EXPECT_EQ(fixture.count(100), 1u);
    EXPECT_EQ(fixture.count(1000), 1u);

    bar.volume = 1;
    bar.count = 1;
    fixture.send(60000, bar);
    bar.close = 100.1;
    bar.volume = 2;
    bar.count = 2;
    fixture.send(60000, bar);
    ASSERT_EQ(fixture.count(60000), 2u);
    EXPECT_DOUBLE_EQ(fixture.events.back().close, 100.1);

    bar.close = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(300000, bar);
    for (int i = 2; i <= 1001; ++i) {
        bar.volume = i;
        bar.count = i;
        fixture.send(300000, bar);
    }
    EXPECT_EQ(fixture.count(300000), 1u);
    fixture.runTimer();
    ASSERT_EQ(fixture.count(300000), 2u);
    EXPECT_DOUBLE_EQ(fixture.events.back().volume, 1001);

    bar.close = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(900000, bar);
    bar.close = 100.05;
    bar.volume = 2;
    bar.count = 2;
    fixture.send(900000, bar);
    EXPECT_EQ(fixture.count(900000), 1u);
    fixture.runTimer();
    ASSERT_EQ(fixture.count(900000), 2u);
    EXPECT_DOUBLE_EQ(fixture.events.back().close, 100.05);
    EXPECT_EQ(fixture.count(100), 1u);
    EXPECT_EQ(fixture.count(1000), 1u);

    bar.close = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(3600000, bar);
    for (int i = 1; i <= 1000; ++i) {
        bar.close = i % 2 ? 100.1 : 100;
        bar.volume = i + 1;
        bar.count = i + 1;
        fixture.send(3600000, bar);
    }
    EXPECT_EQ(fixture.count(3600000), 1001u);
    std::cout << "CANDLE_BUSY_REPLAY_BEFORE=1 AFTER=" << fixture.count(3600000) << '\n';
}

TEST(CandleGateOffline, NonBtcBookTickIsImmediateAndRefreshes) {
    OfflineCandle fixture;
    RecordingServerStopTest::offlineSubscribe(*fixture.session, "SOL-USD");
    RecordingServerStopTest::offlineBookTick(*fixture.session, "SOL-USD", 0.01);
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    fixture.send("SOL-USD", 14400000, bar);
    bar.close = 100.01;
    bar.volume = 2;
    bar.count = 2;
    fixture.send("SOL-USD", 14400000, bar);
    ASSERT_EQ(fixture.count(14400000), 2u);
    RecordingServerStopTest::offlineBookTick(*fixture.session, "SOL-USD", 0.05);
    bar.close = 100.02;
    bar.volume = 3;
    bar.count = 3;
    fixture.send("SOL-USD", 14400000, bar);
    EXPECT_EQ(fixture.count(14400000), 2u);
    fixture.runTimer();
    ASSERT_EQ(fixture.count(14400000), 3u);
    EXPECT_DOUBLE_EQ(fixture.events.back().close, 100.02);
}

TEST(CandleGateOffline, UnsubscribeDropsPendingBeforeResubscribe) {
    OfflineCandle fixture;
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(60000, bar);
    bar.close = 100.05;
    bar.volume = 2;
    bar.count = 2;
    fixture.send(60000, bar);
    ASSERT_EQ(fixture.count(60000), 1u);
    RecordingServerStopTest::offlineUnsubscribe(*fixture.session, "BTC-USD");
    RecordingServerStopTest::offlineSubscribe(*fixture.session, "BTC-USD");
    RecordingServerStopTest::offlineBookTick(*fixture.session, "BTC-USD", 0.1);
    fixture.runTimer();
    EXPECT_EQ(fixture.count(60000), 1u);
}

TEST(CandleGateOffline, ClosedBucketSupersedesPendingTimerUpdate) {
    OfflineCandle fixture;
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = bar.high = bar.low = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(60000, bar);
    bar.volume = 2;
    bar.count = 2;
    fixture.send(60000, bar);
    ASSERT_EQ(fixture.count(60000), 1u);
    bar.is_closed = true;
    RecordingServerStopTest::offlineClosed(*fixture.session, 60000, bar);
    fixture.runTimer();
    EXPECT_EQ(fixture.count(60000), 1u);
}

TEST(CandleGateOffline, SessionCloseCancelsPendingTimer) {
    OfflineCandle fixture;
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = bar.high = bar.low = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(60000, bar);
    bar.volume = 2;
    bar.count = 2;
    fixture.send(60000, bar);
    RecordingServerStopTest::offlineClose(*fixture.session);
    fixture.runTimer();
    EXPECT_EQ(fixture.count(60000), 1u);
}

TEST(CandleGateOffline, SelectionReplacementDiscardsPendingOldTimeframe) {
    OfflineCandle fixture;
    const auto select = [&](int64_t tf, uint64_t generation) {
        RecordingServerStopTest::offlineMessage(*fixture.session,
            {{"type", "candle_live_select"}, {"symbol", "BTC-USD"},
             {"timeframe_ms", tf}, {"selection", generation}});
    };
    select(60000, 1);
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.high = bar.low = bar.close = 100;
    bar.volume = 1;
    bar.count = 1;
    fixture.send(60000, bar);
    bar.volume = 2;
    bar.count = 2;
    fixture.send(60000, bar); // pending volume-only update
    select(300000, 2);
    fixture.runTimer();
    EXPECT_EQ(fixture.count(60000), 1u);
    select(60000, 3);
    bar.volume = 3;
    bar.count = 3;
    fixture.send(60000, bar);
    fixture.runTimer();
    ASSERT_EQ(fixture.count(60000), 2u);
    EXPECT_DOUBLE_EQ(fixture.events.back().volume, 3);
}

// Opt-in recorded-hour measurement. Timer deadlines use exchange time as a
// virtual clock so an hour of trades can be replayed without an hour of sleep.
TEST(CandleRealHourReplay, ReportsUpdateCountsByTimeframe) {
    const auto root = qEnvironmentVariable("SENTINEL_CANDLE_REPLAY_ROOT");
    if (root.isEmpty()) GTEST_SKIP() << "set SENTINEL_CANDLE_REPLAY_ROOT to a read-only RAWL2 root";
    constexpr int64_t fromMs = 1'795'618'800'000; // 2026-11-25 15:00 UTC
    constexpr int64_t toMs = fromMs + 3'600'000;
    constexpr double tick = 0.1;
    struct RateState {
        OHLCVBar sent{}, pendingBar{};
        int64_t sentAt = 0;
        size_t updates = 0, timerUpdates = 0;
        bool hasSent = false, pending = false;
        void publish(const OHLCVBar& bar, int64_t now, bool timer = false) {
            sent = bar; sentAt = now; hasSent = true; pending = false; ++updates;
            if (timer) ++timerUpdates;
        }
        void closed(const OHLCVBar& bar, int64_t now) {
            sent = bar; sentAt = now; hasSent = true; pending = false;
        }
    };
    const auto oldGate = [tick](const OHLCVBar& bar, const RateState& state, int64_t tf, int64_t now) {
        if (!state.hasSent) return true;
        const auto& last = state.sent;
        if (tf < 1000 && (bar.timestamp_ms != last.timestamp_ms || (last.volume <= 0 && bar.volume > 0)))
            return true;
        if (tf >= 60000 && bar.count > 0 &&
            (bar.timestamp_ms != last.timestamp_ms || last.count == 0)) return true;
        const double closeDelta = std::abs(bar.close - last.close);
        const double threshold = tf <= 1000 ? std::max(tick, bar.close * 0.00005)
                                            : std::max(2 * tick, bar.close * 0.0002);
        return bar.high > last.high || bar.low < last.low || closeDelta >= threshold ||
            now - state.sentAt >= (tf <= 1000 ? 200 : 1000);
    };
    std::map<int64_t, RateState> before, after;
    const auto timeframes = std::vector<int64_t>{100, 1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    TimeframeAggregator aggregator(timeframes);
    int64_t nowMs = fromMs;
    int64_t timerDeadline = 0;
    bool timerArmed = false;
    auto flushTimer = [&](int64_t now) {
        if (!timerArmed || timerDeadline > now) return;
        for (auto& [tf, state] : after)
            if (state.pending) state.publish(state.pendingBar, timerDeadline, true);
        timerArmed = false;
    };
    auto updated = [&](int64_t tf, const OHLCVBar& bar) {
        auto& old = before[tf];
        auto& next = after[tf];
        flushTimer(nowMs);
        if (oldGate(bar, old, tf, nowMs)) old.publish(bar, nowMs);
        const bool immediate = tf <= 1000 ? oldGate(bar, next, tf, nowMs) :
            !next.hasSent || bar.timestamp_ms != next.sent.timestamp_ms ||
            (bar.count > 0 && next.sent.count == 0) || bar.high > next.sent.high ||
            bar.low < next.sent.low ||
            std::abs(bar.close - next.sent.close) + tick * 1e-9 >= tick;
        if (immediate) next.publish(bar, nowMs);
        else if (tf >= 60000) {
            next.pendingBar = bar;
            next.pending = true;
            if (!timerArmed) {
                timerDeadline = nowMs + 200;
                timerArmed = true;
            }
        }
    };
    QObject::connect(&aggregator, &TimeframeAggregator::barUpdated, &aggregator,
        [&](const QString&, int64_t tf, const OHLCVBar& bar) { updated(tf, bar); });
    QObject::connect(&aggregator, &TimeframeAggregator::liveRollupsUpdated, &aggregator,
        [&](const QString&, const std::vector<TimeframeBar>& bars) {
            for (const auto& item : bars) updated(item.timeframeMs, item.bar);
        });
    QObject::connect(&aggregator, &TimeframeAggregator::barClosed, &aggregator,
        [&](const QString&, int64_t tf, const OHLCVBar& bar) {
            flushTimer(nowMs);
            before[tf].closed(bar, nowMs);
            after[tf].closed(bar, nowMs);
        });
    trading::JournalTradeEventSource source(root.toStdString(), "BTC-USD", fromMs, toMs);
    size_t trades = 0;
    while (const auto event = source.next()) {
        if (!event->trade) continue;
        const auto& row = *event->trade;
        nowMs = std::max(nowMs, row.timestampMs);
        Trade trade{std::chrono::system_clock::time_point(std::chrono::milliseconds(row.timestampMs)),
                    row.symbol, row.tradeId, row.side, row.price, row.qty};
        aggregator.onTrade(trade);
        ++trades;
    }
    ASSERT_GT(trades, 1000u);
    flushTimer(nowMs + 200);
    for (const auto tf : timeframes) {
        std::cout << "CANDLE_REAL_REPLAY tf_ms=" << tf << " trades=" << trades
                  << " before=" << before[tf].updates << " after=" << after[tf].updates
                  << " timer=" << after[tf].timerUpdates << '\n';
        if (tf <= 1000) EXPECT_EQ(after[tf].updates, before[tf].updates);
    }
}

class CandleWire : public testing::Test {
  protected:
    int argc = 1;
    char name[20] = "candle-wire";
    char *argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    CandleFixture fixture{true, 0, {300000}};
    std::unique_ptr<SentinelStreamClient> client;
    std::vector<Event> events;
    QVector<SentinelStreamClient::CandleBar> page;
    int64_t pageTf = 0, pageEnd = 0;
    int pages = 0;
    int failures = 0;
    std::shared_ptr<Session> session;
    void SetUp() override {
        // The subscribe snapshot must carry the BTC wire tick for close-move tests.
        fixture.model->onLiveOrderBookInitialized("BTC-USD", {{99.9, 1}}, {{100.1, 1}}, 1000);
        const auto book = fixture.model->ensureSymbol("BTC-USD").liveBook.snapshotLevels();
        ASSERT_DOUBLE_EQ(book.tickSize, 0.1);
        ASSERT_EQ(book.bids.size(), 1u);
        ASSERT_EQ(book.asks.size(), 1u);
        fixture.start();
        client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(fixture.port()),
                                                        fixture.config.tls.certFile);
        QObject::connect(client.get(), &SentinelStreamClient::connected, &app,
                         [this] { client->subscribe("BTC-USD"); client->selectLiveCandle("BTC-USD", 300000); });
        QObject::connect(client.get(), &SentinelStreamClient::candleBarUpdateReceived, &app,
                         [this](const QString &, int64_t tf, int64_t start, int64_t seq, const auto &bar, quint64) {
                             events.push_back({false, tf, start, bar.timeEndMs, seq, bar.volume, bar.open, bar.high,
                                               bar.low, bar.close});
                         });
        QObject::connect(client.get(), &SentinelStreamClient::candleBarClosedReceived, &app,
                         [this](const QString &, int64_t tf, int64_t start, int64_t seq, const auto &bar, quint64) {
                             events.push_back({true, tf, start, bar.timeEndMs, seq, bar.volume, bar.open, bar.high,
                                               bar.low, bar.close});
                         });
        QObject::connect(client.get(), &SentinelStreamClient::candleHistoryReceived, &app,
                         [this](const QString &, int64_t tf, int64_t, int64_t end, const auto &bars) {
                             page = bars;
                             pageTf = tf;
                             pageEnd = end;
                             ++pages;
                         });
        QObject::connect(client.get(), &SentinelStreamClient::candleHistoryFailed, &app,
                         [this](const QString &) { ++failures; });
        client->connectToServer();
        ASSERT_TRUE(spinUntil([&] {
            auto sessions = RecordingServerStopTest::sessions(*fixture.server);
            if (sessions.empty()) return false;
            session = sessions.front();
            return fixture.subscribed(session) && fixture.selected(session, 300000);
        }));
    }
    void TearDown() override {
        if (client) client->disconnectFromServer();
        client.reset();
    }
    quint64 selectionGeneration = 0;
    void select(int64_t tf) {
        client->setCandleDeliveryGeneration(++selectionGeneration);
        client->selectLiveCandle("BTC-USD", tf);
        ASSERT_TRUE(spinUntil([&] { return fixture.selected(session, tf); }));
    }
    void fetch(int64_t tf, int64_t end) {
        const int before = pages;
        client->requestCandleHistory("BTC-USD", tf, end, 350);
        ASSERT_TRUE(spinUntil([&] { return pages > before; }));
        EXPECT_EQ(pageTf, tf);
        EXPECT_EQ(pageEnd, end);
    }
};

TEST_F(CandleWire, HundredMillisecondsHasIndependentSequenceAndFirstTradeAfterEmptyOpen) {
    tradeAt(*fixture.model, 1000);
    tradeAt(*fixture.model, 1100);
    fixture.model->onFeedWatermark("BTC-USD", 2200);
    ASSERT_TRUE(spinUntil(
        [&] { return std::count_if(events.begin(), events.end(), [](const auto &e) { return e.tf == 100; }) >= 6; }));
    std::map<int64_t, int64_t> seq;
    std::vector<std::tuple<bool, int64_t, double>> hundred;
    for (const auto &e : events) {
        EXPECT_EQ(e.end, e.start + e.tf);
        EXPECT_EQ(e.seq, ++seq[e.tf]);
        if (e.tf == 100) hundred.emplace_back(e.closed, e.start, e.volume);
    }
    EXPECT_GE(seq[1000], 1);
    EXPECT_EQ(
        hundred,
        (std::vector<std::tuple<bool, int64_t, double>>{
            {false, 1000, 1}, {true, 1000, 1}, {false, 1100, 0}, {false, 1100, 1}, {true, 1100, 1}, {false, 1200, 0}}));
}

TEST_F(CandleWire, OneSecondSequenceMatchesBaseRun) {
    tradeAt(*fixture.model, 1000);
    tradeAt(*fixture.model, 2000);
    fixture.model->onFeedWatermark("BTC-USD", 4000);
    ASSERT_TRUE(spinUntil(
        [&] { return std::count_if(events.begin(), events.end(), [](const auto &e) { return e.tf == 1000; }) >= 3; }));
    ASSERT_TRUE(fixture.fence());
    std::vector<std::tuple<bool, int64_t, double>> seconds;
    int64_t seq = 0;
    for (const auto &e : events) {
        if (e.tf != 1000) continue;
        EXPECT_EQ(e.seq, ++seq);
        EXPECT_EQ(e.end, e.start + 1000);
        EXPECT_EQ(e.open, 100);
        EXPECT_EQ(e.high, 100);
        EXPECT_EQ(e.low, 100);
        EXPECT_EQ(e.close, 100);
        seconds.emplace_back(e.closed, e.start, e.volume);
    }
    // Recorded by executing the c852038d Session with the same journal trades/watermark.
    EXPECT_EQ(seconds,
              (std::vector<std::tuple<bool, int64_t, double>>{{false, 1000, 1}, {true, 1000, 1}, {true, 2000, 1}}));
}

// The first trade of a minute or longer bar is sent at once, though it moves
// no price and adds no volume over the bar just closed (inside the silence
// window), and before any minute of a new 5 m period closes.
TEST_F(CandleWire, MinuteAndFiveMinuteBarsAreSentAtTheirFirstTrade) {
    select(60000);
    tradeAt(*fixture.model, 60000);
    ASSERT_TRUE(spinUntil([&] {
        return std::any_of(events.begin(), events.end(), [](const auto& e) {
            return e.tf == 60000 && !e.closed && e.start == 60000 && e.volume == 1;
        });
    }));
    select(300000);
    tradeAt(*fixture.model, 300000);
    ASSERT_TRUE(spinUntil([&] {
        return std::any_of(events.begin(), events.end(),
                           [](const auto &e) { return e.tf == 300000 && e.closed && e.start == 0; });
    }));
    ASSERT_TRUE(fixture.fence());
    for (const int64_t tf : {60000, 300000}) {
        EXPECT_TRUE(spinUntil([&] {
            return std::any_of(events.begin(), events.end(), [tf](const auto &e) {
                return e.tf == tf && !e.closed && e.start == tf && e.volume == 1 && e.open == 100;
            });
        }, 500ms)) << tf;
    }
}

// A sent empty (carried) bar is replaced by its first trade at once, though
// that trade is in the same bucket, moves no price and is inside the silence window.
TEST_F(CandleWire, CarriedBarIsReplacedByItsFirstTrade) {
    OHLCVBar carried{};
    carried.timestamp_ms = 300000;
    carried.open = carried.high = carried.low = carried.close = 100;
    OHLCVBar traded = carried;
    traded.volume = 1;
    traded.count = 1;
    RecordingServerStopTest::bar(*session, 300000, carried);
    RecordingServerStopTest::bar(*session, 300000, traded);
    ASSERT_TRUE(fixture.fence());
    EXPECT_TRUE(spinUntil([&] {
        return std::any_of(events.begin(), events.end(),
                           [](const auto &e) { return e.tf == 300000 && !e.closed && e.volume == 1; });
    }, 500ms));
    // Later volume-only updates wait for the coalescing timer.
    OHLCVBar more = traded;
    more.volume = 2;
    more.count = 2;
    RecordingServerStopTest::bar(*session, 300000, more);
    ASSERT_TRUE(fixture.fence());
    EXPECT_TRUE(spinUntil([&] {
        return std::any_of(events.begin(), events.end(), [](const auto &e) { return e.tf == 300000 && e.volume == 2; });
    }, 500ms));
}

TEST_F(CandleWire, OneTickCloseMoveIsSentBeforeTheCoalescingTimer) {
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(spinUntil([&] { return events.size() == 1; }));
    bar.close = 100.1;
    bar.volume = 2;
    bar.count = 2;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(fixture.fence());
    EXPECT_TRUE(spinUntil([&] { return events.size() == 2; }, 75ms));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_DOUBLE_EQ(events.back().close, 100.1);
}

TEST_F(CandleWire, VolumeOnlyBurstCoalescesWithoutAnotherTrade) {
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = bar.high = bar.low = 100;
    bar.volume = 1;
    bar.count = 1;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(spinUntil([&] { return events.size() == 1; }));
    const auto started = std::chrono::steady_clock::now();
    for (int i = 2; i <= 1001; ++i) {
        bar.volume = i;
        bar.count = i;
        RecordingServerStopTest::bar(*session, 300000, bar);
        if (i % 100 == 0) ASSERT_TRUE(fixture.fence());
    }
    ASSERT_TRUE(fixture.fence());
    ASSERT_TRUE(spinUntil([&] { return !events.empty() && events.back().volume == 1001; }, 1000ms));
    const auto count = events.size();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    EXPECT_LE(count, size_t(2 + elapsed / 200)); // initial update plus elapsed timer turns
    EXPECT_DOUBLE_EQ(events.back().close, 100);
}

TEST_F(CandleWire, QuietSubtickCloseIsFlushedByTimer) {
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(spinUntil([&] { return events.size() == 1; }));
    bar.close = 100.05;
    bar.volume = 2;
    bar.count = 2;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(fixture.fence());
    EXPECT_EQ(events.size(), 1u);
    ASSERT_TRUE(spinUntil([&] { return events.size() == 2; }, 500ms));
    EXPECT_DOUBLE_EQ(events.back().close, 100.05);
}

TEST_F(CandleWire, BusyTickReplayReportsMessageRate) {
    OHLCVBar bar{};
    bar.timestamp_ms = 300000;
    bar.open = bar.close = 100;
    bar.high = 101;
    bar.low = 99;
    bar.volume = 1;
    bar.count = 1;
    RecordingServerStopTest::bar(*session, 300000, bar);
    ASSERT_TRUE(spinUntil([&] { return events.size() == 1; }));
    for (int i = 1; i <= 1000; ++i) {
        bar.close = i % 2 ? 100.1 : 100;
        bar.volume = i + 1;
        bar.count = i + 1;
        RecordingServerStopTest::bar(*session, 300000, bar);
        if (i % 100 == 0) ASSERT_TRUE(fixture.fence());
    }
    ASSERT_TRUE(spinUntil([&] { return events.size() == 1001; }, 3000ms));
    // The previous max(2 ticks, 2 bps) gate sent only the initial bar here.
    std::cout << "CANDLE_BUSY_REPLAY_BEFORE=1 AFTER=" << events.size() << '\n';
}

TEST_F(CandleWire, PagesBothMemoryIntervalsAndRejectsUnsupportedOrFailedRest) {
    // Keep fan-out below the unrelated executor limit while seeding 80 seconds.
    for (int i = 0; i < 800; ++i) {
        tradeAt(*fixture.model, 100000 + i * 100);
        if (i % 20 == 0) ASSERT_TRUE(fixture.fence());
    }
    fixture.model->onFeedWatermark("BTC-USD", 181000);
    fetch(100, 179950);
    ASSERT_EQ(page.size(), 350);
    EXPECT_EQ(page.back().timeStartMs, 179900);
    const int64_t oldest = page.front().timeStartMs;
    fetch(100, oldest - 100);
    ASSERT_EQ(page.size(), 350);
    EXPECT_EQ(page.back().timeStartMs, oldest - 100);
    for (const auto &b : page) EXPECT_EQ(b.timeEndMs, b.timeStartMs + 100);
    fetch(1000, 179000);
    ASSERT_FALSE(page.empty());
    EXPECT_EQ(page.back().timeStartMs, 179000);
    fetch(1000, 129000);
    ASSERT_FALSE(page.empty());
    EXPECT_EQ(page.back().timeStartMs, 129000);
    fetchFailed = unsupported = false;
    const auto oldHandler = qInstallMessageHandler(errorLogger);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(oldHandler); });
    client->requestCandleHistory("BTC-USD", 60000, 172860000, 1);
    ASSERT_TRUE(spinUntil([&] { return failures == 1 && fetchFailed.load(); }));
    client->requestCandleHistory("BTC-USD", 250, 172860000, 1);
    ASSERT_TRUE(spinUntil([&] { return failures == 2 && unsupported.load(); }));
}

std::atomic<bool> schemaDrop{false};
void schemaLogger(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.contains("candle_schema") ||
        (message.contains("candle") && message.contains("unsupported schema_version=1")))
        schemaDrop = true;
}
TEST_F(CandleWire, SchemaOneIsDroppedBeforeDelivery) {
    schemaDrop = false;
    auto old = qInstallMessageHandler(schemaLogger);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    RecordingServerStopTest::send(*session,
                                  {{"type", "candle_bar_update"},
                                   {"schema_version", 1},
                                   {"symbol", "BTC-USD"},
                                   {"timeframe_ms", 100},
                                   {"seq", 1},
                                   {"bucket_start_ms", 1000},
                                   {"candle", {{"time_start_ms", 1000}, {"time_end_ms", 1100}, {"volume", 1}}}});
    ASSERT_TRUE(spinUntil([] { return schemaDrop.load(); }));
    EXPECT_TRUE(events.empty());
}

TEST_F(CandleWire, ModelEventLimitClosesOn2049thPost) {
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    RecordingServerStopTest::park(*session, entered, release.get_future().share());
    auto unpark = qScopeGuard([&] { release.set_value(); });
    ASSERT_EQ(enteredFuture.wait_for(2s), std::future_status::ready);
    for (size_t i = 0; i < 2048; ++i) RecordingServerStopTest::post(*session);
    EXPECT_EQ(RecordingServerStopTest::pendingModelEvents(*session), 2048);
    EXPECT_FALSE(RecordingServerStopTest::closePosted(*session));
    RecordingServerStopTest::post(*session);
    EXPECT_EQ(RecordingServerStopTest::pendingModelEvents(*session), 2048);
    EXPECT_TRUE(RecordingServerStopTest::closePosted(*session));
    unpark.dismiss();
    release.set_value();
    ASSERT_TRUE(spinUntil([&] { return RecordingServerStopTest::closing(*session); }));
    ASSERT_TRUE(spinUntil([&] { return RecordingServerStopTest::sessions(*fixture.server).empty(); }));
}

TEST(CandleWireLimits, StalledReaderHitsWriteBytesWithExecutorDraining) {
    int argc = 1;
    char name[] = "candle-limit";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    CandleFixture fixture;
    fixture.start();
    StalledCandleClient client(fixture.port());
    auto sessions = RecordingServerStopTest::sessions(*fixture.server);
    ASSERT_EQ(sessions.size(), 1);
    auto session = sessions.front();
    ASSERT_TRUE(spinUntil([&] { return fixture.subscribed(session); }));
    writeOverflow = false;
    const auto oldHandler = qInstallMessageHandler(errorLogger);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(oldHandler); });
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    int64_t start = 1000;
    while (!RecordingServerStopTest::closePosted(*session) && std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 64; ++i, start += 100) RecordingServerStopTest::candle(*session, start);
        ASSERT_TRUE(fixture.fence());
        EXPECT_LE(RecordingServerStopTest::pendingWriteBytes(*session), 16U * 1024U * 1024U);
    }
    EXPECT_TRUE(RecordingServerStopTest::closePosted(*session));
    EXPECT_TRUE(writeOverflow.load());
    // Each emitted candle is below 512 bytes: the refusal must occur at the 16 MiB boundary.
    EXPECT_GT(RecordingServerStopTest::peakWriteBytes(*session), 16U * 1024U * 1024U - 512U);
    EXPECT_LE(RecordingServerStopTest::peakWriteBytes(*session), 16U * 1024U * 1024U);
    EXPECT_LT(RecordingServerStopTest::peakModelEvents(*session), 256);
    EXPECT_TRUE(spinUntil([&] { return RecordingServerStopTest::closing(*session); }));
}

// A synchronous raw client: the reply exactly as the server wrote it.
struct RawCandleClient : StalledCandleClient {
    using StalledCandleClient::StalledCandleClient;
    Json request(int64_t tf, int64_t end) {
        ws.write(net::buffer(Json{{"type", "candle_history_request"},
                                  {"symbol", "BTC-USD"},
                                  {"timeframe_ms", tf},
                                  {"end_time_ms", end},
                                  {"limit", 350}}
                                 .dump()));
        for (;;) {
            beast::flat_buffer b;
            ws.read(b);
            auto m = Json::parse(beast::buffers_to_string(b.data()));
            const auto type = m.value("type", "");
            if (type == "candle_history_chunk" || type == "error") return m;
        }
    }
};

// Reads asynchronously so the model's Qt executor can service selection snapshots.
// Every wait has a deadline, including when run against a server without selection.
struct ActiveCandlePeer : StalledCandleClient {
    std::vector<Json> candles;
    using StalledCandleClient::StalledCandleClient;
    void write(const Json& message) { ws.write(net::buffer(message.dump())); }
    bool until(const std::string& type) {
        for (;;) {
            beast::flat_buffer buffer;
            bool done = false;
            beast::error_code error;
            ws.async_read(buffer, [&](beast::error_code ec, size_t) { error = ec; done = true; });
            const bool ready = spinUntil([&] { io.restart(); io.poll(); return done; });
            if (!ready) {
                beast::error_code ignored;
                beast::get_lowest_layer(ws).socket().cancel(ignored);
                io.restart(); io.run();
                return false;
            }
            if (error) return false;
            const auto frame = Json::parse(beast::buffers_to_string(buffer.data()));
            const auto received = frame.value("type", "");
            if (received == "candle_bar_update" || received == "candle_bar_closed") candles.push_back(frame);
            if (received == type) return true;
        }
    }
    bool select(const std::string& symbol, int64_t tf, uint64_t generation) {
        write({{"type", "candle_live_select"}, {"symbol", symbol},
               {"timeframe_ms", tf}, {"selection", generation}});
        return until("candle_live_selected");
    }
    bool drain(CandleFixture& fixture) {
        if (!fixture.fence()) return false;
        // A history reply is a wire fence after all previously posted bar writes.
        write({{"type", "candle_history_request"}, {"symbol", "BTC-USD"},
               {"timeframe_ms", 100}, {"end_time_ms", 600000}, {"limit", 1}});
        return until("candle_history_chunk");
    }
    std::vector<Json> minutes() const {
        std::vector<Json> result;
        for (const auto& frame : candles)
            if (frame.at("timeframe_ms").get<int64_t>() >= 60000) result.push_back(frame);
        return result;
    }
};

class CandleSelectionWire : public testing::Test {
protected:
    int argc = 1;
    char name[24] = "candle-selection";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    CandleFixture fixture{true, 0, {300000}};
    void SetUp() override { fixture.start(); }
};

TEST_F(CandleSelectionWire, UndeclaredSessionSendsNoMinuteFramesButKeepsSubminuteStreams) {
    ActiveCandlePeer peer(fixture.port());
    ASSERT_TRUE(peer.until("ack"));
    tradeAt(*fixture.model, 300000);
    tradeAt(*fixture.model, 360000);
    ASSERT_TRUE(peer.drain(fixture));
    EXPECT_TRUE(peer.minutes().empty());
    EXPECT_TRUE(std::any_of(peer.candles.begin(), peer.candles.end(), [](const Json& frame) {
        return frame.at("timeframe_ms") == 1000;
    }));
}

TEST_F(CandleSelectionWire, TwoSessionsReceiveOnlyTheirSelectedUpdatesAndCloses) {
    ActiveCandlePeer minute(fixture.port()), five(fixture.port());
    ASSERT_TRUE(minute.until("ack"));
    ASSERT_TRUE(five.until("ack"));
    ASSERT_TRUE(minute.select("BTC-USD", 60000, 1));
    ASSERT_TRUE(five.select("BTC-USD", 300000, 1));
    tradeAt(*fixture.model, 540000);
    tradeAt(*fixture.model, 600000);
    ASSERT_TRUE(minute.drain(fixture));
    ASSERT_TRUE(five.drain(fixture));
    for (const auto& [peer, tf] : {std::pair{&minute, int64_t{60000}}, {&five, int64_t{300000}}}) {
        const auto frames = peer->minutes();
        ASSERT_FALSE(frames.empty());
        int closes = 0, forming = 0;
        int64_t seq = 0;
        for (const auto& frame : frames) {
            EXPECT_EQ(frame.at("timeframe_ms"), tf);
            EXPECT_EQ(frame.at("selection"), 1);
            EXPECT_EQ(frame.at("seq"), ++seq);
            closes += frame.at("type") == "candle_bar_closed";
            forming += frame.at("type") == "candle_bar_update" && frame.at("candle").at("volume") == 1;
        }
        EXPECT_EQ(closes, 1);
        EXPECT_EQ(forming, 2);
        // 1m also sends an empty carry before its first trade. The unchanged
        // 5m closed-minute intermediate is pending and superseded by close.
        EXPECT_EQ(frames.size(), tf == 60000 ? 4u : 3u);
    }
}

TEST_F(CandleSelectionWire, SwitchSeedsQuietFormingBarAndDeliversNextCloseExactlyOnce) {
    ActiveCandlePeer peer(fixture.port());
    ASSERT_TRUE(peer.until("ack"));
    ASSERT_TRUE(peer.select("BTC-USD", 60000, 1));
    tradeAt(*fixture.model, 480000, "BTC-USD", 100, 2);
    tradeAt(*fixture.model, 540000, "BTC-USD", 102, 3);
    ASSERT_TRUE(peer.drain(fixture));
    peer.candles.clear();
    ASSERT_TRUE(peer.select("BTC-USD", 300000, 2));
    const auto initial = peer.minutes();
    ASSERT_EQ(initial.size(), 1u); // no subsequent trade is needed to seed
    EXPECT_EQ(initial.front().at("bucket_start_ms"), 300000);
    EXPECT_EQ(initial.front().at("candle").at("volume"), 5);
    EXPECT_EQ(initial.front().at("candle").at("open"), 100);
    EXPECT_EQ(initial.front().at("candle").at("close"), 102);
    tradeAt(*fixture.model, 600000, "BTC-USD", 103, 1);
    ASSERT_TRUE(peer.drain(fixture));
    int closes = 0, newForming = 0;
    int64_t seq = 0;
    for (const auto& frame : peer.minutes()) {
        EXPECT_EQ(frame.at("timeframe_ms"), 300000);
        EXPECT_EQ(frame.at("selection"), 2);
        EXPECT_EQ(frame.at("seq"), ++seq);
        closes += frame.at("type") == "candle_bar_closed" && frame.at("bucket_start_ms") == 300000;
        newForming += frame.at("type") == "candle_bar_update" && frame.at("bucket_start_ms") == 600000;
    }
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(newForming, 1);
}

TEST_F(CandleSelectionWire, ReconnectResubscribeRestoresSelectionAndQuietSnapshot) {
    {
        ActiveCandlePeer first(fixture.port());
        ASSERT_TRUE(first.until("ack"));
        ASSERT_TRUE(first.select("BTC-USD", 300000, 1));
        tradeAt(*fixture.model, 540000, "BTC-USD", 100, 2);
        ASSERT_TRUE(first.drain(fixture));
    }
    ActiveCandlePeer again(fixture.port());
    ASSERT_TRUE(again.until("ack"));
    ASSERT_TRUE(again.select("BTC-USD", 300000, 2));
    ASSERT_EQ(again.minutes().size(), 1u);
    EXPECT_EQ(again.minutes().front().at("candle").at("volume"), 2);
    tradeAt(*fixture.model, 600000);
    ASSERT_TRUE(again.drain(fixture));
    int closes = 0;
    for (const auto& frame : again.minutes()) {
        EXPECT_EQ(frame.at("timeframe_ms"), 300000);
        EXPECT_EQ(frame.at("selection"), 2);
        closes += frame.at("type") == "candle_bar_closed";
    }
    EXPECT_EQ(closes, 1);
}

TEST_F(CandleSelectionWire, LiveUpdateBeforeSnapshotIsNotDuplicatedAndSymbolsReplaceSelection) {
    ActiveCandlePeer peer(fixture.port());
    ASSERT_TRUE(peer.until("ack"));
    auto sessions = RecordingServerStopTest::sessions(*fixture.server);
    ASSERT_EQ(sessions.size(), 1u);
    ASSERT_TRUE(RecordingServerStopTest::message(*sessions.front(),
        {{"type", "candle_live_select"}, {"symbol", "BTC-USD"}, {"timeframe_ms", 300000}, {"selection", 1}}));
    // The snapshot is queued to this model thread but has not run. Publish a
    // live update first, then let the snapshot observe that same forming bar.
    tradeAt(*fixture.model, 540000);
    ASSERT_TRUE(peer.until("candle_live_selected"));
    ASSERT_EQ(peer.minutes().size(), 1u);
    peer.write({{"type", "subscribe"}, {"symbol", "ETH-USD"}});
    ASSERT_TRUE(peer.until("ack"));
    tradeAt(*fixture.model, 540000, "ETH-USD", 200, 2);
    ASSERT_TRUE(peer.drain(fixture));
    peer.candles.clear();
    ASSERT_TRUE(peer.select("ETH-USD", 300000, 2));
    ASSERT_EQ(peer.minutes().size(), 1u);
    EXPECT_EQ(peer.minutes().front().at("candle").at("close"), 200);
    // An old history request never switches the live selection back to BTC.
    peer.write({{"type", "candle_history_request"}, {"symbol", "BTC-USD"},
        {"timeframe_ms", 60000}, {"end_time_ms", 172860000}, {"limit", 1}});
    tradeAt(*fixture.model, 600000, "BTC-USD");
    tradeAt(*fixture.model, 600000, "ETH-USD", 201);
    ASSERT_TRUE(peer.drain(fixture));
    int closes = 0;
    for (const auto& frame : peer.minutes()) {
        EXPECT_EQ(frame.at("symbol"), "ETH-USD");
        EXPECT_EQ(frame.at("selection"), 2);
        closes += frame.at("type") == "candle_bar_closed";
    }
    EXPECT_EQ(closes, 1);
}

sentinel::roller::CandleRestore restoreOf(std::map<int64_t, std::vector<int64_t>> closed) {
    sentinel::roller::CandleRestore r;
    r.product = "BTC-USD";
    r.outcome = sentinel::roller::RestoreOutcome::complete;
    for (const auto &[tf, starts] : closed) {
        auto &series = r.series[tf];
        for (const int64_t start : starts) {
            OHLCVBar bar;
            bar.timestamp_ms = start;
            bar.open = bar.high = bar.low = bar.close = 7;
            bar.volume = 1;
            bar.count = 1;
            bar.is_closed = true;
            series.closed.push_back(bar);
        }
        series.active.timestamp_ms = starts.empty() ? 0 : starts.back() + tf;
        series.active.open = series.active.high = series.active.low = series.active.close = 7;
        series.active.volume = 1;
        series.active.count = series.active.timestamp_ms ? 1 : 0;
    }
    return r;
}

TEST(CandleWireWarming, WarmingReplyEchoesTheWindowUntilTheRebuildEnds) {
    int argc = 1;
    char name[] = "candle-warming";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    CandleFixture fixture;
    fixture.model->setCandleHistoryWarming({"BTC-USD"});
    EXPECT_FALSE(fixture.model->candleHistoryReady("BTC-USD"));
    EXPECT_TRUE(fixture.model->candleHistoryReady("ETH-USD")); // not warming
    fixture.start();
    RawCandleClient client(fixture.port());
    auto sessions = RecordingServerStopTest::sessions(*fixture.server);
    ASSERT_EQ(sessions.size(), 1);
    ASSERT_TRUE(spinUntil([&] { return fixture.subscribed(sessions.front()); }));
    for (const int64_t tf : {int64_t{100}, int64_t{1000}}) {
        const auto reply = client.request(tf, 103000);
        EXPECT_EQ(reply.value("type", ""), "candle_history_chunk") << tf;
        EXPECT_EQ(reply.value("status", ""), "warming") << tf;
        EXPECT_EQ(reply.value("schema_version", 0), protocol::SentinelProtocol::kCandleSchemaVersion);
        EXPECT_EQ(reply.value("symbol", ""), "BTC-USD");
        EXPECT_EQ(reply.value("timeframe_ms", int64_t{0}), tf);
        EXPECT_EQ(reply.value("start_time_ms", int64_t{0}), std::max<int64_t>(0, 103000 - tf * 350));
        EXPECT_EQ(reply.value("end_time_ms", int64_t{0}), 103000);
        ASSERT_TRUE(reply.contains("candles"));
        EXPECT_TRUE(reply["candles"].is_array() && reply["candles"].empty()) << tf;
    }
    // 1 m pages from REST and never warms (here the REST stub refuses).
    const auto minute = client.request(60000, 172860000);
    EXPECT_FALSE(minute.contains("status"));
    // The rebuild ended: normal replies, without a status, with exactly the
    // newest restored closed bars at or before the requested end.
    std::vector<int64_t> hundreds, seconds;
    for (int64_t t = 60000; t < 103000; t += 100) hundreds.push_back(t);
    for (int64_t t = 60000; t < 103000; t += 1000) seconds.push_back(t);
    fixture.model->onCandleHistoryRestored("BTC-USD", restoreOf({{100, hundreds}, {1000, seconds}}));
    EXPECT_TRUE(fixture.model->candleHistoryReady("BTC-USD"));
    for (const auto &[tf, starts] : {std::pair{int64_t{100}, hundreds}, std::pair{int64_t{1000}, seconds}}) {
        const auto reply = client.request(tf, 100000);
        EXPECT_EQ(reply.value("type", ""), "candle_history_chunk") << tf;
        EXPECT_FALSE(reply.contains("status")) << tf;
        std::vector<int64_t> expected;
        for (const int64_t t : starts)
            if (t <= 100000) expected.push_back(t);
        if (expected.size() > 350) expected.erase(expected.begin(), expected.end() - 350);
        const auto &candles = reply["candles"];
        ASSERT_EQ(candles.size(), expected.size()) << tf;
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(candles[i].value("time_start_ms", int64_t{0}), expected[i]) << tf << " at " << i;
            EXPECT_EQ(candles[i].value("time_end_ms", int64_t{0}), expected[i] + tf) << tf << " at " << i;
            EXPECT_DOUBLE_EQ(candles[i].value("close", 0.0), 7) << tf;
            EXPECT_DOUBLE_EQ(candles[i].value("volume", 0.0), 1) << tf;
            EXPECT_TRUE(candles[i].value("is_closed", false)) << tf;
        }
    }
}

// The install goes only into empty series, both timeframes or neither, never
// emits a bar, and ends warming whatever the outcome.
TEST(CandleWireRestore, InstallsIntoEmptySeriesOnlyAndAlwaysEndsWarming) {
    int argc = 1;
    char name[] = "candle-restore";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    CandleFixture fixture;
    auto &model = *fixture.model;
    int emitted = 0;
    QObject::connect(&model, &ServerDataModel::barClosed, [&](auto &&...) { ++emitted; });
    QObject::connect(&model, &ServerDataModel::barUpdated, [&](auto &&...) { ++emitted; });
    EXPECT_TRUE(model.candleHistoryReady("BTC-USD")); // a bare model never warms
    model.setCandleHistoryWarming({"BTC-USD", "ETH-USD"});
    EXPECT_FALSE(model.candleHistoryReady("BTC-USD"));
    EXPECT_FALSE(model.candleHistoryReady("ETH-USD"));
    model.onCandleHistoryRestored("BTC-USD", restoreOf({{100, {500, 600}}, {1000, {0}}}));
    EXPECT_TRUE(model.candleHistoryReady("BTC-USD"));
    EXPECT_EQ(emitted, 0);
    const auto hundred = model.getHistory("BTC-USD", 100, 100);
    ASSERT_EQ(hundred.size(), 2u);
    EXPECT_EQ(hundred.front().timestamp_ms, 500);
    EXPECT_EQ(model.getHistory("BTC-USD", 1000, 100).size(), 1u);
    // ETH: its 1 s series is already populated, so neither timeframe installs.
    auto first = restoreOf({{1000, {0}}});
    first.product = "ETH-USD";
    model.onCandleHistoryRestored("ETH-USD", std::move(first));
    EXPECT_TRUE(model.candleHistoryReady("ETH-USD"));
    auto second = restoreOf({{100, {500}}, {1000, {1000}}});
    second.product = "ETH-USD";
    model.onCandleHistoryRestored("ETH-USD", std::move(second));
    EXPECT_TRUE(model.getHistory("ETH-USD", 100, 100).empty());
    const auto seconds = model.getHistory("ETH-USD", 1000, 100);
    ASSERT_EQ(seconds.size(), 1u);
    EXPECT_EQ(seconds.front().timestamp_ms, 0);
    // A product that never warmed: logged, nothing installed.
    model.onCandleHistoryRestored("SOL-USD", restoreOf({{100, {500}}}));
    EXPECT_TRUE(model.getHistory("SOL-USD", 100, 100).empty());
    EXPECT_EQ(emitted, 0);
    // The restored open bar continues through the unchanged live path.
    tradeAt(model, 800);
    const auto after = model.getHistory("BTC-USD", 100, 100);
    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(after.back().timestamp_ms, 700);
    EXPECT_EQ(after.back().count, 1u);
}
} // namespace
