#include "CandleWireFixture.hpp"
#include "protocol/TradeRejectionWire.hpp"
#include <gtest/gtest.h>

struct HeatmapChunkWireTest {
    static void closeListener(SentinelStreamServer& server) {
        net::post(server.m_ioc, [&server] {
            beast::error_code ignored;
            server.m_acceptor->close(ignored);
        });
    }
};

// A real server and SentinelStreamClient over TLS/WebSocket: the trading snapshot on
// connect, as the acknowledgement of an algo command, and after a server restart.
namespace {

trading::TradeCommand limitBuy(double price) {
    trading::TradeCommand cmd;
    cmd.commandId = "limit";
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = "BTC-USD";
    cmd.side = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Limit;
    cmd.price = price;
    cmd.hasPrice = true;
    cmd.qty = 1.0;
    return cmd;
}

trading::AlgoParams quoteParams() {
    trading::AlgoParams p;
    p.spreadBps = 10.0;
    p.orderQty = 1.0;
    p.maxPositionQty = 3.0;
    p.skewBps = 0.0;
    return p;
}

std::vector<std::string> orderIds(const trading::TradingSnapshot& s) {
    std::vector<std::string> ids;
    for (const auto& o : s.openOrders) ids.push_back(o.orderId);
    std::sort(ids.begin(), ids.end());
    return ids;
}

class TradingSnapshotWire : public testing::Test {
  protected:
    int argc = 1;
    char name[24] = "trading-snapshot-wire";
    char *argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    std::unique_ptr<CandleFixture> fixture = std::make_unique<CandleFixture>();
    std::unique_ptr<SentinelStreamClient> client;
    std::vector<trading::TradingSnapshot> snapshots;
    int connects = 0;
    int disconnects = 0;

    void SetUp() override {
        qRegisterMetaType<trading::TradingSnapshot>("trading::TradingSnapshot");
        fixture->start();
        client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(fixture->port()),
                                                        fixture->config.tls.certFile);
        QObject::connect(client.get(), &SentinelStreamClient::connected, &app, [this] { ++connects; });
        QObject::connect(client.get(), &SentinelStreamClient::disconnected, &app, [this] { ++disconnects; });
        QObject::connect(client.get(), &SentinelStreamClient::tradingSnapshotReceived, &app,
                         [this](const trading::TradingSnapshot &s) { snapshots.push_back(s); });
        client->connectToServer();
    }
    void TearDown() override {
        client->disconnectFromServer();
        client.reset();
    }
    // The server makes every trading call on its I/O thread; so does the test.
    template <typename Fn> auto onServer(Fn fn) {
        using R = decltype(fn(fixture->server->tradingSession()));
        auto done = std::make_shared<std::promise<R>>();
        auto result = done->get_future();
        net::post(RecordingServerStopTest::io(*fixture->server), [this, fn, done] {
            if constexpr (std::is_void_v<R>) {
                fn(fixture->server->tradingSession());
                done->set_value();
            } else {
                done->set_value(fn(fixture->server->tradingSession()));
            }
        });
        EXPECT_TRUE(spinUntil([&] { return result.wait_for(0ms) == std::future_status::ready; }));
        return result.get();
    }
    void expectEqualsServer(const trading::TradingSnapshot &client) {
        const auto server = onServer([](trading::LiveTradingSession &t) { return t.snapshot(); });
        EXPECT_EQ(client.epoch, server.epoch);
        EXPECT_EQ(orderIds(client), orderIds(server));
        ASSERT_EQ(client.positions.size(), server.positions.size());
        for (const auto &p : server.positions) {
            const auto *c = client.positionFor(p.symbol);
            ASSERT_NE(c, nullptr);
            EXPECT_DOUBLE_EQ(c->positionQty, p.positionQty);
            EXPECT_DOUBLE_EQ(c->avgPrice, p.avgPrice);
        }
        EXPECT_EQ(client.runningAlgos, server.runningAlgos);
    }
};

