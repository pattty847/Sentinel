#include <gtest/gtest.h>
#include "datasources/RemoteGridDataSource.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QStringList>
#include "config/AgentHostMode.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

// Exercise the real serialized request queue and client reply parsers. Only the
// socket is replaced; no test calls CandleSeriesBuffer::applyHistory/applyUpdate.
struct CandleDataSourceTest : testing::Test {
    using Json = nlohmann::json;
    static constexpr qint64 minute = 60'000;
    int argc = 1;
    char name[20] = "candle-data-source";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    RemoteGridDataSource source{"127.0.0.1", "1"};

    bool candleRetryActive() const { return source.m_candleBackfillTimer.isActive(); }
    qint64 candleNow() const { return source.candleNow(); }
    void stepCandleClock(qint64 ms) { source.m_candleClockOffsetMs += ms; }
    qint64 candleRetryDelay() const { return source.m_candleBackfill.retryDelayMs(source.candleLoaded(), source.candleNow()); }
    SentinelStreamClient& client() { return *source.streamClient(); }
    void deliver() { QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall); }
    size_t pendingBookCount() const { return source.m_pendingBookSnapshots.size(); }
    qint64 bookDeadline(const std::string& symbol) const {
        return source.m_pendingBookSnapshots.at(symbol).deadlineMs;
    }
    bool bookRetried(const std::string& symbol) const {
        return source.m_pendingBookSnapshots.at(symbol).retried;
    }
    qint64 nextStaleBookRetry(const std::string& symbol) const {
        return source.m_pendingBookSnapshots.at(symbol).nextStaleRetryMs;
    }
    void advanceBookDeadline(qint64 nowMs) { source.processBookSnapshotDeadlines(nowMs); }
    void bookSnapshot(double bid, double ask) {
        client().handleSnapshotMessage({{"symbol", "BTC-USD"},
            {"tick_size", 0.1},
            {"bids", {{{"p", bid}, {"q", 1.0}}}},
            {"asks", {{{"p", ask}, {"q", 1.0}}}}});
    }
    void emptyBookSnapshot() {
        client().handleSnapshotMessage({{"symbol", "BTC-USD"},
            {"bids", Json::array()}, {"asks", Json::array()}});
    }
    void legacyBookSnapshot() {
        client().handleSnapshotMessage({{"symbol", "BTC-USD"},
            {"bids", {{{"p", 300.0}, {"q", 1.0}}}},
            {"asks", {{{"p", 301.0}, {"q", 1.0}}}}});
    }
    void bookL2(double bid, double size) {
        client().handleL2UpdateMessage({{"product_id", "BTC-USD"},
            {"tick_size", 0.1},
            {"deltas", {{{"side", "bid"}, {"price", bid}, {"size", size}}}}});
    }
    void deliverBookL2(const std::vector<BookLevelUpdate>& updates, quint64 generation) {
        source.onL2UpdateReceived("BTC-USD", updates, 0.1, generation);
    }
    void deliverBookL2At(qint64 nowMs) {
        source.onL2UpdateReceivedAt("BTC-USD", {{true, 300.0, 1.0}}, 0.1,
                                     client().bookDeliveryGeneration("BTC-USD"), nowMs);
    }
    Json takeRequest(int timeoutMs = 1000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            client().m_ioc.restart();
            client().m_ioc.poll();
            if (!client().m_writeQueue.empty()) {
                auto message = std::move(client().m_writeQueue.front());
                client().m_writeQueue.pop_front();
                return Json::parse(message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < until);
        return Json();
    }
    static Json candle(qint64 startMinute, double close, bool closed = true) {
        return {{"time_start_ms", startMinute * minute}, {"time_end_ms", (startMinute + 1) * minute},
                {"open", close}, {"high", close}, {"low", close}, {"close", close},
                {"volume", close}, {"is_closed", closed}};
    }
    void reply(const Json& request, const Json& candles) {
        client().handleCandleHistoryChunkMessage(
            {{"type", "candle_history_chunk"},
             {"schema_version", protocol::SentinelProtocol::kCandleSchemaVersion},
             {"symbol", request.at("symbol")},
             {"timeframe_ms", request.at("timeframe_ms")},
             {"start_time_ms", request.at("end_time_ms").get<qint64>() -
                                   request.at("limit").get<int>() * request.at("timeframe_ms").get<qint64>()},
             {"end_time_ms", request.at("end_time_ms")},
             {"candles", candles}});
        deliver();
    }
    // The server's reply while its startup 100 ms / 1 s rebuild runs.
    void warmingReply(const Json& request) {
        client().handleCandleHistoryChunkMessage(
            {{"type", "candle_history_chunk"},
             {"schema_version", protocol::SentinelProtocol::kCandleSchemaVersion},
             {"symbol", request.at("symbol")},
             {"timeframe_ms", request.at("timeframe_ms")},
             {"start_time_ms", request.at("end_time_ms").get<qint64>() -
                                   request.at("limit").get<int>() * request.at("timeframe_ms").get<qint64>()},
             {"end_time_ms", request.at("end_time_ms")},
             {"status", "warming"},
             {"candles", Json::array()}});
        deliver();
    }
    void live(const char* symbol, qint64 tf, qint64 startMinute, double close, qint64 seq, bool closed) {
        client().handleCandleBarMessage(closed ? protocol::MessageType::CandleBarClosed
                                               : protocol::MessageType::CandleBarUpdate,
                                        {{"schema_version", protocol::SentinelProtocol::kCandleSchemaVersion},
                                         {"symbol", symbol},
                                         {"timeframe_ms", tf},
                                         {"bucket_start_ms", startMinute * minute},
                                         {"seq", seq},
                                         {"candle", candle(startMinute, close, closed)}});
    }
    CandleSeriesBuffer::CandleBar cached(qint64 startMinute) {
        auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        EXPECT_TRUE(buffer->getVisibleSlice("BTC-USD", 60000, startMinute * minute,
                                           startMinute * minute + 1, bars));
        return bars.empty() ? CandleSeriesBuffer::CandleBar{} : bars.front();
    }
    // The viewport ends at the live edge: the open bucket is `end`.
    void warm(qint64 start = 1000, qint64 end = 1010) {
        source.m_candleClockOffsetMs = end * minute + 30'000 - QDateTime::currentMSecsSinceEpoch();
        source.setCandleHistoryViewport("BTC-USD", 60000, start * minute, end * minute);
        qint64 boundary = end;
        const qint64 target = start - (end - start);
        while (boundary > target) {
            const auto request = takeRequest();
            ASSERT_FALSE(request.is_null());
            ASSERT_EQ(request.at("type"), "candle_history_request");
            ASSERT_EQ(request.at("end_time_ms"), boundary * minute);
            boundary -= request.at("limit").get<int>();
            reply(request,
                  Json::array({candle(boundary, 1), candle(request.at("end_time_ms").get<qint64>() / minute - 1, 1)}));
        }
    }
    void reconnect(qint64 start = 1000, qint64 end = 1010) {
        client().disconnected();
        deliver();
        client().connected();
        deliver();
        // This is the configured-history readiness callback used by MainWindowGPU.
        source.setCandleHistoryViewport("BTC-USD", 60000, start * minute, end * minute);
    }
};

