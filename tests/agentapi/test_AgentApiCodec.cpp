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
    // Auto-fit (the axis double-click action): one of time/price/both, alone.
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"price"})").body.fit, "price");
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"time"})").status, 200);
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"both"})").status, 200);
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"all"})").code, "invalid_fit");
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":true})").code, "invalid_fit");
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"price","followLive":true})").code, "invalid_fit");
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"time","startMs":1,"endMs":10})").code, "invalid_fit");
    EXPECT_EQ(check("/api/v1/viewport", R"({"fit":"default"})").body.fit, "default");
    // The auto price scale toggle: a boolean; on cannot come with price bounds or fit.
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":true})").body.autoScale, std::optional<bool>(true));
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":false,"priceMin":1,"priceMax":2})").status, 200);
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":true,"startMs":1,"endMs":10})").status, 200);
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":1})").code, "invalid_auto_scale");
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":true,"priceMin":1,"priceMax":2})").code, "invalid_auto_scale");
    EXPECT_EQ(check("/api/v1/viewport", R"({"autoScale":true,"fit":"price"})").code, "invalid_fit");
    EXPECT_EQ(check("/api/v1/layers", R"({"heatmap":true,"candles":false,"tpo":true})").body.layers.size(), 3);
    EXPECT_EQ(check("/api/v1/layers", R"({"tpo":1})").status, 422);
    EXPECT_EQ(check("/api/v1/layers", R"({"unknown":true})").status, 422);
    EXPECT_EQ(check("/api/v1/layers", R"({"tpo":true,"tpoLayout":"split","tpoTheme":"calm"})").body.layers.size(), 3);
    EXPECT_EQ(check("/api/v1/layers", R"({"tpoLayout":"sideways"})").status, 422);
    EXPECT_EQ(check("/api/v1/layers", R"({"tpoTheme":true})").status, 422);
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

