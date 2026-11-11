#include <gtest/gtest.h>

#include "render/AlgoOrderSpans.hpp"
#include "trading/AvendellaMM.hpp"
#include "trading/LiveTradingSession.hpp"
#include "trading/TradingSnapshot.hpp"
#include "widgets/AlgoStopPolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace {

trading::AlgoParams quoteParams() {
    trading::AlgoParams p;
    p.spreadBps = 10.0;
    p.orderQty = 1.0;
    p.maxPositionQty = 2.0;
    p.skewBps = 0.0;
    return p;
}

trading::TradeCommand limit(const std::string& id, trading::OrderSide side, double price, double qty = 1.0) {
    trading::TradeCommand cmd;
    cmd.commandId = id;
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = "BTC-USD";
    cmd.side = side;
    cmd.orderType = trading::OrderType::Limit;
    cmd.price = price;
    cmd.hasPrice = true;
    cmd.qty = qty;
    return cmd;
}

trading::TradeCommand attachRisk(double tp, double sl) {
    trading::TradeCommand cmd;
    cmd.action = trading::TradeAction::SetAttachedRisk;
    cmd.symbol = "BTC-USD";
    cmd.hasTakeProfit = true;
    cmd.takeProfitPrice = tp;
    cmd.hasStopLoss = true;
    cmd.stopLossPrice = sl;
    return cmd;
}

// Long 1 BTC-USD at 100, a resting manual bid at 90, TP/SL, AvendellaMM quoting.
void tradeIntoState(trading::LiveTradingSession& session) {
    session.registerAlgo(std::make_unique<trading::AvendellaMM>());
    session.processTradeCommand(limit("fill", trading::OrderSide::Buy, 100.0));
    session.onTradeTick("BTC-USD", 99.0, 1000);  // fills the 100 bid
    session.processTradeCommand(limit("rest", trading::OrderSide::Buy, 90.0));
    session.processTradeCommand(attachRisk(150.0, 50.0));
    ASSERT_TRUE(session.startAlgo("AvendellaMM", "BTC-USD", quoteParams()));
    session.onTradeTick("BTC-USD", 99.0, 2000);  // algo quotes around 99
}

trading::TradingSnapshot overTheWire(const trading::TradingSnapshot& server) {
    const auto parsed = trading::parseTradingSnapshotJson(nlohmann::json::parse(trading::tradingSnapshotJson(server).dump()));
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(trading::TradingSnapshot{});
}

bool sameOrders(std::vector<trading::OrderUpdate> a, std::vector<trading::OrderUpdate> b) {
    auto byId = [](const auto& l, const auto& r) { return l.orderId < r.orderId; };
    std::sort(a.begin(), a.end(), byId);
    std::sort(b.begin(), b.end(), byId);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].orderId != b[i].orderId || a[i].symbol != b[i].symbol || a[i].status != b[i].status ||
            a[i].side != b[i].side || a[i].qty != b[i].qty || a[i].filledQty != b[i].filledQty ||
            a[i].remainingQty != b[i].remainingQty || a[i].limitPrice != b[i].limitPrice ||
            a[i].avgPrice != b[i].avgPrice || a[i].algoId != b[i].algoId) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(TradingSnapshot, ListsEveryPositionOrderRiskLegAndRunningAlgo) {
    trading::LiveTradingSession session([](const std::string&) { return 99.0; }, 0.0);
    tradeIntoState(session);
    const auto snap = session.snapshot();

    EXPECT_FALSE(snap.epoch.empty());
    ASSERT_NE(snap.positionFor("BTC-USD"), nullptr);
    EXPECT_DOUBLE_EQ(snap.positionFor("BTC-USD")->positionQty, 1.0);
    EXPECT_GT(snap.positionFor("BTC-USD")->avgPrice, 0.0);

    const auto manual = std::count_if(snap.openOrders.begin(), snap.openOrders.end(),
                                      [](const auto& o) { return o.algoId.empty(); });
    const auto quotes = std::count_if(snap.openOrders.begin(), snap.openOrders.end(),
                                      [](const auto& o) { return o.algoId == "AvendellaMM"; });
    EXPECT_EQ(manual, 1);
    EXPECT_EQ(quotes, 2);
    EXPECT_EQ(snap.openOrders.size(), session.broker().snapshotFor("BTC-USD", 0).openOrders.size());

    ASSERT_EQ(snap.riskOrders.size(), 1u);
    EXPECT_DOUBLE_EQ(snap.riskOrders[0].takeProfitPrice, 150.0);
    EXPECT_DOUBLE_EQ(snap.riskOrders[0].stopLossPrice, 50.0);
    ASSERT_EQ(snap.runningAlgos.size(), 1u);
    EXPECT_EQ(snap.runningAlgos[0], (trading::RunningAlgo{"AvendellaMM", "BTC-USD"}));
}