TEST_F(CandleDataSourceTest, ReconnectRefreshesCoveredCacheAndPreservesNewLiveOwnership) {
    warm();
    live("BTC-USD", 60000, 1005, 1, 50, true);
    deliver();
    reconnect();
    const auto refresh = takeRequest();
    ASSERT_FALSE(refresh.is_null()); // oldest=990 already covers viewport+prefetch
    EXPECT_EQ(refresh.at("end_time_ms"), 1010 * minute);
    EXPECT_EQ(refresh.at("limit"), 20);
    EXPECT_TRUE(takeRequest(120).is_null()); // same single-flight guard during refresh
    live("BTC-USD", 60000, 1009, 9, 1, false);
    deliver();
    reply(refresh, Json::array({candle(990, 5), candle(1005, 5), candle(1009, 2)}));
    EXPECT_DOUBLE_EQ(cached(1005).close, 5);
    EXPECT_EQ(cached(1005).seq, 0);
    EXPECT_DOUBLE_EQ(cached(1009).close, 9);
    EXPECT_EQ(cached(1009).seq, 1);
    EXPECT_FALSE(cached(1009).isClosed);
    live("BTC-USD", 60000, 1009, 10, 2, false);
    deliver();
    EXPECT_DOUBLE_EQ(cached(1009).close, 10);
    EXPECT_TRUE(takeRequest(120).is_null()); // refresh finishes, coverage needs no backfill
}

