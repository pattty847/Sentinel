#include <gtest/gtest.h>

#include "trading/TradingEngine.hpp"
#include "trading/OrderStore.hpp"
#include "trading/PositionManager.hpp"
#include "ConfigLoader.hpp"

#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <limits>

namespace {
trading::TradeCommand marketBuy(double qty, const std::string& symbol = "BTC-USD") {
    trading::TradeCommand cmd;
    cmd.commandId = "cid";
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = symbol;
    cmd.side = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Market;
    cmd.qty = qty;
    return cmd;
}

trading::TradeCommand limitBuy(double qty = 2.0, double price = 99.0) {
    auto cmd = marketBuy(qty);
    cmd.orderType = trading::OrderType::Limit;
    cmd.hasPrice = true;
    cmd.price = price;
    return cmd;
}

trading::TradeCommand attachedRisk(bool takeProfit, double price) {
    trading::TradeCommand cmd;
    cmd.action = trading::TradeAction::SetAttachedRisk;
    cmd.symbol = "BTC-USD";
    cmd.hasTakeProfit = takeProfit;
    cmd.takeProfitPrice = takeProfit ? price : 0.0;
    cmd.hasStopLoss = !takeProfit;
    cmd.stopLossPrice = takeProfit ? 0.0 : price;
    return cmd;
}

constexpr std::array<double, 3> nonFiniteValues{
    std::numeric_limits<double>::quiet_NaN(),
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(),
};

void expectNoUpdates(const trading::TradingResult& result) {
    EXPECT_TRUE(result.orderUpdates.empty());
    EXPECT_TRUE(result.positionUpdates.empty());
    EXPECT_TRUE(result.pnlSnapshots.empty());
    EXPECT_TRUE(result.riskOrderUpdates.empty());
}

QStringList* g_warnings = nullptr;

void captureWarnings(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg && g_warnings) g_warnings->push_back(message);
}
}

TEST(OrderStoreTest, DuplicateOrderIdUpserts) {
    trading::OrderStore store;
    trading::Order o;
    o.id = "ord-1";
    o.symbol = "BTC-USD";
    o.qty = 1.0;
    o.status = trading::OrderStatus::New;
    store.upsert(o);

    o.status = trading::OrderStatus::Filled;
    o.filledQty = 1.0;
    store.upsert(o);

    auto loaded = store.get("ord-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, trading::OrderStatus::Filled);
    EXPECT_DOUBLE_EQ(loaded->filledQty, 1.0);
}

TEST(PositionManagerTest, OpenIncreaseReduceCloseFlip) {
    trading::PositionManager pm;

    auto p1 = pm.applyFill("BTC-USD", trading::OrderSide::Buy, 1.0, 100.0, 100.0);
    EXPECT_DOUBLE_EQ(p1.netQty, 1.0);
    EXPECT_DOUBLE_EQ(p1.avgPrice, 100.0);

    auto p2 = pm.applyFill("BTC-USD", trading::OrderSide::Buy, 1.0, 110.0, 110.0);
    EXPECT_DOUBLE_EQ(p2.netQty, 2.0);
    EXPECT_DOUBLE_EQ(p2.avgPrice, 105.0);

    auto p3 = pm.applyFill("BTC-USD", trading::OrderSide::Sell, 1.0, 120.0, 120.0);
    EXPECT_DOUBLE_EQ(p3.netQty, 1.0);
    EXPECT_DOUBLE_EQ(p3.avgPrice, 105.0);

    auto p4 = pm.applyFill("BTC-USD", trading::OrderSide::Sell, 1.0, 90.0, 90.0);
    EXPECT_DOUBLE_EQ(p4.netQty, 0.0);
    EXPECT_DOUBLE_EQ(p4.avgPrice, 0.0);

    auto p5 = pm.applyFill("BTC-USD", trading::OrderSide::Sell, 2.0, 80.0, 80.0);
    EXPECT_DOUBLE_EQ(p5.netQty, -2.0);
    EXPECT_DOUBLE_EQ(p5.avgPrice, 80.0);
}

TEST(TradingEngineTest, PlaceLifecycleAndInvalidQuantity) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    auto invalid = marketBuy(0.0);
    auto invalidRes = engine.onCommand(invalid);
    EXPECT_TRUE(invalidRes.orderUpdates.empty());

    auto res = engine.onCommand(marketBuy(2.0));
    ASSERT_EQ(res.orderUpdates.size(), 2u);
    EXPECT_EQ(res.orderUpdates[0].status, trading::OrderStatus::New);
    EXPECT_EQ(res.orderUpdates[1].status, trading::OrderStatus::Filled);
    EXPECT_DOUBLE_EQ(res.orderUpdates[1].filledQty, 2.0);
}