TEST(TradingSnapshot, ClientStateAfterTheWireEqualsServerState) {
    trading::LiveTradingSession session([](const std::string&) { return 99.0; }, 0.0);
    tradeIntoState(session);
    auto server = session.snapshot();
    server.reason = "algo_command";
    server.commandId = "cmd-7";
    const auto client = overTheWire(server);

    EXPECT_EQ(client.epoch, server.epoch);
    EXPECT_EQ(client.reason, "algo_command");
    EXPECT_EQ(client.commandId, "cmd-7");
    ASSERT_EQ(client.positions.size(), server.positions.size());
    for (const auto& p : server.positions) {
        const auto* c = client.positionFor(p.symbol);
        ASSERT_NE(c, nullptr) << p.symbol;
        EXPECT_DOUBLE_EQ(c->positionQty, p.positionQty);
        EXPECT_DOUBLE_EQ(c->avgPrice, p.avgPrice);
        EXPECT_DOUBLE_EQ(c->unrealizedPnl, p.unrealizedPnl);
        EXPECT_DOUBLE_EQ(c->realizedPnl, p.realizedPnl);
    }
    EXPECT_TRUE(sameOrders(client.openOrders, server.openOrders));
    ASSERT_EQ(client.riskOrders.size(), 1u);
    EXPECT_TRUE(client.riskOrders[0].hasStopLoss);
    EXPECT_EQ(client.runningAlgos, server.runningAlgos);
}

TEST(TradingSnapshot, ParserRefusesAnotherSchemaOrMissingArrays) {
    trading::LiveTradingSession session([](const std::string&) { return 99.0; }, 0.0);
    auto json = trading::tradingSnapshotJson(session.snapshot());
    ASSERT_TRUE(trading::parseTradingSnapshotJson(json).has_value());
    auto other = json;
    other["schema_version"] = trading::TradingSnapshot::kSchemaVersion + 1;
    EXPECT_FALSE(trading::parseTradingSnapshotJson(other).has_value());
    auto missing = json;
    missing.erase("open_orders");
    EXPECT_FALSE(trading::parseTradingSnapshotJson(missing).has_value())
        << "a snapshot without its order list must not clear the client's orders";
}

// The server restarts: a fresh session holds nothing. The client's stale long, resting
// orders, TP/SL and running algo must all be gone once it applies the new snapshot.
TEST(TradingSnapshot, RestartedServerSnapshotIsEmptyWithANewEpoch) {
    trading::LiveTradingSession before([](const std::string&) { return 99.0; }, 0.0);
    tradeIntoState(before);
    const auto stale = overTheWire(before.snapshot());
    ASSERT_FALSE(stale.openOrders.empty());

    trading::LiveTradingSession after([](const std::string&) { return 99.0; }, 0.0);
    after.registerAlgo(std::make_unique<trading::AvendellaMM>());
    const auto fresh = overTheWire(after.snapshot());
    EXPECT_NE(fresh.epoch, stale.epoch) << "the client must be able to tell that order ids restarted";
    EXPECT_EQ(fresh.positionFor("BTC-USD"), nullptr);
    EXPECT_TRUE(fresh.openOrders.empty());
    EXPECT_TRUE(fresh.riskOrders.empty());
    EXPECT_TRUE(fresh.runningAlgos.empty());
}

// Item: "ghost quote lines remain after algo stop". Server stops the algo, the overlay
// applies exactly what the server broadcasts: no quote line stays open.
TEST(AlgoStopGhosts, StopBroadcastClosesEveryQuoteLine) {
    trading::LiveTradingSession session([](const std::string&) { return 100.0; }, 0.0);
    session.registerAlgo(std::make_unique<trading::AvendellaMM>());
    AlgoOrderSpans spans;
    session.setResultCallback([&spans](trading::TradingResult, std::vector<trading::AlgoOrderEvent> events) {
        for (const auto& ev : events) spans.apply(ev);
    });
    ASSERT_TRUE(session.startAlgo("AvendellaMM", "BTC-USD", quoteParams()));
    session.onTradeTick("BTC-USD", 100.0, 1000);
    ASSERT_EQ(spans.activeCount(), 2u);

    session.stopAlgo("AvendellaMM");
    EXPECT_EQ(spans.activeCount(), 0u) << "a stopped algo's quotes must not keep drawing";
    const auto snap = session.snapshot();
    EXPECT_TRUE(snap.runningAlgos.empty());
    EXPECT_TRUE(std::none_of(snap.openOrders.begin(), snap.openOrders.end(),
                             [](const auto& o) { return o.algoId == "AvendellaMM"; }));
}