TEST_F(CandleDataSourceTest, SelectionReentryRefreshRejectsAlreadyQueuedLiveBars) {
    warm();
    live("BTC-USD", 60000, 1005, 1, 50, true);
    deliver();
    live("BTC-USD", 60000, 1005, 666, 51, false); // queued under old A generation
    source.setCandleHistoryViewport("ETH-USD", 60000, 0, 0);
    source.setCandleHistoryViewport("BTC-USD", 60000, 1000 * minute, 1010 * minute);
    live("ETH-USD", 60000, 1005, 777, 52, false); // current generation, wrong symbol
    live("BTC-USD", 300000, 1005, 888, 53, true); // current generation, wrong timeframe
    deliver();
    EXPECT_EQ(cached(1005).seq, 0); // old queued A cannot reclaim ownership after A -> B -> A
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    std::vector<CandleSeriesBuffer::CandleBar> wrongSelection;
    EXPECT_FALSE(buffer->getVisibleSlice("ETH-USD", 60000, 1005 * minute, 1006 * minute, wrongSelection));
    EXPECT_FALSE(buffer->getVisibleSlice("BTC-USD", 300000, 1005 * minute, 1006 * minute, wrongSelection));
    const auto refresh = takeRequest();
    ASSERT_FALSE(refresh.is_null());
    EXPECT_EQ(refresh.at("end_time_ms"), 1010 * minute);
    EXPECT_EQ(refresh.at("limit"), 20);
    reply(refresh, Json::array({candle(990, 5), candle(1005, 5), candle(1009, 5)}));
    EXPECT_DOUBLE_EQ(cached(1005).close, 5);
    live("BTC-USD", 60000, 1005, 6, 1, false);
    deliver();
    EXPECT_DOUBLE_EQ(cached(1005).close, 6);
    EXPECT_EQ(cached(1005).seq, 1);
    EXPECT_TRUE(takeRequest(120).is_null());
}

TEST_F(CandleDataSourceTest, TimeframeReentryAlsoRefreshesAndRejectsQueuedClose) {
    warm();
    live("BTC-USD", 60000, 1009, 666, 51, true); // old queued close must also be rejected
    source.setCandleHistoryViewport("BTC-USD", 300000, 0, 0);
    source.setCandleHistoryViewport("BTC-USD", 60000, 1000 * minute, 1010 * minute);
    deliver();
    EXPECT_EQ(cached(1009).seq, 0);
    const auto refresh = takeRequest();
    ASSERT_FALSE(refresh.is_null());
    reply(refresh, Json::array({candle(990, 5), candle(1009, 5)}));
    EXPECT_DOUBLE_EQ(cached(1009).close, 5);
    EXPECT_TRUE(takeRequest(120).is_null());
}