TEST(AgentApiControls, DocksValidateIdsFocusAndHideAll) {
    const QJsonObject current{{"heatmap", true}, {"orderBook", true}, {"watchlist", false}};
    auto check = [&](const char* body) {
        return validateControl({"POST", "/api/v1/docks", {}, body}, std::nullopt, current);
    };
    EXPECT_EQ(check(R"({"focus":"heatmap"})").body.dockFocus, "heatmap");
    EXPECT_EQ(check(R"({"visible":{"orderBook":false}})").status, 200);
    EXPECT_EQ(check(R"({"visible":{"heatmap":false,"orderBook":false}})").code, "hide_all_docks");
    const auto unknown = check(R"({"visible":{"missing":false}})");
    EXPECT_EQ(unknown.status, 422);
    EXPECT_EQ(unknown.code, "unknown_dock");
    EXPECT_TRUE(unknown.message.contains("heatmap"));
    EXPECT_EQ(check(R"({"focus":"missing"})").code, "unknown_dock");
    EXPECT_EQ(check(R"({"focus":"heatmap","visible":{"orderBook":false}})").status, 422);
    EXPECT_EQ(check(R"({"visible":{"orderBook":0}})").status, 422);
    EXPECT_EQ(check(R"({"focus":"heatmap","persist":true})").body.persistDocks, true);
    EXPECT_EQ(check(R"({"focus":"heatmap"})").body.persistDocks, false);
    RequestParser get;
    EXPECT_EQ(get.feed("GET /api/v1/docks HTTP/1.1\r\nHost: localhost\r\n\r\n").kind, ParseResult::Kind::Complete);
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

TEST(AgentApiControls, PendingSymbolRequiresANewFrameAfterActivation) {
    Operations ops;
    const auto symbol = ops.apply("symbol", 10, true);
    const auto other = ops.apply("timeframe", 11);
    ops.poll(other.revision, 42);
    EXPECT_EQ(ops.find(symbol.id)->status, "pending");
    const auto activated = ops.activate(symbol.id);
    ASSERT_TRUE(activated.has_value());
    EXPECT_GT(activated->revision, other.revision);
    ops.poll(other.revision, 43);
    EXPECT_EQ(ops.find(symbol.id)->status, "applied");
    ops.poll(activated->revision, 44);
    EXPECT_EQ(ops.find(symbol.id)->status, "rendered");
    EXPECT_EQ(ops.find(symbol.id)->frameId, 44u);
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

TEST(AgentApiWalls, QueryLimitsAndJson) {
    RequestParser parser;
    EXPECT_EQ(parser.feed(request("/api/v1/heatmap/walls?limit=2")).kind, ParseResult::Kind::Complete);
    auto valid = validateQuery({"GET", "/api/v1/heatmap/walls",
        "startMs=100&endMs=200&priceMin=99.5&priceMax=100.5&minQty=0.25&limit=100&symbol=BTC-USD"}, "BTC-USD");
    EXPECT_EQ(valid.status, 200);
    EXPECT_EQ(valid.walls.limit, 100);
    EXPECT_DOUBLE_EQ(valid.walls.minQty, 0.25);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "limit=101"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "limit=0"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "limit=1&limit=2"}).status, 400);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "minQty=-1"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "priceMin=2&priceMax=1"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "startMs=2&endMs=1"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/walls", "symbol=ETH-USD"}, "BTC-USD").status, 409);
    WallsSnapshot snapshot;
    snapshot.meta = {"s1", "BTC-USD", 1, 100, false, "partial", false};
    snapshot.data.loadedStartMs = 0;
    snapshot.data.loadedEndMs = 200;
    snapshot.data.bandTick = 0.5;
    snapshot.data.recordedColumns = 1;
    snapshot.data.missingColumns = 1;
    snapshot.data.walls.push_back({100, 99.5, 100, true, 2, 199.5, false});
    const auto json = wallsJson(snapshot);
    EXPECT_EQ(json.value("data").toObject().value("basis").toString(), "recording-twap-sum");
    EXPECT_EQ(json.value("data").toObject().value("walls").toArray().first().toObject().value("side").toString(), "ask");
    EXPECT_EQ(json.value("meta").toObject().value("coverage").toString(), "partial");
}
TEST(AgentApiWalls, GpuPeriodTickAndPersistenceMetadata) {
    auto parse = [](QString query) { return validateQuery({"GET", "/api/v1/heatmap/walls", query}, "BTC-USD"); };
    const auto q = parse("from_ms=100&to_ms=200&tick=5");
    EXPECT_EQ(q.status, 200); EXPECT_EQ(q.walls.startMs, 100); EXPECT_EQ(q.walls.endMs, 200); EXPECT_EQ(q.walls.tick, 5);
    for (const auto* bad : {"from_ms=100", "to_ms=200", "from_ms=200&to_ms=100", "from_ms=-1&to_ms=200",
                           "from_ms=1.5&to_ms=200", "tick=0", "tick=-1", "tick=nan", "tick=inf"})
        EXPECT_EQ(parse(bad).status, 422) << bad;
    EXPECT_EQ(parse("from_ms=100&startMs=100&to_ms=200").status, 400);
    WallsSnapshot snapshot;
    snapshot.data.bandTick = 5;
    snapshot.data.rangeStartMs = 100; snapshot.data.rangeEndMs = 200;
    snapshot.data.rangePriceMin = 90; snapshot.data.rangePriceMax = 110;
    snapshot.data.unknownRows = true;
    snapshot.data.walls.push_back({100, 95, 100, false, 2, 195, true, 1.5, 100, 150, 2});
    const auto data = wallsJson(snapshot)["data"].toObject();
    EXPECT_EQ(data["renderer"], "gpu"); EXPECT_EQ(data["tick"], 5);
    EXPECT_EQ(data["range"].toObject()["from_ms"].toDouble(), 100);
    EXPECT_TRUE(data["unknownRows"].toBool());
    const auto wall = data["walls"].toArray().first().toObject();
    EXPECT_EQ(wall["rank"], 1); EXPECT_EQ(wall["columns"], 2); EXPECT_EQ(wall["meanQty"], 1.5);
    EXPECT_TRUE(wall["forming"].toBool());
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
    for (const char* overlay : {"footprint", "tpo", "volumeProfile"})
        EXPECT_TRUE(data.value("lastReceivedAtMs").toObject().value(overlay).isNull()) << overlay;
    s.footprintReceivedAtMs = 11; s.tpoReceivedAtMs = 12; s.volumeProfileReceivedAtMs = 13;
    const auto received = stateJson(s).value("data").toObject().value("lastReceivedAtMs").toObject();
    EXPECT_EQ(received.value("footprint").toInteger(), 11);
    EXPECT_EQ(received.value("tpo").toInteger(), 12);
    EXPECT_EQ(received.value("volumeProfile").toInteger(), 13);
    EXPECT_TRUE(data.value("layers").toObject().value("heatmap").isNull());
    EXPECT_TRUE(data.value("render").toObject().value("frameP50Ms").isNull());
    EXPECT_TRUE(data.value("lastSubscriptionRefusal").isNull());
    s.lastSubscriptionRefusal = {{"symbol", "ETH-USD"}, {"maxConnections", 8}, {"message", "cap reached"}};
    EXPECT_EQ(stateJson(s).value("data").toObject().value("lastSubscriptionRefusal").toObject(), s.lastSubscriptionRefusal);
    s.frameP50Ms = 4.5;
    s.frameP95Ms = 7.25;
    s.renderRateHz = 20.0;
    s.frameIdle = true;
    const auto render = stateJson(s).value("data").toObject().value("render").toObject();
    EXPECT_DOUBLE_EQ(render.value("frameP50Ms").toDouble(), 4.5);
    EXPECT_DOUBLE_EQ(render.value("frameP95Ms").toDouble(), 7.25);
    EXPECT_DOUBLE_EQ(render.value("rateHz").toDouble(), 20.0);
    EXPECT_TRUE(render.value("idle").toBool());
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
    s.autoScale = true;
    s.viewportVersion = 42;
    s.widthPx = 1200;
    s.heightPx = 600;
    auto data = viewportJson(s).value("data").toObject();
    EXPECT_EQ(data.value("viewportVersion").toString(), "42");
    EXPECT_TRUE(data.value("autoScale").toBool());
    EXPECT_DOUBLE_EQ(data.value("zoom").toObject().value("msPerPx").toDouble(), 3000);
    EXPECT_NEAR(data.value("zoom").toObject().value("pricePerPx").toDouble(), 1.6666667, 1e-6);
    s.widthPx.reset();
    data = viewportJson(s).value("data").toObject();
    EXPECT_TRUE(data.value("widthPx").isNull());
    EXPECT_TRUE(data.value("zoom").toObject().value("msPerPx").isNull());
    EXPECT_TRUE(data.value("drawn").isNull()) << "no frame drawn yet";
    // Whole-pixel mapping: what the last frame drew (the raster camera).
    s.drawn = ViewportSnapshot::Drawn{1790593187500.0, 1790596787500.0, 63999.5, 65000.5, 2, 16, 2.0};
    const auto drawn = viewportJson(s).value("data").toObject().value("drawn").toObject();
    EXPECT_DOUBLE_EQ(drawn.value("startMs").toDouble(), 1790593187500.0);
    EXPECT_DOUBLE_EQ(drawn.value("endMs").toDouble(), 1790596787500.0);
    EXPECT_DOUBLE_EQ(drawn.value("priceMin").toDouble(), 63999.5);
    EXPECT_DOUBLE_EQ(drawn.value("priceMax").toDouble(), 65000.5);
    EXPECT_EQ(drawn.value("rowPx").toInt(), 2);
    EXPECT_EQ(drawn.value("colPx").toInt(), 16);
    EXPECT_DOUBLE_EQ(drawn.value("dpr").toDouble(), 2.0);
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
    for (const char *target : {"settings", "settings:Look", "settings:Budgets", "telemetry", "toolbar", "chartmenu",
                               "orderBook", "watchlist", "screener", "stockChart", "paperTrading", "sec",
                               "copenet", "statusBar", "window"})
        EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", QString("target=") + target}).status, 200) << target;
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "target=lab"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "target=aiCommentary"}).status, 422);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "target=settings:Nope"}).status, 422);
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