// The overlay missed the cancellations (disconnected during the stop, or the server
// restarted): the next snapshot closes the lines.
TEST(AlgoStopGhosts, SnapshotClosesLinesTheServerNoLongerHolds) {
    trading::LiveTradingSession session([](const std::string&) { return 100.0; }, 0.0);
    session.registerAlgo(std::make_unique<trading::AvendellaMM>());
    AlgoOrderSpans spans;
    bool listening = true;
    session.setResultCallback([&](trading::TradingResult, std::vector<trading::AlgoOrderEvent> events) {
        if (listening) for (const auto& ev : events) spans.apply(ev);
    });
    ASSERT_TRUE(session.startAlgo("AvendellaMM", "BTC-USD", quoteParams()));
    session.onTradeTick("BTC-USD", 100.0, 1000);
    spans.reconcile(session.snapshot(), 1000);
    ASSERT_EQ(spans.activeCount(), 2u);

    listening = false;
    session.stopAlgo("AvendellaMM");
    ASSERT_EQ(spans.activeCount(), 2u) << "precondition: the overlay never saw the cancels";
    spans.reconcile(session.snapshot(), 5000);
    EXPECT_EQ(spans.activeCount(), 0u);
    for (const auto& [id, span] : spans.spans()) EXPECT_EQ(span.closedMs, 5000) << id;

    // Server restart: new epoch, ids restart at ord-1; old lines stay closed and keyed apart.
    trading::LiveTradingSession restarted([](const std::string&) { return 100.0; }, 0.0);
    restarted.registerAlgo(std::make_unique<trading::AvendellaMM>());
    ASSERT_TRUE(restarted.startAlgo("AvendellaMM", "BTC-USD", quoteParams()));
    restarted.onTradeTick("BTC-USD", 100.0, 6000);
    spans.reconcile(restarted.snapshot(), 6000);
    EXPECT_EQ(spans.activeCount(), 2u);
    EXPECT_EQ(spans.spans().size(), 4u) << "the old session's closed lines are kept, not overwritten";
}

TEST(AlgoStopGhosts, NewEpochClosesLinesStillOpen) {
    AlgoOrderSpans spans;
    trading::TradingSnapshot first;
    first.epoch = "a";
    spans.reconcile(first, 0);
    trading::AlgoOrderEvent open;
    open.algoId = "AvendellaMM";
    open.orderId = "ord-1";
    open.status = trading::OrderStatus::Open;
    open.price = 100.0;
    open.timestampMs = 10;
    spans.apply(open);
    ASSERT_EQ(spans.activeCount(), 1u);
    trading::TradingSnapshot restarted;
    restarted.epoch = "b";
    spans.reconcile(restarted, 20);
    EXPECT_EQ(spans.activeCount(), 0u);
}

TEST(OnAlgoStop, AskPromptsAlwaysFlattensNeverKeeps) {
    using paper::AlgoStopStep;
    using paper::OnAlgoStop;
    EXPECT_EQ(paper::stepAfterAlgoStop(OnAlgoStop::Ask), AlgoStopStep::Prompt);
    EXPECT_EQ(paper::stepAfterAlgoStop(OnAlgoStop::Always), AlgoStopStep::Flatten);
    EXPECT_EQ(paper::stepAfterAlgoStop(OnAlgoStop::Never), AlgoStopStep::Keep);

    EXPECT_TRUE(paper::flattenAfterAlgoStop(OnAlgoStop::Ask, true));
    EXPECT_FALSE(paper::flattenAfterAlgoStop(OnAlgoStop::Ask, false));
    EXPECT_TRUE(paper::flattenAfterAlgoStop(OnAlgoStop::Always, false)) << "Always flattens without asking";
    EXPECT_FALSE(paper::flattenAfterAlgoStop(OnAlgoStop::Never, true)) << "Never never flattens";
}

TEST(OnAlgoStop, SettingValuesRoundTripAndUnknownAsks) {
    using paper::OnAlgoStop;
    for (auto p : {OnAlgoStop::Ask, OnAlgoStop::Always, OnAlgoStop::Never})
        EXPECT_EQ(paper::onAlgoStopFromSetting(paper::settingValue(p)), p);
    EXPECT_EQ(paper::onAlgoStopFromSetting(""), OnAlgoStop::Ask) << "the default is Ask";
    EXPECT_EQ(paper::onAlgoStopFromSetting("ALWAYS"), OnAlgoStop::Ask);
    EXPECT_EQ(paper::onAlgoStopFromSetting("yes"), OnAlgoStop::Ask);
}
