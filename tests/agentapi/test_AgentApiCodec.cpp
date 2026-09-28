#include "AgentApiCodec.hpp"
#include "AgentApiSnapshots.hpp"
#include "AgentApiOperations.hpp"
#include "CandleSeriesBuffer.hpp"
#include "marketdata/model/TradeData.h"
#include "config/ConfigTypes.hpp"
#include <gtest/gtest.h>
#include <QJsonDocument>
#include <QJsonArray>

using namespace AgentApi;

TEST(AgentApiControls, BodiesAndBounds) {
    auto check = [](const char* path, const char* body) {
        return validateControl({"POST", path, {}, body}, QList<qint64>{60000, 300000});
    };
    EXPECT_EQ(check("/api/v1/symbol", R"({"symbol":"ETH-USD"})").body.symbol, "ETH-USD");
    EXPECT_EQ(check("/api/v1/symbol", R"({"symbol":"../BAD"})").status, 422);
    EXPECT_EQ(check("/api/v1/timeframe", R"({"candleTimeframeMs":60000})").body.timeframeMs, 60000);
    EXPECT_EQ(check("/api/v1/timeframe", R"({"heatmapTimeframeMs":60000,"candleTimeframeMs":300000})").status, 422);
    EXPECT_EQ(check("/api/v1/timeframe", R"({"heatmapTimeframeMs":1000})").code, "timeframe_unavailable");
    EXPECT_EQ(check("/api/v1/viewport", R"({"startMs":1,"endMs":10,"priceMin":1,"priceMax":2})").status, 200);
    EXPECT_EQ(check("/api/v1/viewport", R"({"startMs":1})").status, 422);
    EXPECT_EQ(check("/api/v1/viewport", R"({"startMs":1,"endMs":10,"followLive":true})").status, 422);
    EXPECT_EQ(check("/api/v1/viewport", R"({"priceMin":1,"priceMax":2,"followLive":true})").status, 422);
    EXPECT_EQ(check("/api/v1/viewport", R"({"priceMin":2,"priceMax":1})").status, 422);
    EXPECT_EQ(check("/api/v1/layers", R"({"heatmap":true,"candles":false,"tpo":true})").body.layers.size(), 3);
    EXPECT_EQ(check("/api/v1/layers", R"({"tpo":1})").status, 422);
    EXPECT_EQ(check("/api/v1/layers", R"({"unknown":true})").status, 422);
}

TEST(AgentApiControls, PostParserAndWaitValidation) {
    RequestParser parser;
    const QByteArray body = R"({"symbol":"ETH-USD"})";
    const QByteArray head = "POST /api/v1/symbol HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\n\r\n";
    EXPECT_EQ(parser.feed(head + body.left(5)).kind, ParseResult::Kind::Incomplete);
    const auto parsed = parser.feed(body.mid(5));
    EXPECT_EQ(parsed.kind, ParseResult::Kind::Complete);
    EXPECT_EQ(parsed.request.body, body);
    RequestParser badMethod;
    EXPECT_EQ(badMethod.feed("GET /api/v1/symbol HTTP/1.1\r\nHost: localhost\r\n\r\n").status, 405);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/operations/o1", "waitMs=5000"}).status, 200);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/operations/o1", "waitMs=5001"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "afterOperation=o1&waitMs=5000"}).status, 200);
}

TEST(AgentApiControls, OperationStateWithFakeFrameAck) {
    Operations ops;
    const auto first = ops.apply("viewport", 42);
    EXPECT_EQ(ops.find(first.id)->status, "applied");
    ops.poll(0, 0); // hidden window / no frame: waiting expires with applied status
    EXPECT_EQ(ops.find(first.id)->status, "applied");
    EXPECT_TRUE(shouldDefer(*ops.find(first.id), 99, 100));
    EXPECT_FALSE(shouldDefer(*ops.find(first.id), 100, 100));
    const auto second = ops.apply("viewport", 43);
    EXPECT_EQ(ops.find(first.id)->status, "superseded");
    ops.poll(second.revision, 912);
    EXPECT_EQ(ops.find(second.id)->status, "rendered");
    EXPECT_EQ(ops.find(second.id)->frameId, 912);
    const auto failed = ops.apply("symbol", 43);
    ops.fail(failed.id);
    ops.poll(failed.revision, 913);
    EXPECT_EQ(ops.find(failed.id)->status, "failed");
    EXPECT_FALSE(ops.find("o99999").has_value());
}

