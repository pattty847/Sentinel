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