TEST_F(CandleDataSourceTest, WideRefreshPagesNewestFirstThenResumesOlderBackfill) {
    warm(1000, 1400); // cached oldest=600, covers the whole original prefetch range
    reconnect(1000, 1400);
    for (const auto [end, limit] : {std::pair{1400, 350}, {1050, 350}, {700, 100}}) {
        const auto request = takeRequest();
        ASSERT_FALSE(request.is_null());
        EXPECT_EQ(request.at("end_time_ms"), end * minute);
        EXPECT_EQ(request.at("limit"), limit);
        if (end == 1400)
            source.setCandleHistoryViewport("BTC-USD", 60000, 950 * minute, 1400 * minute);
        EXPECT_TRUE(takeRequest(110).is_null());
        reply(request, Json::array({candle(end - limit, 5), candle(end - 1, 5)}));
    }
    EXPECT_DOUBLE_EQ(cached(600).close, 5);
    const auto backfill = takeRequest();
    ASSERT_FALSE(backfill.is_null()); // newer viewport now needs history before cached oldest
    EXPECT_EQ(backfill.at("end_time_ms"), 600 * minute);
    EXPECT_EQ(backfill.at("limit"), 100);
    reply(backfill, Json::array({candle(500, 5)}));
    EXPECT_TRUE(takeRequest(120).is_null());
}