TEST(TradingEngineTest, EmptyBookMidRejectsMarketFill) {
    trading::TradingEngine engine([](const std::string&) { return 0.0; }, 0.0);
    const auto result = engine.onCommand(marketBuy(1.0));
    EXPECT_TRUE(result.orderUpdates.empty());
}

TEST(TradingEngineTest, PartialAndFullFillsAndOutOfOrderProtection) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    trading::TradeCommand cmd;
    cmd.commandId = "cid-limit";
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = "BTC-USD";
    cmd.side = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Limit;
    cmd.hasPrice = true;
    cmd.price = 99.0;
    cmd.qty = 10.0;

    auto placed = engine.onCommand(cmd);
    ASSERT_EQ(placed.orderUpdates.size(), 1u);
    const std::string orderId = placed.orderUpdates[0].orderId;

    auto part = engine.onExternalFill(orderId, 4.0, 100.0);
    ASSERT_EQ(part.orderUpdates.size(), 1u);
    EXPECT_EQ(part.orderUpdates[0].status, trading::OrderStatus::Partial);
    EXPECT_DOUBLE_EQ(part.orderUpdates[0].filledQty, 4.0);

    auto full = engine.onExternalFill(orderId, 10.0, 105.0);
    ASSERT_EQ(full.orderUpdates.size(), 1u);
    EXPECT_EQ(full.orderUpdates[0].status, trading::OrderStatus::Filled);
    EXPECT_DOUBLE_EQ(full.orderUpdates[0].avgPrice, 103.0);

    auto stale = engine.onExternalFill(orderId, 8.0, 106.0);
    EXPECT_TRUE(stale.orderUpdates.empty());
}

TEST(TradingEngineTest, RejectsNonFiniteOrderQuantityAndLimitPrice) {
    for (double bad : nonFiniteValues) {
        for (int field = 0; field < 3; ++field) {
            trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
            const auto command = field == 0 ? marketBuy(bad)
                : field == 1 ? limitBuy(bad) : limitBuy(2.0, bad);
            expectNoUpdates(engine.onCommand(command));
            EXPECT_TRUE(engine.getOpenOrders("BTC-USD").empty());
            EXPECT_FALSE(engine.getPosition("BTC-USD").has_value());
            EXPECT_FALSE(engine.findOrder("ord-1").has_value());
        }
    }
}

TEST(TradingEngineTest, RejectsNonFiniteMarketMarkAndFillPrice) {
    for (double bad : nonFiniteValues) {
        double mark = bad;
        trading::TradingEngine engine([&](const std::string&) { return mark; }, 0.0);
        expectNoUpdates(engine.onCommand(marketBuy(1.0)));
        EXPECT_FALSE(engine.findOrder("ord-1").has_value());
        EXPECT_FALSE(engine.getPosition("BTC-USD").has_value());
    }
    trading::TradingEngine overflow([](const std::string&) { return std::numeric_limits<double>::max(); }, 10000.0);
    expectNoUpdates(overflow.onCommand(marketBuy(1.0)));
    EXPECT_FALSE(overflow.findOrder("ord-1").has_value());
}

