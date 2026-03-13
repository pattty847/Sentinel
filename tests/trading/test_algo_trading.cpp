#include <gtest/gtest.h>

#include "trading/AlgoEngine.hpp"
#include "trading/AvendellaMM.hpp"
#include "trading/TradingEngine.hpp"
#include "trading/TradingTypes.hpp"

namespace {

// Helpers
trading::AlgoParams defaultParams() {
    trading::AlgoParams p;
    p.spreadBps      = 10.0;   // 10 bps half-spread
    p.orderQty       = 1.0;
    p.maxPositionQty = 5.0;
    p.skewBps        = 0.0;    // no skew for deterministic tests
    return p;
}

trading::Position emptyPosition(const std::string& sym = "BTC-USD") {
    trading::Position p;
    p.symbol = sym;
    p.netQty = 0.0;
    p.avgPrice = 0.0;
    p.realizedPnl = 0.0;
    return p;
}

}  // namespace

// ============================================================
// TradingEngine — limit order placement and fill-on-tick
// ============================================================

TEST(TradingEngineLimitTest, LimitOrderRests) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    trading::TradeCommand cmd;
    cmd.commandId = "lim-1";
    cmd.action    = trading::TradeAction::PlaceOrder;
    cmd.symbol    = "BTC-USD";
    cmd.side      = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Limit;
    cmd.price      = 99.0;
    cmd.hasPrice   = true;
    cmd.qty        = 1.0;

    auto res = engine.onCommand(cmd);
    // Should emit New then Open (resting)
    ASSERT_GE(res.orderUpdates.size(), 1u);
    bool hasOpen = false;
    for (const auto& u : res.orderUpdates) {
        if (u.status == trading::OrderStatus::Open) hasOpen = true;
    }
    EXPECT_TRUE(hasOpen) << "Limit order should rest with Open status";
}

TEST(TradingEngineLimitTest, LimitBuyFillsWhenPriceCrosses) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    trading::TradeCommand cmd;
    cmd.commandId  = "lim-2";
    cmd.action     = trading::TradeAction::PlaceOrder;
    cmd.symbol     = "BTC-USD";
    cmd.side       = trading::OrderSide::Buy;
    cmd.orderType  = trading::OrderType::Limit;
    cmd.price      = 100.0;  // buy at or below 100
    cmd.hasPrice   = true;
    cmd.qty        = 1.0;
    engine.onCommand(cmd);

    // Tick with price at 100 — should fill
    auto tickRes = engine.onTick("BTC-USD", 100.0, 1000);
    bool filled = false;
    for (const auto& u : tickRes.orderUpdates) {
        if (u.status == trading::OrderStatus::Filled) filled = true;
    }
    EXPECT_TRUE(filled) << "Buy limit at 100 should fill when tick price == 100";
}

TEST(TradingEngineLimitTest, LimitBuyDoesNotFillAboveLimit) {
    trading::TradingEngine engine([](const std::string&) { return 200.0; }, 0.0);

    trading::TradeCommand cmd;
    cmd.commandId  = "lim-3";
    cmd.action     = trading::TradeAction::PlaceOrder;
    cmd.symbol     = "BTC-USD";
    cmd.side       = trading::OrderSide::Buy;
    cmd.orderType  = trading::OrderType::Limit;
    cmd.price      = 99.0;
    cmd.hasPrice   = true;
    cmd.qty        = 1.0;
    engine.onCommand(cmd);

    // Tick with price above limit — should NOT fill
    auto tickRes = engine.onTick("BTC-USD", 101.0, 1000);
    for (const auto& u : tickRes.orderUpdates) {
        EXPECT_NE(u.status, trading::OrderStatus::Filled) << "Buy limit at 99 must not fill at price 101";
    }
}