namespace {
QByteArray request(const QByteArray& path, const QByteArray& extra = {}) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:17100\r\n" + extra + "\r\n";
}
}

TEST(AgentApiCodec, FragmentedHeaderAndBody) {
    RequestParser p;
    const QByteArray r = request("/api/v1/state", "Content-Length: 0\r\n");
    EXPECT_EQ(p.feed(r.left(10)).kind, ParseResult::Kind::Incomplete);
    EXPECT_EQ(p.feed(r.mid(10, 21)).kind, ParseResult::Kind::Incomplete);
    auto parsed = p.feed(r.mid(31));
    EXPECT_EQ(parsed.kind, ParseResult::Kind::Complete);
    EXPECT_EQ(parsed.request.path, "/api/v1/state");
    RequestParser body;
    const QByteArray withBody = "POST /api/v1/state HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nABCD";
    EXPECT_EQ(body.feed(withBody.left(withBody.size()-2)).kind, ParseResult::Kind::Incomplete);
    parsed = body.feed("CD");
    EXPECT_EQ(parsed.status, 405);
}
TEST(AgentApiCodec, LimitsAndMalformedInput) {
    RequestParser headers;
    EXPECT_EQ(headers.feed(QByteArray(8193, 'a')).status, 431);
    RequestParser body;
    EXPECT_EQ(body.feed(request("/api/v1/state", "Content-Length: 16385\r\n")).status, 413);
    RequestParser extra;
    EXPECT_EQ(extra.feed(request("/api/v1/state") + "x").status, 400);
    RequestParser transfer;
    EXPECT_EQ(transfer.feed(request("/api/v1/state", "Transfer-Encoding: chunked\r\n")).status, 400);
}
TEST(AgentApiCodec, RouteAndOriginHostValidation) {
    RequestParser method;
    EXPECT_EQ(method.feed("POST /api/v1/state HTTP/1.1\r\nHost: localhost\r\n\r\n").status, 405);
    RequestParser route;
    EXPECT_EQ(route.feed(request("/api/v1/missing")).status, 404);
    RequestParser origin;
    EXPECT_EQ(origin.feed(request("/api/v1/state", "Origin: http://localhost\r\n")).status, 403);
    RequestParser host;
    EXPECT_EQ(host.feed("GET /api/v1/state HTTP/1.1\r\nHost: evil.example\r\n\r\n").status, 403);
    RequestParser noHost;
    EXPECT_EQ(noHost.feed("GET /api/v1/state HTTP/1.1\r\n\r\n").status, 403);
    RequestParser legacy;
    EXPECT_EQ(legacy.feed(request("/screenshot?name=a")).kind, ParseResult::Kind::Complete);
}
TEST(AgentApiCodec, EnvelopeAndUnknowns) {
    StateSnapshot s;
    s.meta = {"session", "BTC-USD", 7, 1790596800000, true, "unknown", false};
    s.serverHost = "127.0.0.1";
    s.servedTimeframesMs = QList<qint64>{60000};
    s.candlesLayer = true;
    const auto obj = stateJson(s);
    ASSERT_TRUE(obj.value("ok").toBool());
    const auto meta = obj.value("meta").toObject();
    EXPECT_EQ(meta.value("selectionEpoch").toString(), "7");
    EXPECT_EQ(meta.value("sessionId").toString(), "session");
    EXPECT_EQ(meta.value("source").toString(), "gui-cache");
    EXPECT_TRUE(meta.value("stale").toBool());
    const auto data = obj.value("data").toObject();
    const auto config = data.value("serverConfig").toObject();
    EXPECT_TRUE(config.value("defaultSymbols").isNull());
    const auto heatmap = config.value("heatmap").toObject();
    EXPECT_TRUE(heatmap.value("configuredTimeframesMs").isNull());
    EXPECT_EQ(heatmap.value("servedTimeframesMs").toArray().size(), 1);
    EXPECT_TRUE(data.value("lastReceivedAtMs").toObject().value("trades").isNull());
    EXPECT_TRUE(data.value("layers").toObject().value("heatmap").isNull());
    const auto err = error("invalid_range", "startMs must precede endMs");
    EXPECT_FALSE(err.value("ok").toBool());
    EXPECT_EQ(err.value("error").toObject().value("code").toString(), "invalid_range");
    EXPECT_FALSE(err.contains("data"));
}
TEST(AgentApiCodec, ViewportFixtureAndNullZoom) {
    ViewportSnapshot s;
    s.meta = {"s", "BTC-USD", 3, 1790596800000, false, "partial", false};
    s.startMs = 1790593200000;
    s.endMs = 1790596800000;
    s.priceMin = 64000;
    s.priceMax = 65000;
    s.heatmapTimeframeMs = 60000;
    s.candleTimeframeMs = 60000;
    s.followLive = false;
    s.viewportVersion = 42;
    s.widthPx = 1200;
    s.heightPx = 600;
    auto data = viewportJson(s).value("data").toObject();
    EXPECT_EQ(data.value("viewportVersion").toString(), "42");
    EXPECT_DOUBLE_EQ(data.value("zoom").toObject().value("msPerPx").toDouble(), 3000);
    EXPECT_NEAR(data.value("zoom").toObject().value("pricePerPx").toDouble(), 1.6666667, 1e-6);
    s.widthPx.reset();
    data = viewportJson(s).value("data").toObject();
    EXPECT_TRUE(data.value("widthPx").isNull());
    EXPECT_TRUE(data.value("zoom").toObject().value("msPerPx").isNull());
}

