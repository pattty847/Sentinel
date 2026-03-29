#include <gtest/gtest.h>

#include "trading/TradingEngine.hpp"
#include "trading/OrderStore.hpp"
#include "trading/PositionManager.hpp"

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

TEST(TradingEngineTest, PartialAndFullFillsAndOutOfOrderProtection) {
    trading::TradingEngine engine([](const std::string&) { return 100.0; }, 0.0);

    trading::TradeCommand cmd;
    cmd.commandId = "cid-limit";
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = "BTC-USD";
    cmd.side = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Unknown; // non-market: stays NEW for tests
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