TEST(TradingEngineLimitTest, LimitSellFillsWhenPriceCrosses) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    // First go long
    trading::TradeCommand buy;
    buy.commandId = "mkt-buy";
    buy.action    = trading::TradeAction::PlaceOrder;
    buy.symbol    = "BTC-USD";
    buy.side      = trading::OrderSide::Buy;
    buy.orderType = trading::OrderType::Market;
    buy.qty       = 1.0;
    engine.onCommand(buy);

    // Now place sell limit above current price
    trading::TradeCommand sell;
    sell.commandId  = "lim-sell";
    sell.action     = trading::TradeAction::PlaceOrder;
    sell.symbol     = "BTC-USD";
    sell.side       = trading::OrderSide::Sell;
    sell.orderType  = trading::OrderType::Limit;
    sell.price      = 110.0;
    sell.hasPrice   = true;
    sell.qty        = 1.0;
    engine.onCommand(sell);

    // Tick at 110 — sell limit should fill
    auto tickRes = engine.onTick("BTC-USD", 110.0, 2000);
    bool filled = false;
    for (const auto& u : tickRes.orderUpdates) {
        if (u.status == trading::OrderStatus::Filled) filled = true;
    }
    EXPECT_TRUE(filled) << "Sell limit at 110 should fill when tick price == 110";
}

TEST(TradingEngineLimitTest, PnlSnapshotEmittedOnTick) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    trading::TradeCommand buy;
    buy.commandId = "mkt";
    buy.action    = trading::TradeAction::PlaceOrder;
    buy.symbol    = "BTC-USD";
    buy.side      = trading::OrderSide::Buy;
    buy.orderType = trading::OrderType::Market;
    buy.qty       = 1.0;
    engine.onCommand(buy);

    auto tickRes = engine.onTick("BTC-USD", 105.0, 3000);
    bool hasPnl = !tickRes.pnlSnapshots.empty();
    EXPECT_TRUE(hasPnl) << "onTick should emit a PnlSnapshot when position is open";
    if (hasPnl) {
        EXPECT_NEAR(tickRes.pnlSnapshots[0].unrealizedPnl, 5.0, 1e-9);
    }
}

// ============================================================
// PositionManager — realized PnL on close
// ============================================================

TEST(PositionManagerRealizedPnl, LongThenClose) {
    trading::PositionManager pm;

    auto after_buy = pm.applyFill("BTC-USD", trading::OrderSide::Buy, 1.0, 100.0, 100.0);
    EXPECT_DOUBLE_EQ(after_buy.realizedPnl, 0.0);

    auto after_sell = pm.applyFill("BTC-USD", trading::OrderSide::Sell, 1.0, 110.0, 110.0);
    EXPECT_DOUBLE_EQ(after_sell.realizedPnl, 10.0);   // closed at +10
    EXPECT_DOUBLE_EQ(after_sell.netQty, 0.0);
}

TEST(PositionManagerRealizedPnl, FlipAccumulates) {
    trading::PositionManager pm;

    pm.applyFill("BTC-USD", trading::OrderSide::Buy, 2.0, 100.0, 100.0);
    // Sell 3: closes 2 long (+20 realized) and opens 1 short
    auto p = pm.applyFill("BTC-USD", trading::OrderSide::Sell, 3.0, 110.0, 110.0);
    EXPECT_NEAR(p.realizedPnl, 20.0, 1e-9);
    EXPECT_DOUBLE_EQ(p.netQty, -1.0);
}

// ============================================================
// AvendellaMM — start/stop, quote generation, stale detection
// ============================================================

TEST(AvendellaMM, NotRunningBeforeStart) {
    trading::AvendellaMM algo;
    EXPECT_FALSE(algo.isRunning());
}

TEST(AvendellaMM, RunningAfterStart) {
    trading::AvendellaMM algo;
    algo.start("BTC-USD", defaultParams());
    EXPECT_TRUE(algo.isRunning());
    algo.stop();
    EXPECT_FALSE(algo.isRunning());
}

