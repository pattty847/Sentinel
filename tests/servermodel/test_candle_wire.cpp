#include "CandleWireFixture.hpp"
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
class CandleWire : public testing::Test {
  protected:
    int argc = 1;
    char name[20] = "candle-wire";
    char *argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    CandleFixture fixture;
    std::unique_ptr<SentinelStreamClient> client;
    std::vector<Event> events;
    QVector<SentinelStreamClient::CandleBar> page;
    int64_t pageTf = 0, pageEnd = 0;
    int pages = 0;
    int failures = 0;
    std::shared_ptr<Session> session;
    void SetUp() override {
        fixture.start();
        client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(fixture.port()));
        QObject::connect(client.get(), &SentinelStreamClient::connected, &app,
                         [this] { client->subscribe("BTC-USD"); });
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
            return fixture.subscribed(session);
        }));
    }
    void TearDown() override {
        client->disconnectFromServer();
        client.reset();
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
} // namespace
