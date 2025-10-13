/*
Sentinel — MessageDispatcher Tests
Role: Verify pure JSON → Event parsing for all Coinbase channels
Testing Strategy: Golden JSON fixtures → assert Event types and fields
Coverage: Trades, order book snapshots/updates, acks, errors, malformed input
*/
#include <gtest/gtest.h>
#include "marketdata/dispatch/MessageDispatcher.hpp"
#include "fixtures/coinbase_messages.hpp"

// =============================================================================
// Trade Message Tests
// =============================================================================

TEST(MessageDispatcher, ParseSingleTrade) {
    auto json = fixtures::coinbaseTrade("BTC-USD", 95123.45, 0.05, "BUY");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);

    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);

    const auto& trade = trade_event->trade;
    EXPECT_EQ(trade.product_id, "BTC-USD");
    EXPECT_DOUBLE_EQ(trade.price, 95123.45);
    EXPECT_DOUBLE_EQ(trade.size, 0.05);
    EXPECT_EQ(trade.side, AggressorSide::Buy);
    EXPECT_EQ(trade.trade_id, "12345");
}

TEST(MessageDispatcher, ParseTradeSellSide) {
    auto json = fixtures::coinbaseTrade("ETH-USD", 3500.00, 1.25, "SELL");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Sell);
}

TEST(MessageDispatcher, ParseMultipleTrades) {
    auto json = fixtures::coinbaseMultiTrade("BTC-USD", {
        {95000.0, 0.1, "BUY"},
        {95001.0, 0.2, "SELL"},
        {95002.0, 0.3, "BUY"}
    });

    auto result = MessageDispatcher::parse(json);
    EXPECT_EQ(result.events.size(), 3);

    for (const auto& event : result.events) {
        EXPECT_TRUE(std::holds_alternative<TradeEvent>(event));
    }
}

TEST(MessageDispatcher, ParseTradeWithTimestamp) {
    auto json = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1, "buy",
                                       "12345", "2025-10-09T12:34:56.789123456Z");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);

    // Verify timestamp was parsed (non-epoch)
    auto epoch = std::chrono::system_clock::time_point();
    EXPECT_NE(trade_event->trade.timestamp, epoch);
}

// =============================================================================
// Order Book Snapshot Tests
// =============================================================================

TEST(MessageDispatcher, ParseOrderBookSnapshot) {
    auto json = fixtures::coinbaseL2Snapshot("BTC-USD",
        {{95000.0, 1.5}, {94999.0, 2.0}},  // bids
        {{95001.0, 1.0}, {95002.0, 0.5}}   // asks
    );

    auto result = MessageDispatcher::parse(json);

    // MessageDispatcher returns envelope events; detailed parsing happens in MarketDataCore
    ASSERT_GE(result.events.size(), 1);

    bool found_snapshot = false;
    for (const auto& event : result.events) {
        if (auto* snapshot = std::get_if<BookSnapshotEvent>(&event)) {
            EXPECT_EQ(snapshot->productId, "BTC-USD");
            found_snapshot = true;
        }
    }
    EXPECT_TRUE(found_snapshot);
}

// =============================================================================
// Order Book Update Tests
// =============================================================================

TEST(MessageDispatcher, ParseOrderBookUpdate) {
    auto json = fixtures::coinbaseL2Update("BTC-USD", {
        {"bid", 95000.0, 2.5},
        {"offer", 95001.0, 1.5}
    });

    auto result = MessageDispatcher::parse(json);
    ASSERT_GE(result.events.size(), 1);

    bool found_update = false;
    for (const auto& event : result.events) {
        if (auto* update = std::get_if<BookUpdateEvent>(&event)) {
            EXPECT_EQ(update->productId, "BTC-USD");
            found_update = true;
        }
    }
    EXPECT_TRUE(found_update);
}

// =============================================================================
// Side Detection Tests - Document Current Behavior
// =============================================================================
// IMPORTANT: fastSideDetection() ONLY recognizes uppercase "BUY"/"SELL"
// Lowercase or "offer" → AggressorSide::Unknown (line 37: Cpp20Utils::fastSideDetection)
// Side normalization for order books ("offer"→"ask") happens in MarketDataCore, NOT dispatcher

TEST(MessageDispatcher, UppercaseBuySideRecognized) {
    auto json = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1, "BUY");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Buy);
}

TEST(MessageDispatcher, UppercaseSellSideRecognized) {
    auto json = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1, "SELL");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Sell);
}

TEST(MessageDispatcher, LowercaseSideBecomesUnknown) {
    // BUG/LIMITATION: Cpp20Utils::fastSideDetection only checks uppercase
    // Real Coinbase messages may use lowercase "buy"/"sell"
    auto json = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1, "buy");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);
    // Documents current behavior - should be fixed to handle lowercase
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Unknown);
}

TEST(MessageDispatcher, OfferSideInTradesBecomesUnknown) {
    // "offer" is valid for order books but not recognized for trades
    auto json = fixtures::coinbaseTradeWithOffer("BTC-USD", 95000, 0.1);
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);
    // Documents current behavior
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Unknown);
}

TEST(MessageDispatcher, BookEventsDoNotNormalizeSide) {
    // Order book "offer" normalization happens in MarketDataCore, not dispatcher
    // Dispatcher only returns envelope events (BookSnapshotEvent/BookUpdateEvent)
    auto json = fixtures::coinbaseL2Update("BTC-USD", {
        {"offer", 95001.0, 1.0}
    });

    auto result = MessageDispatcher::parse(json);
    ASSERT_GE(result.events.size(), 1);

    // Dispatcher produces BookUpdateEvent without processing side strings
    auto* update = std::get_if<BookUpdateEvent>(&result.events[0]);
    EXPECT_NE(update, nullptr);
    EXPECT_EQ(update->productId, "BTC-USD");
}