TEST(TradingEngineTest, RejectsNonFiniteExternalFillQuantityAndPrice) {
    for (double bad : nonFiniteValues) {
        for (bool badQuantity : {true, false}) {
            trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
            const auto placed = engine.onCommand(limitBuy());
            ASSERT_EQ(placed.orderUpdates.size(), 1u);
            const auto id = placed.orderUpdates.front().orderId;
            expectNoUpdates(engine.onExternalFill(id, badQuantity ? bad : 1.0,
                                                  badQuantity ? 100.0 : bad));
            const auto order = engine.findOrder(id);
            ASSERT_TRUE(order.has_value());
            EXPECT_EQ(order->status, trading::OrderStatus::Open);
            EXPECT_DOUBLE_EQ(order->filledQty, 0.0);
            EXPECT_EQ(engine.getOpenOrders("BTC-USD").size(), 1u);
            EXPECT_FALSE(engine.getPosition("BTC-USD").has_value());
        }
    }
}

TEST(TradingEngineTest, RejectsNonFiniteTickWithoutChangingPositionOrOrder) {
    for (double bad : nonFiniteValues) {
        trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
        engine.onCommand(marketBuy(1.0));
        const auto placed = engine.onCommand(limitBuy());
        ASSERT_EQ(placed.orderUpdates.size(), 1u);
        const auto before = engine.getPosition("BTC-USD");
        ASSERT_TRUE(before.has_value());
        expectNoUpdates(engine.onTick("BTC-USD", bad, 1234));
        const auto after = engine.getPosition("BTC-USD");
        ASSERT_TRUE(after.has_value());
        EXPECT_DOUBLE_EQ(after->netQty, before->netQty);
        EXPECT_DOUBLE_EQ(after->avgPrice, before->avgPrice);
        EXPECT_DOUBLE_EQ(after->unrealizedPnl, before->unrealizedPnl);
        EXPECT_DOUBLE_EQ(after->realizedPnl, before->realizedPnl);
        EXPECT_EQ(engine.getOpenOrders("BTC-USD").size(), 1u);
    }
}

TEST(TradingEngineTest, RejectsNonFiniteAttachedRiskPrices) {
    for (double bad : nonFiniteValues) {
        for (bool takeProfit : {true, false}) {
            trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
            engine.onCommand(marketBuy(1.0));
            trading::TradeCommand risk;
            risk.action = trading::TradeAction::SetAttachedRisk;
            risk.symbol = "BTC-USD";
            risk.hasTakeProfit = takeProfit;
            risk.takeProfitPrice = takeProfit ? bad : 0.0;
            risk.hasStopLoss = !takeProfit;
            risk.stopLossPrice = takeProfit ? 0.0 : bad;
            expectNoUpdates(engine.onCommand(risk));
            const auto tick = engine.onTick("BTC-USD", 105.0, 1234);
            EXPECT_TRUE(tick.orderUpdates.empty());
            EXPECT_TRUE(tick.riskOrderUpdates.empty());
            const auto position = engine.getPosition("BTC-USD");
            ASSERT_TRUE(position.has_value());
            EXPECT_DOUBLE_EQ(position->netQty, 1.0);
        }
    }
}

TEST(TradingEngineTest, CancelQueriesFlattenAndUnknownFill) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    auto first = limitBuy();
    first.algoId = "a";
    const auto one = engine.onCommand(first).orderUpdates.front().orderId;
    auto second = limitBuy();
    second.algoId = "b";
    const auto two = engine.onCommand(second).orderUpdates.front().orderId;
    EXPECT_EQ(engine.getOpenOrders("BTC-USD", "a").size(), 1u);
    EXPECT_EQ(engine.getOpenOrders("BTC-USD", "b").size(), 1u);
    EXPECT_EQ(engine.getOpenOrders("BTC-USD").size(), 2u);
    EXPECT_FALSE(engine.findOrder("missing").has_value());
    expectNoUpdates(engine.onExternalFill("missing", 1.0, 100.0));

    trading::TradeCommand cancel;
    cancel.action = trading::TradeAction::CancelOrder;
    cancel.targetOrderId = one;
    const auto canceled = engine.onCommand(cancel);
    ASSERT_EQ(canceled.orderUpdates.size(), 1u);
    EXPECT_EQ(canceled.orderUpdates.front().status, trading::OrderStatus::Canceled);
    EXPECT_EQ(engine.findOrder(one)->status, trading::OrderStatus::Canceled);
    expectNoUpdates(engine.onCommand(cancel));

    trading::TradeCommand cancelAll;
    cancelAll.action = trading::TradeAction::CancelAll;
    const auto all = engine.onCommand(cancelAll);
    ASSERT_EQ(all.orderUpdates.size(), 1u);
    EXPECT_EQ(all.orderUpdates.front().orderId, two);
    EXPECT_TRUE(engine.getOpenOrders("BTC-USD").empty());

    engine.onCommand(marketBuy(2.0));
    trading::TradeCommand flatten;
    flatten.action = trading::TradeAction::Flatten;
    flatten.symbol = "BTC-USD";
    const auto flat = engine.onCommand(flatten);
    ASSERT_EQ(flat.positionUpdates.size(), 1u);
    EXPECT_DOUBLE_EQ(flat.positionUpdates.front().positionQty, 0.0);
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 0.0);
    expectNoUpdates(engine.onCommand(flatten));
}

