#include "AgentApiCodec.hpp"
#include "config/ConfigTypes.hpp"
#include <gtest/gtest.h>
#include <QJsonDocument>
#include <QJsonArray>

using namespace AgentApi;

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
    EXPECT_EQ(validateQuery({"GET", "/api/v1/screenshot", "afterOperation=o1"}).status, 422);
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