TEST_F(TradingSnapshotWire, ConnectThenAlgoAcksThenServerRestartClearsEverything) {
    ASSERT_TRUE(spinUntil([&] { return !snapshots.empty(); }, 5s)) << "no trading_snapshot on connect";
    EXPECT_EQ(snapshots.front().reason, "connect");
    EXPECT_TRUE(snapshots.front().openOrders.empty());
    const std::string firstEpoch = snapshots.front().epoch;
    ASSERT_FALSE(firstEpoch.empty());

    // Build state on server A: a long, a resting bid, a running algo with quotes.
    onServer([](trading::LiveTradingSession &t) {
        t.processTradeCommand(limitBuy(100.0));
        t.onTradeTick("BTC-USD", 99.0, 1000);
        t.processTradeCommand(limitBuy(90.0));
    });
    client->sendAlgoCommand("AvendellaMM", "start", "BTC-USD", quoteParams(), "start-1");
    ASSERT_TRUE(spinUntil([&] { return snapshots.back().commandId == "start-1"; }, 5s))
        << "an algo command must be acknowledged by a snapshot echoing its command_id";
    EXPECT_EQ(snapshots.back().reason, "algo_command");
    ASSERT_EQ(snapshots.back().runningAlgos.size(), 1u);
    onServer([](trading::LiveTradingSession &t) { t.onTradeTick("BTC-USD", 99.0, 2000); }); // algo quotes

    // A second connection sees the whole state at once, equal to the server's.
    {
        std::vector<trading::TradingSnapshot> late;
        SentinelStreamClient other("127.0.0.1", std::to_string(fixture->port()),
                                   fixture->config.tls.certFile);
        QObject::connect(&other, &SentinelStreamClient::tradingSnapshotReceived, &app,
                         [&late](const trading::TradingSnapshot &s) { late.push_back(s); });
        other.connectToServer();
        ASSERT_TRUE(spinUntil([&] { return !late.empty(); }, 5s));
        EXPECT_EQ(late.front().reason, "connect");
        EXPECT_EQ(late.front().openOrders.size(), 3u) << "manual bid plus the algo's bid and ask";
        expectEqualsServer(late.front());
        other.disconnectFromServer();
    }

    client->sendAlgoCommand("AvendellaMM", "stop", "BTC-USD", {}, "stop-1");
    ASSERT_TRUE(spinUntil([&] { return snapshots.back().commandId == "stop-1"; }, 5s));
    EXPECT_TRUE(snapshots.back().runningAlgos.empty()) << "stop acknowledged: the algo is not running";
    EXPECT_EQ(snapshots.back().openOrders.size(), 1u) << "only the manual bid survives the stop";
    expectEqualsServer(snapshots.back());
    ASSERT_NE(snapshots.back().positionFor("BTC-USD"), nullptr);
    EXPECT_DOUBLE_EQ(snapshots.back().positionFor("BTC-USD")->positionQty, 1.0);

    // Restart: server A goes away, server B (fresh paper account) takes the same port.
    // Server B keeps A's certificate: the client verifies against the CA it loaded at start.
    const unsigned short port = fixture->port();
    const int connectsBefore = connects;
    QTemporaryDir tls;
    ASSERT_TRUE(tls.isValid());
    ASSERT_TRUE(QFile::copy(QString::fromStdString(fixture->config.tls.certFile), tls.filePath("fixture.crt")));
    ASSERT_TRUE(QFile::copy(QString::fromStdString(fixture->config.tls.keyFile), tls.filePath("fixture.key")));
    fixture.reset();
    ASSERT_TRUE(spinUntil([&] { return disconnects > 0; }, 5s));
    fixture = std::make_unique<CandleFixture>(true, port, std::vector<int64_t>{}, tls.path());
    fixture->start();
    ASSERT_TRUE(spinUntil([&] { return connects > connectsBefore && snapshots.back().epoch != firstEpoch; }, 15s))
        << "the client must reconnect and receive the restarted server's snapshot";
    const auto &fresh = snapshots.back();
    EXPECT_EQ(fresh.reason, "connect");
    EXPECT_EQ(fresh.positionFor("BTC-USD"), nullptr) << "no stale position survives a restart";
    EXPECT_TRUE(fresh.openOrders.empty()) << "no ghost order survives a restart";
    EXPECT_TRUE(fresh.runningAlgos.empty()) << "no stale algo state survives a restart";
    expectEqualsServer(fresh);
}

