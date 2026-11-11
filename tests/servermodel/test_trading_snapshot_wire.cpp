#include "CandleWireFixture.hpp"
#include <gtest/gtest.h>

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
        client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(fixture->port()));
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
    trading::LiveTradingSession &serverTrading() { return fixture->server->tradingSession(); }
    void expectEqualsServer(const trading::TradingSnapshot &client) {
        const auto server = serverTrading().snapshot();
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
    auto &trading = serverTrading();
    trading.processTradeCommand(limitBuy(100.0));
    trading.onTradeTick("BTC-USD", 99.0, 1000);
    trading.processTradeCommand(limitBuy(90.0));
    client->sendAlgoCommand("AvendellaMM", "start", "BTC-USD", quoteParams(), "start-1");
    ASSERT_TRUE(spinUntil([&] { return snapshots.back().commandId == "start-1"; }, 5s))
        << "an algo command must be acknowledged by a snapshot echoing its command_id";
    EXPECT_EQ(snapshots.back().reason, "algo_command");
    ASSERT_EQ(snapshots.back().runningAlgos.size(), 1u);
    trading.onTradeTick("BTC-USD", 99.0, 2000); // algo quotes

    // A second connection sees the whole state at once, equal to the server's.
    {
        std::vector<trading::TradingSnapshot> late;
        SentinelStreamClient other("127.0.0.1", std::to_string(fixture->port()));
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
    const unsigned short port = fixture->port();
    const int connectsBefore = connects;
    fixture.reset();
    ASSERT_TRUE(spinUntil([&] { return disconnects > 0; }, 5s));
    fixture = std::make_unique<CandleFixture>(true, port);
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

} // namespace
