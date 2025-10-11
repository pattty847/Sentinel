/*
Sentinel — DataCacheSinkAdapter Tests
Role: Verify integration between parsed events and DataCache
Testing Strategy: Event → Cache → Verify storage
Coverage: Trade storage, thread safety, cache integration
*/
#include <gtest/gtest.h>
#include "marketdata/sinks/DataCacheSinkAdapter.hpp"
#include "marketdata/cache/DataCache.hpp"
#include "marketdata/model/TradeData.h"
#include <chrono>

// =============================================================================
// Test Fixture
// =============================================================================

class DataCacheSinkAdapterTest : public ::testing::Test {
protected:
    DataCache cache;
    DataCacheSinkAdapter sink{cache};

    // Helper to create a test trade
    Trade makeTrade(
        const std::string& product_id,
        double price,
        double size,
        AggressorSide side = AggressorSide::Buy,
        const std::string& trade_id = "test_trade_123"
    ) {
        Trade trade;
        trade.product_id = product_id;
        trade.price = price;
        trade.size = size;
        trade.side = side;
        trade.trade_id = trade_id;
        trade.timestamp = std::chrono::system_clock::now();
        return trade;
    }
};

// =============================================================================
// Basic Trade Storage Tests
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, OnTradeStoresInCache) {
    Trade trade = makeTrade("BTC-USD", 95000.0, 0.1);

    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    ASSERT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].product_id, "BTC-USD");
    EXPECT_DOUBLE_EQ(trades[0].price, 95000.0);
    EXPECT_DOUBLE_EQ(trades[0].size, 0.1);
    EXPECT_EQ(trades[0].side, AggressorSide::Buy);
}

TEST_F(DataCacheSinkAdapterTest, MultipleTrades) {
    Trade trade1 = makeTrade("BTC-USD", 95000.0, 0.1, AggressorSide::Buy, "id1");
    Trade trade2 = makeTrade("BTC-USD", 95001.0, 0.2, AggressorSide::Sell, "id2");
    Trade trade3 = makeTrade("BTC-USD", 95002.0, 0.3, AggressorSide::Buy, "id3");

    sink.onTrade(trade1);
    sink.onTrade(trade2);
    sink.onTrade(trade3);

    auto trades = cache.recentTrades("BTC-USD");
    ASSERT_EQ(trades.size(), 3);

    EXPECT_EQ(trades[0].trade_id, "id1");
    EXPECT_EQ(trades[1].trade_id, "id2");
    EXPECT_EQ(trades[2].trade_id, "id3");
}

TEST_F(DataCacheSinkAdapterTest, DifferentProducts) {
    Trade btc_trade = makeTrade("BTC-USD", 95000.0, 0.1);
    Trade eth_trade = makeTrade("ETH-USD", 3500.0, 1.0);

    sink.onTrade(btc_trade);
    sink.onTrade(eth_trade);

    auto btc_trades = cache.recentTrades("BTC-USD");
    auto eth_trades = cache.recentTrades("ETH-USD");

    ASSERT_EQ(btc_trades.size(), 1);
    ASSERT_EQ(eth_trades.size(), 1);

    EXPECT_EQ(btc_trades[0].product_id, "BTC-USD");
    EXPECT_EQ(eth_trades[0].product_id, "ETH-USD");
}

// =============================================================================
// Trade Properties Verification
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, TradePropertiesPreserved) {
    auto now = std::chrono::system_clock::now();

    Trade trade;
    trade.product_id = "BTC-USD";
    trade.trade_id = "unique_trade_456";
    trade.price = 95123.45;
    trade.size = 0.05678;
    trade.side = AggressorSide::Sell;
    trade.timestamp = now;

    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    ASSERT_EQ(trades.size(), 1);

    const auto& stored = trades[0];
    EXPECT_EQ(stored.product_id, "BTC-USD");
    EXPECT_EQ(stored.trade_id, "unique_trade_456");
    EXPECT_DOUBLE_EQ(stored.price, 95123.45);
    EXPECT_DOUBLE_EQ(stored.size, 0.05678);
    EXPECT_EQ(stored.side, AggressorSide::Sell);
    EXPECT_EQ(stored.timestamp, now);
}

TEST_F(DataCacheSinkAdapterTest, BuySideStored) {
    Trade trade = makeTrade("BTC-USD", 95000.0, 0.1, AggressorSide::Buy);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_EQ(trades[0].side, AggressorSide::Buy);
}

TEST_F(DataCacheSinkAdapterTest, SellSideStored) {
    Trade trade = makeTrade("BTC-USD", 95000.0, 0.1, AggressorSide::Sell);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_EQ(trades[0].side, AggressorSide::Sell);
}

// =============================================================================
// High Volume / Ring Buffer Tests
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, HandlesHighTradeVolume) {
    // DataCache uses RingBuffer<Trade, 1000>
    for (int i = 0; i < 100; ++i) {
        Trade trade = makeTrade("BTC-USD", 95000.0 + i, 0.1,
                                AggressorSide::Buy, "id_" + std::to_string(i));
        sink.onTrade(trade);
    }

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_EQ(trades.size(), 100);
}