TEST_F(TradingSnapshotWire, SharedTradingTicksOnceWithZeroOneAndTwoClients) {
    ASSERT_TRUE(spinUntil([&] { return !snapshots.empty(); }));
    auto ticks = std::make_shared<std::atomic<int>>(0);
    onServer([&](trading::LiveTradingSession& t) {
        t.processTradeCommand(limitBuy(100));
        t.onTradeTick("BTC-USD", 99, 1000); // establish a position for one PnL per tick
        t.setResultCallback([ticks](trading::TradingResult r, auto) { *ticks += int(r.pnlSnapshots.size()); });
    });
    const auto tick = [&](int phase) {
        ticks->store(0);
        tradeAt(*fixture->model, 2000 + phase, "BTC-USD", 99);
        ASSERT_TRUE(fixture->fence());
        EXPECT_EQ(ticks->load(), 1) << "phase " << phase;
    };
    tick(1);
    SentinelStreamClient other("127.0.0.1", std::to_string(fixture->port()), fixture->config.tls.certFile);
    bool ready = false;
    QObject::connect(&other, &SentinelStreamClient::tradingSnapshotReceived, &app,
                     [&](const auto&) { ready = true; });
    other.connectToServer();
    ASSERT_TRUE(spinUntil([&] { return ready; }));
    tick(2);
    other.disconnectFromServer();
    client->disconnectFromServer();
    ASSERT_TRUE(spinUntil([&] { return fixture->server->sessionCount() == 0; }));
    tick(3);
    fixture->server->stop();
    ASSERT_TRUE(fixture->server->start());
    tick(4);
    onServer([](trading::LiveTradingSession& t) { t.setResultCallback({}); });
}

TEST(StreamListenerMetrics, LifecycleFailureRestartAndDestruction) {
    int argc = 1;
    char name[] = "listener-metrics";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    sentinel::metrics::MetricsRegistry registry;
    auto fixture = std::make_unique<CandleFixture>();
    fixture->server->registerMetrics(registry);
    const auto expect = [&](int value) {
        EXPECT_NE(registry.render().find("sentinel_stream_listening " + std::to_string(value) + "\n"),
                  std::string::npos);
    };
    expect(0);
    ASSERT_TRUE(fixture->server->start());
    expect(1);
    {
        sentinel::metrics::MetricsRegistry failedRegistry;
        CandleFixture conflict(true, fixture->port());
        conflict.server->registerMetrics(failedRegistry);
        EXPECT_FALSE(conflict.server->start());
        EXPECT_NE(failedRegistry.render().find("sentinel_stream_listening 0\n"), std::string::npos);
    }
    HeatmapChunkWireTest::closeListener(*fixture->server);
    EXPECT_TRUE(spinUntil([&] {
        return registry.render().find("sentinel_stream_listening 0\n") != std::string::npos;
    }));
    fixture->server->stop();
    expect(0);
    ASSERT_TRUE(fixture->server->start());
    expect(1);
    fixture.reset();
    expect(0); // the registry can outlive its listener
}