// Review item 8 (pre-existing): on a re-subscription the replica kept from the earlier
// subscription had stopped updating; the first L2 update landed on its stale levels and
// the chart seeded from a top ~$600 off before the fresh snapshot. The replica is
// cleared on subscribe: nothing derives a top from it until the new snapshot.
TEST_F(CandleDataSourceTest, ResubscribeDropsTheRetainedBookUntilTheFreshSnapshot) {
    std::vector<std::pair<uint32_t, double>> bids, asks;
    auto top = [&](double& bid, double& ask) {
        const auto view = source.getDirectLiveOrderBook("BTC-USD").captureDenseNonZero(bids, asks, 1);
        bid = view.bidLevels.empty() ? 0.0 : view.minPrice + view.bidLevels.front().first * view.tickSize;
        ask = view.askLevels.empty() ? 0.0 : view.minPrice + view.askLevels.front().first * view.tickSize;
    };
    source.subscribe("BTC-USD");
    emit client().snapshotReceived("BTC-USD", {{86'597.0, 1.0}}, {{86'598.0, 1.0}},
                                         0.1, client().bookDeliveryGeneration("BTC-USD"));
    deliver();
    double bid = 0, ask = 0;
    top(bid, ask);
    ASSERT_NEAR(bid, 86'597.0, 0.11); // the $0.10 book grid
    source.subscribe("BTC-USD"); // e.g. back from ETH-USD
    emit client().l2UpdateReceived("BTC-USD", {{true, 86'010.0, 2.0}},
                                         0.1, client().bookDeliveryGeneration("BTC-USD"));
    deliver();
    top(bid, ask);
    EXPECT_EQ(bid, 0.0) << "no top from the retained book before the fresh snapshot";
    EXPECT_EQ(ask, 0.0);
    EXPECT_TRUE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    emit client().snapshotReceived("BTC-USD", {{86'009.0, 1.0}}, {{86'011.0, 1.0}},
                                         0.1, client().bookDeliveryGeneration("BTC-USD"));
    deliver();
    top(bid, ask);
    EXPECT_NEAR(bid, 86'009.0, 0.11); // the $0.10 book grid
    EXPECT_NEAR(ask, 86'011.0, 0.11); // the $0.10 book grid
}

TEST_F(CandleDataSourceTest, QueuedOldBookMessagesCannotRefillResubscribedReplica) {
    source.subscribe("BTC-USD");
    const auto oldGeneration = client().bookDeliveryGeneration("BTC-USD");
    bookSnapshot(100.0, 101.0);
    bookL2(100.0, 9.0);
    EXPECT_EQ(oldGeneration, client().bookDeliveryGeneration("BTC-USD"));
    source.subscribe("BTC-USD");
    deliver();
    EXPECT_TRUE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    EXPECT_EQ(pendingBookCount(), 1u);
    emit client().snapshotReceived("BTC-USD", {{200.0, 1.0}}, {{201.0, 1.0}},
                                         0.1, client().bookDeliveryGeneration("BTC-USD"));
    deliver();
    EXPECT_FALSE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    EXPECT_EQ(pendingBookCount(), 0u);
    std::vector<std::pair<uint32_t, double>> bids, asks;
    source.getDirectLiveOrderBook("BTC-USD").captureDenseNonZero(bids, asks, 1);
    const auto freshBids = bids;
    deliverBookL2({{true, 200.0, 9.0}}, oldGeneration);
    source.getDirectLiveOrderBook("BTC-USD").captureDenseNonZero(bids, asks, 1);
    EXPECT_EQ(bids, freshBids);
}

TEST_F(CandleDataSourceTest, MissingBookSnapshotRetriesOnceThenReportsStale) {
    source.subscribe("BTC-USD");
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    emptyBookSnapshot();
    deliver();
    EXPECT_EQ(pendingBookCount(), 1u);
    int staleErrors = 0;
    QObject::connect(&source, &IGridDataSource::errorOccurred, &source,
        [&](const QString& message) { if (message.contains("snapshot stale")) ++staleErrors; });
    const auto firstDeadline = bookDeadline("BTC-USD");
    advanceBookDeadline(firstDeadline);
    EXPECT_EQ(takeRequest().at("type"), "subscribe");
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    advanceBookDeadline(firstDeadline + 5000);
    EXPECT_TRUE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(staleErrors, 1);
    advanceBookDeadline(firstDeadline + 10000);
    EXPECT_EQ(staleErrors, 1);
    EXPECT_TRUE(takeRequest(30).is_null());
    bookSnapshot(300.0, 301.0);
    deliver();
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(pendingBookCount(), 0u);
}

TEST_F(CandleDataSourceTest, RepeatedOldServerUnavailablePreservesRetryAndStaleBackoff) {
    source.subscribe("BTC-USD");
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    const auto firstDeadline = bookDeadline("BTC-USD");
    for (int i = 0; i < 3; ++i) {
        legacyBookSnapshot(); // Old server has levels but no tick_size/book_version.
        deliver();
        EXPECT_EQ(bookDeadline("BTC-USD"), firstDeadline);
        EXPECT_DOUBLE_EQ(source.getDirectLiveOrderBook("BTC-USD").getTickSize(), 0.0);
    }
    advanceBookDeadline(firstDeadline);
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    const auto retryDeadline = bookDeadline("BTC-USD");
    for (int i = 0; i < 3; ++i) {
        legacyBookSnapshot();
        deliver();
        EXPECT_EQ(bookDeadline("BTC-USD"), retryDeadline);
        EXPECT_TRUE(bookRetried("BTC-USD"));
    }
    advanceBookDeadline(retryDeadline);
    ASSERT_TRUE(source.isBookSnapshotStale("BTC-USD"));
    const auto staleRetry = nextStaleBookRetry("BTC-USD");
    legacyBookSnapshot();
    deliver();
    EXPECT_TRUE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(nextStaleBookRetry("BTC-USD"), staleRetry);
    deliverBookL2At(staleRetry);
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    const auto backedOffUntil = nextStaleBookRetry("BTC-USD");
    legacyBookSnapshot();
    deliver();
    EXPECT_EQ(nextStaleBookRetry("BTC-USD"), backedOffUntil);
    bookSnapshot(300.0, 301.0);
    deliver();
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(pendingBookCount(), 0u);
    EXPECT_DOUBLE_EQ(source.getDirectLiveOrderBook("BTC-USD").getTickSize(), 0.1);
}

TEST_F(CandleDataSourceTest, DisconnectClearsStaleBookStatusBeforeFreshSnapshot) {
    source.subscribe("BTC-USD");
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    std::vector<bool> staleChanges;
    QObject::connect(&source, &IGridDataSource::bookSnapshotStaleChanged, &source,
        [&](const QString& symbol, bool stale) {
            if (symbol == "BTC-USD") staleChanges.push_back(stale);
        });
    const auto deadline = bookDeadline("BTC-USD");
    advanceBookDeadline(deadline);
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    advanceBookDeadline(deadline + 5000);
    ASSERT_TRUE(source.isBookSnapshotStale("BTC-USD"));
    ASSERT_EQ(staleChanges, (std::vector<bool>{true}));

    client().disconnected();
    deliver();
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(pendingBookCount(), 0u);
    EXPECT_EQ(staleChanges, (std::vector<bool>{true, false}));

    client().connected();
    deliver();
    source.subscribe("BTC-USD");
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    bookSnapshot(300.0, 301.0);
    deliver();
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(pendingBookCount(), 0u);
    EXPECT_EQ(staleChanges, (std::vector<bool>{true, false}));
}

TEST_F(CandleDataSourceTest, StaleL2RequestsFreshSnapshotsWithBoundedBackoff) {
    source.subscribe("BTC-USD");
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    int staleOn = 0, staleOff = 0;
    QObject::connect(&source, &IGridDataSource::bookSnapshotStaleChanged, &source,
        [&](const QString& symbol, bool stale) {
            if (symbol == "BTC-USD") (stale ? staleOn : staleOff)++;
        });
    const auto deadline = bookDeadline("BTC-USD");
    advanceBookDeadline(deadline);
    ASSERT_EQ(takeRequest().at("type"), "subscribe");
    advanceBookDeadline(deadline + 5000);
    ASSERT_TRUE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(staleOn, 1);
    const auto staleAt = deadline + 5000;
    deliverBookL2At(staleAt + 4999);
    EXPECT_TRUE(takeRequest(20).is_null());
    const auto firstGeneration = client().bookDeliveryGeneration("BTC-USD");
    deliverBookL2At(staleAt + 5000);
    EXPECT_EQ(takeRequest().at("type"), "subscribe");
    EXPECT_GT(client().bookDeliveryGeneration("BTC-USD"), firstGeneration);
    deliverBookL2At(staleAt + 5000 + 14999);
    EXPECT_TRUE(takeRequest(20).is_null());
    deliverBookL2At(staleAt + 5000 + 15000);
    EXPECT_EQ(takeRequest().at("type"), "subscribe");
    deliverBookL2At(staleAt + 5000 + 15000 + 59999);
    EXPECT_TRUE(takeRequest(20).is_null());
    deliverBookL2At(staleAt + 5000 + 15000 + 60000);
    EXPECT_EQ(takeRequest().at("type"), "subscribe");
    EXPECT_EQ(staleOn, 1);
    bookSnapshot(300.0, 301.0);
    deliver();
    EXPECT_FALSE(source.isBookSnapshotStale("BTC-USD"));
    EXPECT_EQ(staleOff, 1);
    EXPECT_EQ(pendingBookCount(), 0u);
}

TEST_F(CandleDataSourceTest, FifteenMinuteViewportSendsNativeBarPageThroughRealClientQueue) {
    source.setCandleHistoryViewport("BTC-USD", 900000, 1000 * 900000LL, 2344 * 900000LL);
    const auto request = takeRequest();
    ASSERT_FALSE(request.is_null());
    EXPECT_EQ(request.at("type"), "candle_history_request");
    EXPECT_EQ(request.at("timeframe_ms"), 900000);
    EXPECT_EQ(request.at("limit"), 350);
    EXPECT_EQ(request.at("end_time_ms"), 2344 * 900000);
}

TEST_F(CandleDataSourceTest, DisconnectClearsReplicaBeforeGuardedStartupResubscribe) {
    QTemporaryDir host("/private/tmp/sentinel-reconnect-host-XXXXXX");
    const auto resetHost = qScopeGuard([] { AgentHostMode::resetForTests(); });
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(host.path(), {}, &error)) << error.toStdString();
    AgentHostMode::setSymbolAllowlist({"BTC-USD"});
    std::vector<bool> statuses;
    // Keep the startup subscriber connected for every new transport session.
    QObject::connect(&source, &IGridDataSource::connectionStatusChanged, &source, [&](bool up) {
        statuses.push_back(up);
        if (up) {
            source.subscribe("BTC-USD");
            source.subscribe("ETH-USD"); // refused at the same boundary on reconnect
        }
    });
    client().connected();
    deliver();
    ASSERT_EQ(takeRequest().at("symbol"), "BTC-USD");
    EXPECT_TRUE(takeRequest(20).is_null());
    bookSnapshot(300.0, 301.0);
    deliver();
    ASSERT_FALSE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    client().disconnected();
    deliver();
    EXPECT_TRUE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    EXPECT_EQ(source.marketHealth()->snapshot().transport, MarketHealth::Transport::Reconnecting);
    client().disconnected(); // failed retry stays down without another GUI status notification
    deliver();
    EXPECT_EQ(statuses, (std::vector<bool>{true, false}));
    client().connected();
    deliver();
    ASSERT_EQ(takeRequest().at("symbol"), "BTC-USD");
    EXPECT_TRUE(takeRequest(20).is_null());
    EXPECT_TRUE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
    EXPECT_EQ(source.marketHealth()->snapshot().transport, MarketHealth::Transport::Connected);
    bookSnapshot(400.0, 401.0);
    deliver();
    EXPECT_FALSE(source.getDirectLiveOrderBook("BTC-USD").isEmpty());
}

TEST_F(CandleDataSourceTest, HundredMillisecondsRequestsAndAppliesMilliseconds) {
    source.setCandleHistoryViewport("BTC-USD", 100, 60000, 61000);
    const auto request = takeRequest();
    ASSERT_FALSE(request.is_null());
    EXPECT_EQ(request.at("timeframe_ms"), 100);
    EXPECT_EQ(request.at("end_time_ms"), 60900);
    EXPECT_EQ(request.at("limit"), 20);
    reply(request, Json::array({{{"time_start_ms", 60000},
                                 {"time_end_ms", 60100},
                                 {"open", 5},
                                 {"high", 5},
                                 {"low", 5},
                                 {"close", 5},
                                 {"volume", 1},
                                 {"is_closed", true}}}));
    auto *buffer = qobject_cast<CandleSeriesBuffer *>(source.candleBuffer());
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    ASSERT_TRUE(buffer->getVisibleSlice("BTC-USD", 100, 60000, 60100, bars));
    ASSERT_EQ(bars.size(), 1);
    EXPECT_EQ(bars.front().timeEndMs, 60100);
    EXPECT_EQ(bars.front().close, 5);
}

namespace {
nlohmann::json second(qint64 startMs, double close) {
    return {{"time_start_ms", startMs}, {"time_end_ms", startMs + 1000}, {"open", close}, {"high", close},
            {"low", close}, {"close", close}, {"volume", 1}, {"is_closed", true}};
}
} // namespace

// Warming on A, then a ready B: B's request goes out at once; back to A, a
// fresh A request (the held one and its 2 s deadline are gone).
TEST_F(CandleDataSourceTest, WarmingSelectionSwitchSendsTheOtherRequestAtOnce) {
    source.setCandleHistoryViewport("BTC-USD", 1000, 600000, 610000);
    const auto a = takeRequest();
    ASSERT_FALSE(a.is_null());
    EXPECT_EQ(a.at("timeframe_ms"), 1000);
    warmingReply(a);
    EXPECT_TRUE(candleRetryActive());
    EXPECT_GT(candleRetryDelay(), 1500);
    const auto sentB = std::chrono::steady_clock::now();
    source.setCandleHistoryViewport("ETH-USD", 1000, 600000, 610000);
    const auto b = takeRequest();
    ASSERT_FALSE(b.is_null());
    EXPECT_EQ(b.at("symbol"), "ETH-USD");
    EXPECT_LT(std::chrono::steady_clock::now() - sentB, std::chrono::milliseconds(500));
    reply(b, nlohmann::json::array({second(605000, 3)}));
    source.setCandleHistoryViewport("BTC-USD", 1000, 600000, 610000);
    const auto again = takeRequest(500);
    ASSERT_FALSE(again.is_null());
    EXPECT_EQ(again.at("symbol"), "BTC-USD");
    EXPECT_EQ(again.at("end_time_ms"), a.at("end_time_ms"));
    EXPECT_EQ(again.at("limit"), a.at("limit"));
}

// Warming on A: a viewport that needs no older data stops the retry and sends
// nothing; scrolling back resends the held request verbatim at its deadline.
// The warming chunk applies no history; the reply to the resend does.
TEST_F(CandleDataSourceTest, WarmingRequestParksOutsideItsNeedAndResendsVerbatim) {
    source.setCandleHistoryViewport("BTC-USD", 1000, 600000, 610000);
    const auto a = takeRequest();
    ASSERT_FALSE(a.is_null());
    warmingReply(a);
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    EXPECT_FALSE(buffer->getVisibleSlice("BTC-USD", 1000, 590000, 610000, bars));
    const qint64 future = QDateTime::currentMSecsSinceEpoch() + 600000;
    source.setCandleHistoryViewport("BTC-USD", 1000, future, future + 10000);
    EXPECT_FALSE(candleRetryActive());
    EXPECT_TRUE(takeRequest(150).is_null());
    const auto held = std::chrono::steady_clock::now();
    source.setCandleHistoryViewport("BTC-USD", 1000, 600000, 610000);
    EXPECT_TRUE(candleRetryActive());
    const auto resent = takeRequest(3000);
    ASSERT_FALSE(resent.is_null());
    EXPECT_GE(std::chrono::steady_clock::now() - held, std::chrono::milliseconds(1500));
    EXPECT_EQ(resent, a);
    reply(resent, nlohmann::json::array({second(595000, 4), second(600000, 5)}));
    ASSERT_TRUE(buffer->getVisibleSlice("BTC-USD", 1000, 595000, 601000, bars));
    ASSERT_EQ(bars.size(), 2u);
    EXPECT_DOUBLE_EQ(bars.back().close, 5);
}

// Back to an interval at an older viewport: the real data source requests the
// window, the span up to the bars it already holds and the span since it left.
TEST_F(CandleDataSourceTest, ReturnToOlderViewportRequestsEveryUnfetchedSpan) {
    const auto fullReply = [this](const Json& request) {
        const qint64 end = request.at("end_time_ms").get<qint64>() / minute;
        const qint64 start = end - request.at("limit").get<int>();
        const qint64 liveEdge = candleNow() / minute;
        Json bars = Json::array();
        for (qint64 m = start; m < std::min(end, liveEdge); ++m) bars.push_back(candle(m, 5));
        reply(request, bars);
        return std::pair{start, end};
    };
    warm(2990, 3000);
    live("BTC-USD", 60000, 3000, 5, 1, false);
    deliver();
    EXPECT_TRUE(takeRequest(150).is_null());
    source.setCandleHistoryViewport("BTC-USD", 300000, 0, 0);
    stepCandleClock(20 * minute); // away for 20 minutes
    source.setCandleHistoryViewport("BTC-USD", 60000, 2900 * minute, 2910 * minute);
    std::vector<std::pair<qint64, qint64>> requested;
    for (int page = 0; page < 10; ++page) {
        const auto request = takeRequest(300);
        if (request.is_null()) break;
        requested.push_back(fullReply(request));
    }
    const auto requestedSpan = [&](qint64 from, qint64 to) {
        return std::any_of(requested.begin(), requested.end(),
                           [&](const auto& r) { return r.first <= from && r.second >= to; });
    };
    EXPECT_TRUE(requestedSpan(2890, 2910)) << "the window";
    EXPECT_TRUE(requestedSpan(2910, 2980)) << "up to the bars loaded before";
    EXPECT_TRUE(requestedSpan(3001, 3020)) << "the time away, up to the live edge";
    const auto* coverage = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer())->coverage("BTC-USD", 60000);
    ASSERT_NE(coverage, nullptr);
    EXPECT_TRUE(coverage->coversRange(2890 * minute, 3020 * minute));
    EXPECT_TRUE(takeRequest(150).is_null());
}