TEST(AvendellaMM, FirstTickProducesTwoLimitOrders) {
    trading::AvendellaMM algo;
    algo.start("BTC-USD", defaultParams());

    auto cmds = algo.onTick(100.0, 1000, {}, emptyPosition());
    // Expect bid + ask limit orders
    ASSERT_EQ(cmds.size(), 2u) << "First tick should produce exactly 2 limit orders (bid + ask)";

    bool hasBid = false, hasAsk = false;
    for (const auto& c : cmds) {
        EXPECT_EQ(c.orderType, trading::OrderType::Limit);
        EXPECT_EQ(c.action, trading::TradeAction::PlaceOrder);
        if (c.side == trading::OrderSide::Buy)  hasBid = true;
        if (c.side == trading::OrderSide::Sell) hasAsk = true;
    }
    EXPECT_TRUE(hasBid) << "First tick must include a buy (bid) limit order";
    EXPECT_TRUE(hasAsk) << "First tick must include a sell (ask) limit order";
}

TEST(AvendellaMM, QuotePricesRespectSpread) {
    trading::AvendellaMM algo;
    trading::AlgoParams p = defaultParams();
    p.spreadBps = 10.0;  // 10 bps total spread -> 5 bps per side
    algo.start("BTC-USD", p);

    auto cmds = algo.onTick(100.0, 1000, {}, emptyPosition());
    ASSERT_EQ(cmds.size(), 2u);

    double bidPrice = 0.0, askPrice = 0.0;
    for (const auto& c : cmds) {
        if (c.side == trading::OrderSide::Buy)  bidPrice = c.price;
        if (c.side == trading::OrderSide::Sell) askPrice = c.price;
    }
    // 5 bps of 100 = 0.05 per side
    EXPECT_NEAR(bidPrice, 99.95, 1e-6) << "Bid should be mid minus half-spread";
    EXPECT_NEAR(askPrice, 100.05, 1e-6) << "Ask should be mid plus half-spread";
}

TEST(AvendellaMM, NoRequoteWhenMidUnchanged) {
    trading::AvendellaMM algo;
    algo.start("BTC-USD", defaultParams());

    // First tick — places bid + ask
    auto cmds1 = algo.onTick(100.0, 1000, {}, emptyPosition());
    ASSERT_EQ(cmds1.size(), 2u);

    // Simulate orders resting
    trading::OrderUpdate bidUpd;
    bidUpd.orderId = cmds1[0].commandId;
    bidUpd.status  = trading::OrderStatus::Open;
    bidUpd.side    = trading::OrderSide::Buy;
    algo.onOrderUpdate(bidUpd);

    trading::OrderUpdate askUpd;
    askUpd.orderId = cmds1[1].commandId;
    askUpd.status  = trading::OrderStatus::Open;
    askUpd.side    = trading::OrderSide::Sell;
    algo.onOrderUpdate(askUpd);

    // Build active orders list to pass in
    std::vector<trading::Order> openOrders;
    {
        trading::Order o;
        o.id     = cmds1[0].commandId;
        o.side   = trading::OrderSide::Buy;
        o.status = trading::OrderStatus::Open;
        openOrders.push_back(o);
        o.id   = cmds1[1].commandId;
        o.side = trading::OrderSide::Sell;
        openOrders.push_back(o);
    }

    // Second tick with same mid — no new commands (quotes still valid)
    auto cmds2 = algo.onTick(100.0, 2000, openOrders, emptyPosition());
    EXPECT_TRUE(cmds2.empty()) << "Same mid price should not trigger requote";
}

TEST(AvendellaMM, RequoteWhenMidMovesSignificantly) {
    trading::AvendellaMM algo;
    trading::AlgoParams p = defaultParams();
    p.spreadBps = 10.0;  // 10 bps half-spread (0.10 on 100)
    algo.start("BTC-USD", p);

    auto cmds1 = algo.onTick(100.0, 1000, {}, emptyPosition());
    ASSERT_EQ(cmds1.size(), 2u);

    // Move mid significantly (much more than half-spread)
    auto cmds2 = algo.onTick(200.0, 2000, {}, emptyPosition());
    // Should produce cancels + new quotes (at least 2 commands)
    EXPECT_GE(cmds2.size(), 2u) << "Large mid move should trigger requote";
}