TEST(TradingEngineTest, AttachedRiskTriggersExitAndCancelAllClearsIt) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    engine.onCommand(marketBuy(1.0));
    trading::TradeCommand risk;
    risk.action = trading::TradeAction::SetAttachedRisk;
    risk.symbol = "BTC-USD";
    risk.hasTakeProfit = true;
    risk.takeProfitPrice = 110.0;
    const auto attached = engine.onCommand(risk);
    ASSERT_EQ(attached.riskOrderUpdates.size(), 1u);
    EXPECT_TRUE(attached.riskOrderUpdates.front().hasTakeProfit);
    const auto exit = engine.onTick("BTC-USD", 111.0, 1234);
    ASSERT_EQ(exit.orderUpdates.size(), 2u);
    EXPECT_EQ(exit.orderUpdates.back().status, trading::OrderStatus::Filled);
    ASSERT_TRUE(engine.getPosition("BTC-USD").has_value());
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 0.0);
    ASSERT_EQ(exit.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(exit.riskOrderUpdates.front().hasTakeProfit);

    engine.onCommand(marketBuy(1.0));
    engine.onCommand(risk);
    trading::TradeCommand cancelAll;
    cancelAll.action = trading::TradeAction::CancelAll;
    const auto cleared = engine.onCommand(cancelAll);
    ASSERT_EQ(cleared.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(cleared.riskOrderUpdates.front().hasTakeProfit);
    EXPECT_TRUE(engine.onTick("BTC-USD", 111.0, 1235).orderUpdates.empty());
}

TEST(TradingEngineTest, AlgoFlipShortToLongClearsManualRiskBeforeNextTick) {
    double mark = 100.0;
    trading::TradingEngine engine([&](const std::string&) { return mark; }, 0.0);
    auto shortOrder = marketBuy(1.0);
    shortOrder.side = trading::OrderSide::Sell;
    engine.onCommand(shortOrder);
    auto risk = attachedRisk(true, 95.0);
    risk.hasStopLoss = true;
    risk.stopLossPrice = 105.0;
    ASSERT_EQ(engine.onCommand(risk).riskOrderUpdates.size(), 1u);

    mark = 101.0;
    auto algoFlip = marketBuy(2.0);
    algoFlip.algoId = "algo";
    const auto fill = engine.onCommand(algoFlip);
    ASSERT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 1.0);
    ASSERT_EQ(fill.riskOrderUpdates.size(), 1u);
    EXPECT_EQ(fill.riskOrderUpdates[0].symbol, "BTC-USD");
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasTakeProfit);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasStopLoss);
    EXPECT_TRUE(engine.getAllAttachedRisk().empty());
    const auto tick = engine.onTick("BTC-USD", 101.0, 1234);
    EXPECT_TRUE(tick.orderUpdates.empty());
    EXPECT_TRUE(tick.riskOrderUpdates.empty());
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 1.0);
}