TEST(AgentApiCodec, QueryValidation) {
    EXPECT_EQ(validateQuery({"GET", "/api/v1/state", "symbol=ETH-USD"}, "BTC-USD").status, 409);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/viewport", "unexpected=1"}, "BTC-USD").status, 400);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/state", "symbol=BTC-USD"}, "BTC-USD").status, 200);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/state", "symbol=BTC-USD&symbol=BTC-USD"}, "BTC-USD").status, 400);
    const auto screenshot = validateQuery({"GET", "/api/v1/screenshot", "name=review&target=heatmap"});
    EXPECT_EQ(screenshot.status, 200);
    EXPECT_EQ(screenshot.screenshotName, "review");
    EXPECT_EQ(screenshot.screenshotTarget, "heatmap");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "name=../outside"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "target=unknown"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "afterOperation=o1"}).status, 200);
}

TEST(AgentApiCodec, AdvertisedCapabilitiesOnly) {
    ServerConfig config;
    config.heatmap.servedTimeframesMs = {60000};
    config.heatmap.timeframesMs = {1000, 60000};
    config.heatmap.gridWidth = 4096;
    config.advertisedFields = {"heatmap.servedTimeframesMs", "heatmap.gridWidth"};
    StateSnapshot state;
    applyAdvertisedServerConfig(state, config);
    ASSERT_TRUE(state.servedTimeframesMs.has_value());
    EXPECT_EQ(state.servedTimeframesMs->size(), 1);
    EXPECT_EQ(state.gridWidth, 4096);
    EXPECT_FALSE(state.configuredTimeframesMs.has_value());
    EXPECT_FALSE(state.gridHeight.has_value());
    EXPECT_FALSE(state.defaultSymbols.has_value());
}

