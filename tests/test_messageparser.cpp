// =============================================================================
// MessageParser Unit Tests
// Validates JSON parsing of market trades and level 2 updates
// =============================================================================

#include <gtest/gtest.h>
#include "../libs/core/MessageParser.hpp"

// Fixture not required as functions are stateless, but we use namespace alias
namespace MP = MessageParser;

// -----------------------------------------------------------------------------
// ParseValidMarketTrades_ReturnsCorrectData
// -----------------------------------------------------------------------------
TEST(MessageParserTest, ParseValidMarketTrades_ReturnsCorrectData) {
    const std::string json = R"({
        "events": [{
            "trades": [{
                "product_id": "BTC-USD",
                "trade_id": "1",
                "price": "100.0",
                "size": "0.5",
                "side": "BUY"
            }]
        }]
    })";

    auto trades = MP::parseMarketTrades(json);
    ASSERT_EQ(trades.size(), 1u);
    const Trade& t = trades.front();
    EXPECT_EQ(t.product_id, "BTC-USD");
    EXPECT_EQ(t.trade_id, "1");
    EXPECT_DOUBLE_EQ(t.price, 100.0);
    EXPECT_DOUBLE_EQ(t.size, 0.5);
    EXPECT_EQ(t.side, AggressorSide::Buy);
}

// -----------------------------------------------------------------------------
// ParseMarketTrades_EmptyEvents_ReturnsEmptyVector
// -----------------------------------------------------------------------------
TEST(MessageParserTest, ParseMarketTrades_EmptyEvents_ReturnsEmptyVector) {
    const std::string json = R"({ "events": [] })";
    auto trades = MP::parseMarketTrades(json);
    EXPECT_TRUE(trades.empty());
}

// -----------------------------------------------------------------------------
// ParseMarketTrades_InvalidJson_Throws
// -----------------------------------------------------------------------------
TEST(MessageParserTest, ParseMarketTrades_InvalidJson_Throws) {
    const std::string json = "{ invalid json";
    EXPECT_THROW({ MP::parseMarketTrades(json); }, nlohmann::json::parse_error);
}

// -----------------------------------------------------------------------------
// ParseValidL2Update_ReturnsExpectedBook
// -----------------------------------------------------------------------------
TEST(MessageParserTest, ParseValidL2Update_ReturnsExpectedBook) {
    const std::string json = R"({
        "product_id": "ETH-USD",
        "updates": [
            { "side": "bid", "price_level": "100.0", "new_quantity": "2" },
            { "side": "offer", "price_level": "101.0", "new_quantity": "3" }
        ]
    })";

    auto book = MP::parseL2Update(json);
    EXPECT_EQ(book.product_id, "ETH-USD");
    ASSERT_EQ(book.bids.size(), 1u);
    ASSERT_EQ(book.asks.size(), 1u);
    EXPECT_DOUBLE_EQ(book.bids.front().price, 100.0);
    EXPECT_DOUBLE_EQ(book.bids.front().size, 2.0);
    EXPECT_DOUBLE_EQ(book.asks.front().price, 101.0);
    EXPECT_DOUBLE_EQ(book.asks.front().size, 3.0);
}

// -----------------------------------------------------------------------------
// ParseL2Update_EmptyUpdates_ReturnsEmptyBook
// -----------------------------------------------------------------------------
TEST(MessageParserTest, ParseL2Update_EmptyUpdates_ReturnsEmptyBook) {
    const std::string json = R"({ "product_id": "ETH-USD", "updates": [] })";
    auto book = MP::parseL2Update(json);
    EXPECT_EQ(book.product_id, "ETH-USD");
    EXPECT_TRUE(book.bids.empty());
    EXPECT_TRUE(book.asks.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