// =============================================================================
// Subscription Acknowledgment Tests
// =============================================================================

TEST(MessageDispatcher, ParseSubscriptionAck) {
    auto json = fixtures::coinbaseSubscriptionAck({"BTC-USD", "ETH-USD"});
    auto result = MessageDispatcher::parse(json);

    ASSERT_GE(result.events.size(), 1);

    auto* ack = std::get_if<SubscriptionAckEvent>(&result.events[0]);
    ASSERT_NE(ack, nullptr);
    EXPECT_GE(ack->productIds.size(), 2);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(MessageDispatcher, ParseProviderError) {
    auto json = fixtures::coinbaseError("Invalid product_id", "INVALID_ARGUMENT");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);

    auto* error = std::get_if<ProviderErrorEvent>(&result.events[0]);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->message, "Invalid product_id");
}

TEST(MessageDispatcher, HandleMalformedJSON) {
    auto json = fixtures::malformedMessage();
    auto result = MessageDispatcher::parse(json);

    // Malformed messages return empty events (verified behavior: line 23 returns early)
    EXPECT_TRUE(result.events.empty());
}

TEST(MessageDispatcher, HandleNonObjectJSON) {
    nlohmann::json invalid = nlohmann::json::array({1, 2, 3});
    auto result = MessageDispatcher::parse(invalid);

    EXPECT_TRUE(result.events.empty());
}

TEST(MessageDispatcher, HandleEmptyJSON) {
    nlohmann::json empty = nlohmann::json::object();
    auto result = MessageDispatcher::parse(empty);

    EXPECT_TRUE(result.events.empty());
}

// =============================================================================
// Channel Recognition Tests - Verify Parser Behavior, Not Fixtures
// =============================================================================

TEST(MessageDispatcher, TradesChannelProducesTradeEvent) {
    auto json = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1, "BUY");
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<TradeEvent>(result.events[0]));
}

TEST(MessageDispatcher, L2ChannelProducesBookEvent) {
    auto json = fixtures::coinbaseL2Snapshot("BTC-USD", {{95000.0, 1.0}}, {{95001.0, 1.0}});
    auto result = MessageDispatcher::parse(json);

    ASSERT_GE(result.events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<BookSnapshotEvent>(result.events[0]));
}

TEST(MessageDispatcher, SubscriptionsChannelProducesAckEvent) {
    auto json = fixtures::coinbaseSubscriptionAck({"BTC-USD"});
    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<SubscriptionAckEvent>(result.events[0]));
}

// =============================================================================
// Edge Cases & Robustness
// =============================================================================

TEST(MessageDispatcher, HandleMissingChannel) {
    nlohmann::json json = {
        {"type", "update"},
        {"product_id", "BTC-USD"}
    };

    auto result = MessageDispatcher::parse(json);
    // Should handle gracefully (empty events or specific error event)
    EXPECT_NO_THROW(MessageDispatcher::parse(json));
}

TEST(MessageDispatcher, HandleMissingTradeFields) {
    // Test defaults: price=0, size=0, side=Unknown, timestamp=now()
    nlohmann::json json = {
        {"channel", "market_trades"},
        {"trades", nlohmann::json::array({
            {{"product_id", "BTC-USD"}}  // Missing price, size, side, time
        })}
    };

    auto result = MessageDispatcher::parse(json);

    ASSERT_EQ(result.events.size(), 1);
    auto* trade_event = std::get_if<TradeEvent>(&result.events[0]);
    ASSERT_NE(trade_event, nullptr);

    // Verify defaults (lines 34-35: fastStringToDouble("0"), line 37: side="", line 41: now())
    EXPECT_EQ(trade_event->trade.product_id, "BTC-USD");
    EXPECT_DOUBLE_EQ(trade_event->trade.price, 0.0);
    EXPECT_DOUBLE_EQ(trade_event->trade.size, 0.0);
    EXPECT_EQ(trade_event->trade.side, AggressorSide::Unknown);
    // timestamp will be now(), can't assert exact value
    EXPECT_NE(trade_event->trade.timestamp, std::chrono::system_clock::time_point());
}

// =============================================================================
// Performance & Scale Tests
// =============================================================================

TEST(MessageDispatcher, HandleLargeTradeVolume) {
    std::vector<std::tuple<double, double, std::string>> large_batch;
    for (int i = 0; i < 100; ++i) {
        large_batch.push_back({95000.0 + i, 0.1, "buy"});
    }

    auto json = fixtures::coinbaseMultiTrade("BTC-USD", large_batch);
    auto result = MessageDispatcher::parse(json);

    EXPECT_EQ(result.events.size(), 100);
}

TEST(MessageDispatcher, ParseIsStateless) {
    auto json1 = fixtures::coinbaseTrade("BTC-USD", 95000, 0.1);
    auto json2 = fixtures::coinbaseTrade("ETH-USD", 3500, 1.0);

    auto result1 = MessageDispatcher::parse(json1);
    auto result2 = MessageDispatcher::parse(json2);

    // Second parse should not affect first result
    ASSERT_EQ(result1.events.size(), 1);
    ASSERT_EQ(result2.events.size(), 1);

    auto* trade1 = std::get_if<TradeEvent>(&result1.events[0]);
    auto* trade2 = std::get_if<TradeEvent>(&result2.events[0]);

    EXPECT_EQ(trade1->trade.product_id, "BTC-USD");
    EXPECT_EQ(trade2->trade.product_id, "ETH-USD");
}