TEST(TradingEngineTest, AlgoFlipLongToShortClearsManualRiskBeforeNextTick) {
    double mark = 100.0;
    trading::TradingEngine engine([&](const std::string&) { return mark; }, 0.0);
    engine.onCommand(marketBuy(1.0));
    auto risk = attachedRisk(true, 105.0);
    risk.hasStopLoss = true;
    risk.stopLossPrice = 95.0;
    ASSERT_EQ(engine.onCommand(risk).riskOrderUpdates.size(), 1u);

    mark = 99.0;
    auto algoFlip = marketBuy(2.0);
    algoFlip.side = trading::OrderSide::Sell;
    algoFlip.algoId = "algo";
    const auto fill = engine.onCommand(algoFlip);
    ASSERT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, -1.0);
    ASSERT_EQ(fill.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasTakeProfit);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasStopLoss);
    EXPECT_TRUE(engine.getAllAttachedRisk().empty());
    const auto tick = engine.onTick("BTC-USD", 99.0, 1234);
    EXPECT_TRUE(tick.orderUpdates.empty());
    EXPECT_TRUE(tick.riskOrderUpdates.empty());
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, -1.0);
}

TEST(TradingEngineTest, AlgoCloseClearsRiskAndSameSideFillsKeepIt) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    engine.onCommand(marketBuy(2.0));
    auto risk = attachedRisk(true, 110.0);
    risk.hasStopLoss = true;
    risk.stopLossPrice = 90.0;
    ASSERT_EQ(engine.onCommand(risk).riskOrderUpdates.size(), 1u);

    auto algoAdd = marketBuy(1.0);
    algoAdd.algoId = "algo";
    EXPECT_TRUE(engine.onCommand(algoAdd).riskOrderUpdates.empty());
    auto algoReduce = marketBuy(2.0);
    algoReduce.side = trading::OrderSide::Sell;
    algoReduce.algoId = "algo";
    EXPECT_TRUE(engine.onCommand(algoReduce).riskOrderUpdates.empty());
    ASSERT_EQ(engine.getAllAttachedRisk().size(), 1u);
    EXPECT_TRUE(engine.getAllAttachedRisk()[0].hasTakeProfit);
    EXPECT_TRUE(engine.getAllAttachedRisk()[0].hasStopLoss);

    auto algoClose = marketBuy(1.0);
    algoClose.side = trading::OrderSide::Sell;
    algoClose.algoId = "algo";
    const auto fill = engine.onCommand(algoClose);
    ASSERT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 0.0);
    ASSERT_EQ(fill.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasTakeProfit);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasStopLoss);
    EXPECT_TRUE(engine.getAllAttachedRisk().empty());
}

TEST(TradingEngineTest, ExternalAlgoFillClearsRiskOnFlip) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    engine.onCommand(marketBuy(1.0));
    ASSERT_EQ(engine.onCommand(attachedRisk(true, 110.0)).riskOrderUpdates.size(), 1u);
    auto algoSell = marketBuy(2.0);
    algoSell.side = trading::OrderSide::Sell;
    algoSell.orderType = trading::OrderType::Limit;
    algoSell.hasPrice = true;
    algoSell.price = 101.0;
    algoSell.algoId = "algo";
    const auto placed = engine.onCommand(algoSell);
    ASSERT_EQ(placed.orderUpdates.size(), 1u);
    const auto fill = engine.onExternalFill(placed.orderUpdates[0].orderId, 2.0, 101.0);
    ASSERT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, -1.0);
    ASSERT_EQ(fill.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasTakeProfit);
    EXPECT_TRUE(engine.getAllAttachedRisk().empty());
}

TEST(TradingEngineTest, RestingAlgoLimitFlipPublishesRiskClear) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    auto shortOrder = marketBuy(1.0);
    shortOrder.side = trading::OrderSide::Sell;
    engine.onCommand(shortOrder);
    ASSERT_EQ(engine.onCommand(attachedRisk(true, 95.0)).riskOrderUpdates.size(), 1u);
    auto algoBuy = limitBuy(2.0, 101.0);
    algoBuy.algoId = "algo";
    engine.onCommand(algoBuy);

    const auto fill = engine.onTick("BTC-USD", 101.0, 1234);
    ASSERT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 1.0);
    ASSERT_EQ(fill.riskOrderUpdates.size(), 1u);
    EXPECT_FALSE(fill.riskOrderUpdates[0].hasTakeProfit);
    EXPECT_TRUE(engine.getAllAttachedRisk().empty());
    EXPECT_TRUE(engine.onTick("BTC-USD", 101.0, 1235).orderUpdates.empty());
}