TEST(AgentApiHeatmap, NewRoutesAndStrictInputKinds) {
    for (const auto &route : {QByteArray("/api/v1/heatmap/settings"), QByteArray("/api/v1/input")}) {
        RequestParser parser;
        EXPECT_EQ(parser.feed("POST " + route + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}").kind, ParseResult::Kind::Complete);
        RequestParser wrongMethod;
        EXPECT_EQ(wrongMethod.feed("GET " + route + " HTTP/1.1\r\nHost: localhost\r\n\r\n").status, 405);
    }
    RequestParser parser;
    EXPECT_EQ(parser.feed("GET /api/v1/heatmap/state HTTP/1.1\r\nHost: localhost\r\n\r\n").kind, ParseResult::Kind::Complete);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/state", "symbol=ETH-USD", {}}, "BTC-USD").status, 409);
    EXPECT_EQ(validateQuery({"GET", "/api/v1/heatmap/state", "bad=1", {}}, "BTC-USD").status, 400);
    for (const char *kind : {"wheel", "dragStart", "dragMove", "dragEnd", "click", "doubleClick"}) {
        for (const char *target : {"chart", "priceAxis", "timeAxis"}) {
            QJsonObject body{{"kind", kind}, {"target", target}, {"x", 10.5}, {"y", 20}, {"modifiers", QJsonArray{"shift", "alt"}}};
            if (QString(kind) == "wheel") body["deltaY"] = -120;
            const auto out = validateControl({"POST", "/api/v1/input", {}, QJsonDocument(body).toJson()}, {});
            EXPECT_EQ(out.status, 200) << kind << " " << target;
            EXPECT_EQ(out.body.input.kind, kind); EXPECT_EQ(out.body.input.target, target);
            EXPECT_EQ(out.body.input.x, 10.5); EXPECT_EQ(out.body.input.modifiers.size(), 2);
        }
    }
    const auto input = [](const char *json) { return validateControl({"POST", "/api/v1/input", {}, json}, {}).status; };
    for (const char *bad : {
        R"({"kind":"wheel","target":"chart","x":1,"y":1})",
        R"({"kind":"wheel","target":"chart","x":1,"y":1,"deltaY":0})",
        R"({"kind":"wheel","target":"chart","x":1,"y":1,"deltaY":1.5})",
        R"({"kind":"wheel","target":"chart","x":1,"y":1,"deltaY":12001})",
        R"({"kind":"click","target":"chart","x":-1,"y":1})",
        R"({"kind":"click","target":"chart","x":"1","y":1})",
        R"({"kind":"click","target":"chart","x":1,"y":1,"deltaY":120})",
        R"({"kind":"click","target":"chart","x":1,"y":1,"modifiers":["shift","shift"]})",
        R"({"kind":"click","target":"chart","x":1,"y":1,"modifiers":["super"]})",
        R"({"kind":"click","target":"chart","x":1,"y":1,"extra":true})",
        R"({"kind":"type","target":"chart","x":1,"y":1})",
        R"({"kind":"click","target":"main","x":1,"y":1})"}) EXPECT_EQ(input(bad), 422) << bad;
}
TEST(AgentApiHeatmap, SettingsPartialTypesAndGradientValidation) {
    const auto check = [](const char *json) { return validateControl({"POST", "/api/v1/heatmap/settings", {}, json}, {}); };
    EXPECT_EQ(check(R"({"renderer":"gpu","manualTick":250,"opacity":0.5})").body.heatmapSettings.size(), 3);
    EXPECT_EQ(check(R"({"opacity":-2})").status, 200); // numeric limits clamp in the settings model
    for (const char *bad : {R"({"renderer":"bad"})", R"({"tickMode":"AUTO"})", R"({"manualTick":1.5})",
        R"({"opacity":null})", R"({"showTelemetry":1})", R"({"unknown":1})", R"({"palettePreset":"bad"})",
        R"({"bidGradient":[{"position":0,"color":"red"},{"position":1,"color":"#ffffff"}]})",
        R"({"askGradient":[{"position":0.5,"color":"#000000"},{"position":1,"color":"#ffffff"}]})",
        R"({"bidGradient":[]})"}) EXPECT_EQ(check(bad).status, 422) << bad;
}

TEST(AgentApiHeatmap, CandleSettingsAreExposedAndValidated) {
    const auto check = [](const char *json) { return validateControl({"POST", "/api/v1/heatmap/settings", {}, json}, {}); };
    const auto valid = check(R"({"candleUpColor":"#2EBD85","candleDownColor":"#F6465D","candleWickColor":"auto","candleBodyOpacity":1,"candleWickWidth":1})");
    ASSERT_EQ(valid.status, 200);
    EXPECT_EQ(valid.body.heatmapSettings.size(), 5);
    for (const char *bad : {R"({"candleUpColor":"green"})", R"({"candleDownColor":"#1234"})",
                            R"({"candleWickColor":"transparent"})", R"({"candleWickWidth":0})",
                            R"({"candleWickWidth":4})", R"({"candleWickWidth":1.5})",
                            R"({"candleBodyOpacity":"100%"})"})
        EXPECT_EQ(check(bad).status, 422) << bad;
}

TEST(AgentApiHeatmap, ProcessOnlySettingsFlagIsStrictAndRemovedFromThePatch) {
    const auto check = [](const char *json) { return validateControl({"POST", "/api/v1/heatmap/settings", {}, json}, {}); };
    auto c = check(R"({"renderer":"gpu","persist":false})");
    ASSERT_EQ(c.status, 200);
    EXPECT_FALSE(c.body.persistHeatmapSettings);
    EXPECT_EQ(c.body.heatmapSettings.size(), 1);
    EXPECT_EQ(c.body.heatmapSettings["renderer"], "gpu");
    EXPECT_TRUE(check(R"({"opacity":0.4})").body.persistHeatmapSettings);
    EXPECT_TRUE(check(R"({"opacity":0.4,"persist":true})").body.persistHeatmapSettings);
    for (const auto *body : {R"({"persist":false})", R"({"renderer":"gpu","persist":0})", R"({"renderer":"gpu","persist":"false"})"})
        EXPECT_EQ(check(body).status, 422);
}

TEST(AgentApiCodec, TradeEvidencePreservesEventTimeSeparatelyFromReceiveWindow) {
    TradeTape tape;
    tape.append({3000, 1, "123", "buy", 10, 2, 1000});
    const auto s = tape.snapshot({"s", "BTC-USD", 1, 3000, false, "unknown", false}, 1000, 100);
    const auto data = tradesJson(s).value("data").toObject();
    ASSERT_EQ(s.trades.size(), 1);
    EXPECT_EQ(data["timeBasis"].toString(), "received");
    EXPECT_EQ(data["trades"].toArray()[0].toObject()["eventTimeMs"].toInteger(), 1000);
}