TEST(AgentApiCodec, CandleHalfOpenAndPageLimit) {
    CandleSeriesBuffer buffer;
    std::vector<CandleSeriesBuffer::CandleBar> fixture;
    for (qint64 t : {1000, 2000, 3000}) {
        CandleSeriesBuffer::CandleBar b;
        b.timeStartMs = t; b.timeEndMs = t + 1000;
        b.open = b.high = b.low = b.close = 10;
        b.volume = 2; b.isClosed = true; b.seq = t;
        fixture.push_back(b);
    }
    buffer.applyHistory("BTC-USD", 1, fixture);
    Metadata meta{"s", "BTC-USD", 1, 4000, false, "unknown", false};
    auto first = captureCandles(buffer, "BTC-USD", 1000, 3000, 1000, 1, meta);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->bars.size(), 1);
    EXPECT_EQ(first->bars[0].startMs, 1000);
    EXPECT_EQ(first->nextStartMs, 2000);
    EXPECT_TRUE(first->meta.truncated);
    auto second = captureCandles(buffer, "BTC-USD", *first->nextStartMs, 3000, 1000, 10, meta);
    ASSERT_TRUE(second);
    ASSERT_EQ(second->bars.size(), 1);
    EXPECT_EQ(second->bars[0].startMs, 2000);
    EXPECT_FALSE(second->nextStartMs);
    EXPECT_FALSE(captureCandles(buffer, "BTC-USD", 0, 4000, 60000, 10, meta));
    EXPECT_EQ(candlesJson(*first).value("data").toObject().value("bars").toArray().size(), 1);
}

TEST(AgentApiCodec, BookTopNEmptySidesAndScanBudget) {
    LiveOrderBook book("BTC-USD");
    book.initialize(90, 110, 1);
    const std::vector<BookLevelUpdate> updates{{true, 99, 2}, {true, 98, 3},
                                               {false, 101, 4}, {false, 102, 5}};
    book.applyUpdates(updates, {}, nullptr);
    Metadata meta{"s", "BTC-USD", 1, 1000, false, "unknown", false};
    auto snapshot = captureBook(book, 1, 900, meta);
    ASSERT_EQ(snapshot.bids.size(), 1);
    ASSERT_EQ(snapshot.asks.size(), 1);
    EXPECT_DOUBLE_EQ(*snapshot.bestBid, 99);
    EXPECT_DOUBLE_EQ(*snapshot.bestAsk, 101);
    EXPECT_DOUBLE_EQ(*snapshot.spread, 2);
    EXPECT_FALSE(snapshot.scanLimited);
    EXPECT_EQ(bookJson(snapshot).value("data").toObject().value("band").toArray().size(), 2);
    LiveOrderBook emptySide("BTC-USD");
    emptySide.initialize(90, 110, 1);
    emptySide.applyUpdates(std::vector<BookLevelUpdate>{{false, 100, 1}}, {}, nullptr);
    const auto oneSide = captureBook(emptySide, 20, 900, meta);
    EXPECT_FALSE(oneSide.bestBid);
    EXPECT_FALSE(oneSide.spread);
    EXPECT_TRUE(bookJson(oneSide).value("data").toObject().value("bestBid").isNull());
    emptySide.applyUpdates(std::vector<BookLevelUpdate>{{false, 100, 0}}, {}, nullptr);
    const auto empty = captureBook(emptySide, 20, 900, meta);
    EXPECT_FALSE(empty.bestBid);
    EXPECT_FALSE(empty.bestAsk);
    EXPECT_TRUE(empty.bids.empty());
    EXPECT_TRUE(empty.asks.empty());
    LiveOrderBook wide("BTC-USD");
    wide.initialize(1, 100000, 1);
    wide.applyUpdates(std::vector<BookLevelUpdate>{{true, 2, 1}}, {}, nullptr);
    std::vector<std::pair<uint32_t, double>> bidScratch, askScratch;
    const auto limited = wide.captureDenseNonZero(bidScratch, askScratch, 20, 1);
    EXPECT_TRUE(limited.scanLimited);
    const auto indexed = captureBook(wide, 20, 900, meta);
    ASSERT_TRUE(indexed.bestBid);
    EXPECT_DOUBLE_EQ(*indexed.bestBid, 2);
}