TEST(TradingEngineTest, InvalidMarketStopLossExitKeepsOrderPositionAndRiskIntact) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 10000.0);
    engine.onCommand(marketBuy(1.0)); // Buy fills at 200 with 100% slippage.
    ASSERT_EQ(engine.onCommand(attachedRisk(false, 150.0)).riskOrderUpdates.size(), 1u);

    QStringList warnings;
    g_warnings = &warnings;
    const auto previous = qInstallMessageHandler(captureWarnings);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); g_warnings = nullptr; });

    const auto rejectedExit = engine.onTick("BTC-USD", 140.0, 1234); // Sell fill would be zero.
    EXPECT_EQ(rejectedExit.rejectReason, "invalid_fill_price");
    EXPECT_TRUE(rejectedExit.orderUpdates.empty());
    EXPECT_TRUE(rejectedExit.riskOrderUpdates.empty());
    EXPECT_TRUE(engine.getOpenOrders("BTC-USD").empty());
    EXPECT_FALSE(engine.findOrder("ord-2").has_value());
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 1.0);
    ASSERT_EQ(engine.getAllAttachedRisk().size(), 1u);
    EXPECT_TRUE(engine.getAllAttachedRisk().front().hasStopLoss);

    for (int i = 1; i <= 20; ++i) {
        const auto repeated = engine.onTick("BTC-USD", 140.0 - i, 1234 + i);
        EXPECT_TRUE(repeated.rejectReason.empty());
        EXPECT_TRUE(repeated.orderUpdates.empty());
        EXPECT_TRUE(repeated.riskOrderUpdates.empty());
    }
    EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const QString& warning) {
        return warning.contains("paper risk exit rejected:");
    }), 1);

    engine.onCommand(attachedRisk(false, 150.0)); // Repeating the same settings does not re-arm.
    EXPECT_TRUE(engine.onTick("BTC-USD", 140.0, 1236).rejectReason.empty());
    engine.onCommand(attachedRisk(false, 160.0));
    const auto changedRisk = engine.onTick("BTC-USD", 140.0, 1237);
    EXPECT_EQ(changedRisk.rejectReason, "invalid_fill_price");
    EXPECT_TRUE(changedRisk.orderUpdates.empty());
}

TEST(TradingEngineTest, NonFiniteMarketStopLossFillDoesNotCreateExitOrder) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; },
                                  std::numeric_limits<double>::infinity());
    const auto placed = engine.onCommand(limitBuy(1.0, 99.0));
    ASSERT_EQ(placed.orderUpdates.size(), 1u);
    engine.onExternalFill(placed.orderUpdates.front().orderId, 1.0, 100.0);
    engine.onCommand(attachedRisk(false, 90.0));

    const auto rejectedExit = engine.onTick("BTC-USD", 89.0, 1234);
    EXPECT_EQ(rejectedExit.rejectReason, "invalid_fill_price");
    EXPECT_TRUE(rejectedExit.orderUpdates.empty());
    EXPECT_FALSE(engine.findOrder("ord-2").has_value());
    EXPECT_DOUBLE_EQ(engine.getPosition("BTC-USD")->netQty, 1.0);
    ASSERT_EQ(engine.getAllAttachedRisk().size(), 1u);
    EXPECT_TRUE(engine.getAllAttachedRisk().front().hasStopLoss);
}