TEST(TradingRejectionWire, CommandsAndRiskTicksPublishReasonsWithoutPhantomOrders) {
    int argc = 1;
    char name[] = "trading-rejection";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    CandleFixture fixture;
    fixture.config.trading.slippageBps = 10000;
    fixture.server = std::make_unique<SentinelStreamServer>(*fixture.model, *fixture.auth, fixture.config, 0);
    ASSERT_TRUE(fixture.server->start());
    std::vector<Json> rejected;
    fixture.server->registerTradingBroadcaster([&](const std::string& bytes) {
        auto message = Json::parse(bytes);
        if (message.value("type", "") == "trade_rejected") rejected.push_back(std::move(message));
    });
    net::post(RecordingServerStopTest::io(*fixture.server), [&] {
        auto& t = fixture.server->tradingSession();
        trading::TradeCommand flatten;
        flatten.action = trading::TradeAction::Flatten;
        flatten.commandId = "empty-flatten";
        flatten.symbol = "BTC-USD";
        flatten.timestamp = 10;
        t.processTradeCommand(flatten);
        t.processTradeCommand(limitBuy(100));
        t.onTradeTick("BTC-USD", 99, 1000);
        trading::TradeCommand risk;
        risk.action = trading::TradeAction::SetAttachedRisk;
        risk.symbol = "BTC-USD";
        risk.hasStopLoss = true;
        risk.stopLossPrice = 90;
        t.processTradeCommand(risk);
        t.onTradeTick("BTC-USD", 89, 2000); // sell price would be zero
        t.onTradeTick("BTC-USD", 88, 3000); // same failed risk is edge-triggered
        const auto state = t.snapshot();
        EXPECT_TRUE(state.openOrders.empty());
        ASSERT_NE(state.positionFor("BTC-USD"), nullptr);
        EXPECT_EQ(state.positionFor("BTC-USD")->positionQty, 1);
        EXPECT_EQ(state.riskOrders.size(), 1u);
    });
    ASSERT_TRUE(fixture.fence());
    ASSERT_EQ(rejected.size(), 2u);
    EXPECT_EQ(rejected[0]["schema_version"], 1);
    EXPECT_EQ(rejected[0]["command_id"], "empty-flatten");
    EXPECT_EQ(rejected[0]["reason"], "no_position");
    EXPECT_EQ(rejected[1]["symbol"], "BTC-USD");
    EXPECT_EQ(rejected[1]["command_id"], "");
    EXPECT_EQ(rejected[1]["reason"], "invalid_fill_price");
    EXPECT_EQ(rejected[1]["timestamp_ms"], 2000);
}

} // namespace

TEST(TradingRejectionWire, CodecRejectsMalformedOrUnknownSchema) {
    const trading::TradeRejection rejection{"BTC-USD", "command-1", "no_position", "", 1234};
    const auto encoded = protocol::tradeRejectionJson(rejection);
    const auto decoded = protocol::parseTradeRejection(Json::parse(encoded.dump()));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->commandId, rejection.commandId);
    EXPECT_EQ(decoded->reason, rejection.reason);
    EXPECT_EQ(decoded->timestampMs, rejection.timestampMs);
    auto bad = encoded;
    bad["schema_version"] = 2;
    EXPECT_FALSE(protocol::parseTradeRejection(bad));
    bad = encoded; bad.erase("reason");
    EXPECT_FALSE(protocol::parseTradeRejection(bad));
    bad = encoded; bad["reason"] = "";
    EXPECT_FALSE(protocol::parseTradeRejection(bad));
    bad = encoded; bad["timestamp_ms"] = UINT64_MAX;
    EXPECT_FALSE(protocol::parseTradeRejection(bad));
    bad = encoded; bad["timestamp_ms"] = 1.5;
    EXPECT_FALSE(protocol::parseTradeRejection(bad));
}

TEST(StreamListenerMetrics, RegistrationAndTlsSetupFailureWithoutBinding) {
    int argc = 1;
    char name[] = "listener-metrics-failure";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    ServerConfig config;
    config.recording.enabled = false;
    config.rollerShadow.enabled = false;
    config.tls.certFile = (scratch.path() + "/missing.crt").toStdString();
    config.tls.keyFile = (scratch.path() + "/missing.key").toStdString();
    ServerDataModel model(config);
    Authenticator auth((scratch.path() + "/no-credentials").toStdString());
    sentinel::metrics::MetricsRegistry registry;
    auto server = std::make_unique<SentinelStreamServer>(model, auth, config, 0);
    server->registerMetrics(registry);
    const auto down = [&] {
        EXPECT_NE(registry.render().find("sentinel_stream_listening 0\n"), std::string::npos);
    };
    down();
    EXPECT_FALSE(server->start()); // missing TLS certificate: fails before opening a listener
    down();
    server->stop();
    down();
    server.reset();
    down();
}