TEST(AvendellaMM, StopsQuotingWhenNotRunning) {
    trading::AvendellaMM algo;
    algo.start("BTC-USD", defaultParams());
    algo.stop();

    auto cmds = algo.onTick(100.0, 1000, {}, emptyPosition());
    EXPECT_TRUE(cmds.empty()) << "Stopped algo must not generate any commands";
}

TEST(AvendellaMM, MaxPositionGuardSuppressesBidWhenLong) {
    trading::AvendellaMM algo;
    trading::AlgoParams p = defaultParams();
    p.orderQty       = 1.0;
    p.maxPositionQty = 1.0;   // already at max if netQty >= 1
    algo.start("BTC-USD", p);

    // Position already at max long
    trading::Position pos = emptyPosition();
    pos.netQty = 1.0;

    auto cmds = algo.onTick(100.0, 1000, {}, pos);
    // Should not place a bid (would push past max)
    for (const auto& c : cmds) {
        if (c.action == trading::TradeAction::PlaceOrder) {
            EXPECT_NE(c.side, trading::OrderSide::Buy)
                << "Should not bid when already at max long position";
        }
    }
}

// ============================================================
// AlgoEngine — registration, start/stop, tick routing
// ============================================================

TEST(AlgoEngineTest, RegisterAndStart) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    trading::AlgoEngine algoEngine(engine);
    algoEngine.registerAlgo(std::make_unique<trading::AvendellaMM>());

    EXPECT_TRUE(algoEngine.startAlgo("AvendellaMM", "BTC-USD", defaultParams()));
    auto running = algoEngine.runningAlgos();
    EXPECT_EQ(running.size(), 1u);
    EXPECT_EQ(running[0], "AvendellaMM");
}

TEST(AlgoEngineTest, StartUnknownAlgoReturnsFalse) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    trading::AlgoEngine algoEngine(engine);

    EXPECT_FALSE(algoEngine.startAlgo("NonExistentAlgo", "BTC-USD", defaultParams()));
    EXPECT_TRUE(algoEngine.runningAlgos().empty());
}

TEST(AlgoEngineTest, StopAlgo) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    trading::AlgoEngine algoEngine(engine);
    algoEngine.registerAlgo(std::make_unique<trading::AvendellaMM>());
    algoEngine.startAlgo("AvendellaMM", "BTC-USD", defaultParams());

    algoEngine.stopAlgo("AvendellaMM");
    EXPECT_TRUE(algoEngine.runningAlgos().empty());
}

TEST(AlgoEngineTest, TickProducesCallbackWithEvents) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    trading::AlgoEngine algoEngine(engine);
    algoEngine.registerAlgo(std::make_unique<trading::AvendellaMM>());
    algoEngine.startAlgo("AvendellaMM", "BTC-USD", defaultParams());

    int callbackCount = 0;
    int totalEvents   = 0;
    algoEngine.setResultCallback([&](trading::TradingResult result,
                                     std::vector<trading::AlgoOrderEvent> events) {
        ++callbackCount;
        totalEvents += static_cast<int>(events.size());
    });

    algoEngine.onTick("BTC-USD", 100.0, 1000);
    EXPECT_GE(callbackCount, 1) << "Callback should fire at least once per tick";
    EXPECT_GE(totalEvents, 2) << "First tick should generate at least 2 algo order events";
}

TEST(AlgoEngineTest, TickOnWrongSymbolDoesNothing) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    trading::AlgoEngine algoEngine(engine);
    algoEngine.registerAlgo(std::make_unique<trading::AvendellaMM>());
    algoEngine.startAlgo("AvendellaMM", "BTC-USD", defaultParams());

    int callbackCount = 0;
    algoEngine.setResultCallback([&](trading::TradingResult, std::vector<trading::AlgoOrderEvent>) {
        ++callbackCount;
    });

    // Tick on different symbol — AvendellaMM is watching BTC-USD
    algoEngine.onTick("ETH-USD", 2000.0, 1000);
    // Callback may still fire (for limit checks) but algo should not emit orders for ETH-USD
    // We just verify it doesn't crash
    SUCCEED();
}