TEST_F(DataCacheSinkAdapterTest, RingBufferWrapAround) {
    // Push more than ring buffer capacity (1000)
    for (int i = 0; i < 1200; ++i) {
        Trade trade = makeTrade("BTC-USD", 95000.0 + i, 0.1,
                                AggressorSide::Buy, "id_" + std::to_string(i));
        sink.onTrade(trade);
    }

    auto trades = cache.recentTrades("BTC-USD");
    // Ring buffer should cap at 1000
    EXPECT_EQ(trades.size(), 1000);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, EmptyProductId) {
    Trade trade = makeTrade("", 95000.0, 0.1);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("");
    EXPECT_EQ(trades.size(), 1);
}

TEST_F(DataCacheSinkAdapterTest, ZeroPrice) {
    Trade trade = makeTrade("BTC-USD", 0.0, 0.1);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_DOUBLE_EQ(trades[0].price, 0.0);
}

TEST_F(DataCacheSinkAdapterTest, ZeroSize) {
    Trade trade = makeTrade("BTC-USD", 95000.0, 0.0);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_DOUBLE_EQ(trades[0].size, 0.0);
}

TEST_F(DataCacheSinkAdapterTest, NegativeValues) {
    // Should store as-is (validation is caller's responsibility)
    Trade trade = makeTrade("BTC-USD", -100.0, -0.5);
    sink.onTrade(trade);

    auto trades = cache.recentTrades("BTC-USD");
    EXPECT_DOUBLE_EQ(trades[0].price, -100.0);
    EXPECT_DOUBLE_EQ(trades[0].size, -0.5);
}

// =============================================================================
// Integration with Cache Query Methods
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, TradesSinceQuery) {
    Trade trade1 = makeTrade("BTC-USD", 95000.0, 0.1, AggressorSide::Buy, "id1");
    Trade trade2 = makeTrade("BTC-USD", 95001.0, 0.2, AggressorSide::Buy, "id2");
    Trade trade3 = makeTrade("BTC-USD", 95002.0, 0.3, AggressorSide::Buy, "id3");

    sink.onTrade(trade1);
    sink.onTrade(trade2);
    sink.onTrade(trade3);

    // Query trades since id1
    auto trades = cache.tradesSince("BTC-USD", "id1");

    // Should return id2 and id3
    EXPECT_GE(trades.size(), 2);
}

// =============================================================================
// Thread Safety (Smoke Test)
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, ConcurrentTrades) {
    // DataCache uses std::shared_mutex for thread safety
    // This is a basic smoke test; proper concurrency testing needs threading

    std::vector<Trade> trades;
    for (int i = 0; i < 10; ++i) {
        trades.push_back(makeTrade("BTC-USD", 95000.0 + i, 0.1,
                                   AggressorSide::Buy, "id_" + std::to_string(i)));
    }

    for (const auto& trade : trades) {
        sink.onTrade(trade);
    }

    auto stored = cache.recentTrades("BTC-USD");
    EXPECT_EQ(stored.size(), 10);
}

// =============================================================================
// Realistic Scenario Tests
// =============================================================================

TEST_F(DataCacheSinkAdapterTest, RealisticTradeSequence) {
    // Simulate a realistic sequence of BTC trades
    sink.onTrade(makeTrade("BTC-USD", 95000.00, 0.05, AggressorSide::Buy, "t1"));
    sink.onTrade(makeTrade("BTC-USD", 95000.50, 0.10, AggressorSide::Sell, "t2"));
    sink.onTrade(makeTrade("BTC-USD", 95001.00, 0.02, AggressorSide::Buy, "t3"));
    sink.onTrade(makeTrade("BTC-USD", 95000.75, 0.15, AggressorSide::Sell, "t4"));

    auto trades = cache.recentTrades("BTC-USD");
    ASSERT_EQ(trades.size(), 4);

    // Verify sequence is preserved
    EXPECT_EQ(trades[0].trade_id, "t1");
    EXPECT_EQ(trades[1].trade_id, "t2");
    EXPECT_EQ(trades[2].trade_id, "t3");
    EXPECT_EQ(trades[3].trade_id, "t4");
}

TEST_F(DataCacheSinkAdapterTest, MultiProductRealisticVolume) {
    // BTC trades
    for (int i = 0; i < 50; ++i) {
        sink.onTrade(makeTrade("BTC-USD", 95000.0 + i * 0.5, 0.1,
                               AggressorSide::Buy, "btc_" + std::to_string(i)));
    }

    // ETH trades
    for (int i = 0; i < 30; ++i) {
        sink.onTrade(makeTrade("ETH-USD", 3500.0 + i * 0.25, 0.5,
                               AggressorSide::Sell, "eth_" + std::to_string(i)));
    }

    auto btc_trades = cache.recentTrades("BTC-USD");
    auto eth_trades = cache.recentTrades("ETH-USD");

    EXPECT_EQ(btc_trades.size(), 50);
    EXPECT_EQ(eth_trades.size(), 30);
}