TEST(AgentApiCodec, TradeTapeRetentionEpochAndSummary) {
    TradeTape tape;
    tape.append({1000, 1, "a", "buy", 10, 2});
    tape.append({2000, 1, "b", "sell", 20, 1});
    tape.append({3000, 1, {}, "unknown", 30, 1});
    Metadata meta{"s", "BTC-USD", 1, 3000, false, "unknown", false};
    auto snapshot = tape.snapshot(meta, 3000, 1);
    EXPECT_EQ(snapshot.summary.count, 3);
    EXPECT_DOUBLE_EQ(snapshot.summary.buyQty, 2);
    EXPECT_DOUBLE_EQ(snapshot.summary.sellQty, 1);
    EXPECT_DOUBLE_EQ(snapshot.summary.unknownQty, 1);
    EXPECT_DOUBLE_EQ(snapshot.summary.deltaQty, 1);
    ASSERT_TRUE(snapshot.summary.vwap);
    EXPECT_DOUBLE_EQ(*snapshot.summary.vwap, 17.5);
    EXPECT_EQ(snapshot.trades.size(), 1);
    EXPECT_EQ(snapshot.trades[0].receivedAtMs, 3000);
    EXPECT_TRUE(snapshot.meta.truncated);
    auto json = tradesJson(snapshot).value("data").toObject();
    EXPECT_EQ(json.value("timeBasis").toString(), "received");
    EXPECT_TRUE(json.value("trades").toArray()[0].toObject().value("eventTimeMs").isNull());
    meta.selectionEpoch = 2;
    EXPECT_EQ(tape.snapshot(meta, 3000, 100).summary.count, 0);
    tape.append({4000, 2, {}, "buy", 10, 1});
    meta.observedAtMs = 4000;
    EXPECT_EQ(tape.snapshot(meta, 3000, 100).summary.count, 1);
    for (int i = 0; i <= 10000; ++i) tape.append({5000 + i, 2, {}, "buy", 10, 1});
    meta.observedAtMs = 15000;
    auto capped = tape.snapshot(meta, 20000, 1000);
    EXPECT_EQ(capped.summary.count, 10000);
    EXPECT_TRUE(capped.retentionLimited);
    meta.observedAtMs = 1000000;
    EXPECT_EQ(tape.snapshot(meta, 60000, 1000).summary.count, 0);
    tape.append({1000000, 2, {}, "sell", 11, 2});
    EXPECT_EQ(tape.snapshot(meta, 900000, 1000).summary.count, 1);
}

TEST(AgentApiCodec, SnapshotQueryValidation) {
    EXPECT_EQ(validateQuery({"GET", "/api/v1/candles", "startMs=1&endMs=1&timeframeMs=1000"}, "BTC-USD").code, "invalid_range");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/candles", "startMs=1&endMs=2"}, "BTC-USD").code, "missing_parameter");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/candles", "startMs=1&endMs=2&timeframeMs=1001"}, "BTC-USD").code, "invalid_timeframe");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/candles", "startMs=1&endMs=2&timeframeMs=1000&limit=2001"}, "BTC-USD").code, "invalid_limit");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/book", "levels=201"}, "BTC-USD").code, "invalid_levels");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/trades", "windowMs=900001"}, "BTC-USD").code, "invalid_window");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/trades", "limit=1001"}, "BTC-USD").code, "invalid_limit");
    EXPECT_EQ(validateQuery({"GET", "/api/v1/book", "symbol=ETH-USD"}, "BTC-USD").status, 409);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/book", "levels=1&levels=2"}, "BTC-USD").status, 400);
    RequestParser parser;
    EXPECT_EQ(parser.feed(request("/api/v1/book?levels=1")).kind, ParseResult::Kind::Complete);
}