TEST(TradingEngineTest, ValidTakeProfitAndStopLossKeepNewThenFilledSequence) {
    for (const bool takeProfit : {true, false}) {
        trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
        engine.onCommand(marketBuy(1.0));
        engine.onCommand(attachedRisk(takeProfit, takeProfit ? 110.0 : 90.0));

        const auto exit = engine.onTick("BTC-USD", takeProfit ? 111.0 : 89.0, 1234);
        EXPECT_TRUE(exit.rejectReason.empty());
        ASSERT_EQ(exit.orderUpdates.size(), 2u);
        EXPECT_EQ(exit.orderUpdates[0].status, trading::OrderStatus::New);
        EXPECT_EQ(exit.orderUpdates[1].status, trading::OrderStatus::Filled);
        EXPECT_EQ(exit.orderUpdates[0].orderId, exit.orderUpdates[1].orderId);
        EXPECT_EQ(exit.orderUpdates[0].orderId, "ord-2");
        EXPECT_EQ(exit.orderUpdates[0].side, trading::OrderSide::Sell);
        EXPECT_DOUBLE_EQ(exit.orderUpdates[0].qty, 1.0);
        EXPECT_DOUBLE_EQ(exit.orderUpdates[0].limitPrice, takeProfit ? 110.0 : 0.0);
        EXPECT_DOUBLE_EQ(exit.orderUpdates[1].avgPrice, takeProfit ? 110.0 : 89.0);
        ASSERT_EQ(exit.positionUpdates.size(), 2u);
        EXPECT_DOUBLE_EQ(exit.positionUpdates[0].positionQty, 1.0);
        EXPECT_DOUBLE_EQ(exit.positionUpdates[1].positionQty, 0.0);
        ASSERT_EQ(exit.pnlSnapshots.size(), 2u);
        ASSERT_EQ(exit.riskOrderUpdates.size(), 1u);
        EXPECT_FALSE(exit.riskOrderUpdates[0].hasTakeProfit);
        EXPECT_FALSE(exit.riskOrderUpdates[0].hasStopLoss);
        EXPECT_EQ(engine.findOrder("ord-2")->status, trading::OrderStatus::Filled);
    }
}

TEST(TradingConfigTest, SlippageOutsideExecutionRangeUsesDefault) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("server.yaml");
    const auto load = [&](const QByteArray& value) {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("server:\n  trading:\n    slippage_bps: " + value + "\n");
        file.close();
        ServerConfig config;
        config.trading.slippageBps = 7.0;
        EXPECT_TRUE(ConfigLoader::loadServerConfig(path.toStdString(), &config));
        return config.trading.slippageBps;
    };

    QStringList warnings;
    g_warnings = &warnings;
    const auto previous = qInstallMessageHandler(captureWarnings);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); g_warnings = nullptr; });
    for (const QByteArray bad : {"-1", "10000", "10001", ".inf", ".nan"}) {
        warnings.clear();
        EXPECT_DOUBLE_EQ(load(bad), ServerConfig{}.trading.slippageBps) << bad.toStdString();
        EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const QString& warning) {
            return warning.contains("Ignored trading.slippage_bps=");
        }), 1) << bad.toStdString();
    }
    warnings.clear();
    EXPECT_DOUBLE_EQ(load("0"), 0.0);
    EXPECT_DOUBLE_EQ(load("9999.999"), 9999.999);
    EXPECT_TRUE(warnings.empty());
}

TEST(TradingEngineTest, ParseCommandJsonAndIgnoreUnknownAction) {
    const auto command = trading::parseTradeCommandJson(
        R"({"command_id":"c1","action":"PLACE_ORDER","symbol":"BTC-USD","side":"BUY","order_type":"LIMIT","qty":2,"price":99,"timestamp":1234,"algo_id":"a"})");
    EXPECT_EQ(command.commandId, "c1");
    EXPECT_EQ(command.action, trading::TradeAction::PlaceOrder);
    EXPECT_EQ(command.side, trading::OrderSide::Buy);
    EXPECT_EQ(command.orderType, trading::OrderType::Limit);
    EXPECT_DOUBLE_EQ(command.qty, 2.0);
    EXPECT_TRUE(command.hasPrice);
    EXPECT_DOUBLE_EQ(command.price, 99.0);
    EXPECT_EQ(command.timestamp, 1234);
    EXPECT_EQ(command.algoId, "a");
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);
    expectNoUpdates(engine.onCommand(trading::TradeCommand{}));
}
