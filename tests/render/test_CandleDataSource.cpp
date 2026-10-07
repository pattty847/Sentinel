#include <gtest/gtest.h>
#include "datasources/RemoteGridDataSource.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
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
        client().handleCandleHistoryChunkMessage({
            {"type", "candle_history_chunk"}, {"schema_version", protocol::SentinelProtocol::kCandleSchemaVersion},
            {"symbol", request.at("symbol")}, {"timeframe_sec", request.at("timeframe_sec")},
            {"start_time_sec", request.at("end_time_sec").get<qint64>() -
                request.at("limit").get<int>() * request.at("timeframe_sec").get<qint64>()},
            {"end_time_sec", request.at("end_time_sec")}, {"candles", candles}});
        deliver();
    }
    void live(const char* symbol, qint64 tf, qint64 startMinute, double close, qint64 seq, bool closed) {
        client().handleCandleBarMessage(closed ? protocol::MessageType::CandleBarClosed
                                             : protocol::MessageType::CandleBarUpdate,
            {{"schema_version", protocol::SentinelProtocol::kCandleSchemaVersion},
             {"symbol", symbol}, {"timeframe_sec", tf}, {"bucket_start_ms", startMinute * minute},
             {"seq", seq}, {"candle", candle(startMinute, close, closed)}});
    }
    CandleSeriesBuffer::CandleBar cached(qint64 startMinute) {
        auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        EXPECT_TRUE(buffer->getVisibleSlice("BTC-USD", 60, startMinute * minute,
                                           startMinute * minute + 1, bars));
        return bars.empty() ? CandleSeriesBuffer::CandleBar{} : bars.front();
    }
    void warm(qint64 start = 1000, qint64 end = 1010) {
        source.setCandleHistoryViewport("BTC-USD", 60, start * minute, end * minute);
        qint64 boundary = end;
        const qint64 target = start - (end - start);
        while (boundary > target) {
            const auto request = takeRequest();
            ASSERT_FALSE(request.is_null());
            ASSERT_EQ(request.at("type"), "candle_history_request");
            ASSERT_EQ(request.at("end_time_sec"), boundary * 60);
            boundary -= request.at("limit").get<int>();
            reply(request, Json::array({candle(boundary, 1), candle(request.at("end_time_sec").get<qint64>() / 60 - 1, 1)}));
        }
    }
    void reconnect(qint64 start = 1000, qint64 end = 1010) {
        client().disconnected();
        deliver();
        client().connected();
        deliver();
        // This is the configured-history readiness callback used by MainWindowGPU.
        source.setCandleHistoryViewport("BTC-USD", 60, start * minute, end * minute);
    }
};

TEST_F(CandleDataSourceTest, ReconnectRefreshesCoveredCacheAndPreservesNewLiveOwnership) {
    warm();
    live("BTC-USD", 60, 1005, 1, 50, true);
    deliver();
    reconnect();
    const auto refresh = takeRequest();
    ASSERT_FALSE(refresh.is_null()); // oldest=990 already covers viewport+prefetch
    EXPECT_EQ(refresh.at("end_time_sec"), 1010 * 60);
    EXPECT_EQ(refresh.at("limit"), 20);
    EXPECT_TRUE(takeRequest(120).is_null()); // same single-flight guard during refresh
    live("BTC-USD", 60, 1009, 9, 1, false);
    deliver();
    reply(refresh, Json::array({candle(990, 5), candle(1005, 5), candle(1009, 2)}));
    EXPECT_DOUBLE_EQ(cached(1005).close, 5);
    EXPECT_EQ(cached(1005).seq, 0);
    EXPECT_DOUBLE_EQ(cached(1009).close, 9);
    EXPECT_EQ(cached(1009).seq, 1);
    EXPECT_FALSE(cached(1009).isClosed);
    live("BTC-USD", 60, 1009, 10, 2, false);
    deliver();
    EXPECT_DOUBLE_EQ(cached(1009).close, 10);
    EXPECT_TRUE(takeRequest(120).is_null()); // refresh finishes, coverage needs no backfill
}

TEST_F(CandleDataSourceTest, SelectionReentryRefreshRejectsAlreadyQueuedLiveBars) {
    warm();
    live("BTC-USD", 60, 1005, 1, 50, true);
    deliver();
    live("BTC-USD", 60, 1005, 666, 51, false); // queued under old A generation
    source.setCandleHistoryViewport("ETH-USD", 60, 0, 0);
    source.setCandleHistoryViewport("BTC-USD", 60, 1000 * minute, 1010 * minute);
    live("ETH-USD", 60, 1005, 777, 52, false); // current generation, wrong symbol
    live("BTC-USD", 300, 1005, 888, 53, true); // current generation, wrong timeframe
    deliver();
    EXPECT_EQ(cached(1005).seq, 0); // old queued A cannot reclaim ownership after A -> B -> A
    auto* buffer = qobject_cast<CandleSeriesBuffer*>(source.candleBuffer());
    std::vector<CandleSeriesBuffer::CandleBar> wrongSelection;
    EXPECT_FALSE(buffer->getVisibleSlice("ETH-USD", 60, 1005 * minute, 1006 * minute, wrongSelection));
    EXPECT_FALSE(buffer->getVisibleSlice("BTC-USD", 300, 1005 * minute, 1006 * minute, wrongSelection));
    const auto refresh = takeRequest();
    ASSERT_FALSE(refresh.is_null());
    EXPECT_EQ(refresh.at("end_time_sec"), 1010 * 60);
    EXPECT_EQ(refresh.at("limit"), 20);
    reply(refresh, Json::array({candle(990, 5), candle(1005, 5), candle(1009, 5)}));
    EXPECT_DOUBLE_EQ(cached(1005).close, 5);
    live("BTC-USD", 60, 1005, 6, 1, false);
    deliver();
    EXPECT_DOUBLE_EQ(cached(1005).close, 6);
    EXPECT_EQ(cached(1005).seq, 1);
    EXPECT_TRUE(takeRequest(120).is_null());
}

TEST_F(CandleDataSourceTest, TimeframeReentryAlsoRefreshesAndRejectsQueuedClose) {
    warm();
    live("BTC-USD", 60, 1009, 666, 51, true); // old queued close must also be rejected
    source.setCandleHistoryViewport("BTC-USD", 300, 0, 0);
    source.setCandleHistoryViewport("BTC-USD", 60, 1000 * minute, 1010 * minute);
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
        EXPECT_EQ(request.at("end_time_sec"), end * 60);
        EXPECT_EQ(request.at("limit"), limit);
        if (end == 1400)
            source.setCandleHistoryViewport("BTC-USD", 60, 950 * minute, 1400 * minute);
        EXPECT_TRUE(takeRequest(110).is_null());
        reply(request, Json::array({candle(end - limit, 5), candle(end - 1, 5)}));
    }
    EXPECT_DOUBLE_EQ(cached(600).close, 5);
    const auto backfill = takeRequest();
    ASSERT_FALSE(backfill.is_null()); // newer viewport now needs history before cached oldest
    EXPECT_EQ(backfill.at("end_time_sec"), 600 * 60);
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
    source.setCandleHistoryViewport("BTC-USD", 900, 1000 * 900000LL, 2344 * 900000LL);
    const auto request = takeRequest();
    ASSERT_FALSE(request.is_null());
    EXPECT_EQ(request.at("type"), "candle_history_request");
    EXPECT_EQ(request.at("timeframe_sec"), 900);
    EXPECT_EQ(request.at("limit"), 350);
    EXPECT_EQ(request.at("end_time_sec"), 2344 * 900);
}
