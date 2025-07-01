#include <gtest/gtest.h>
#include "libs/core/Authenticator.hpp"
#include "libs/core/DataCache.hpp"
#include "libs/core/MessageParser.hpp"
#include <fstream>
#include <cstdio>

class AuthenticatorTest : public ::testing::Test {
protected:
    std::string keyFile{"test_key.json"};
    void SetUp() override {
        std::ofstream out(keyFile);
        out << "{\n  \"key\": \"test-key\",\n  \"secret\": \"-----BEGIN PRIVATE KEY-----\\nMIIE...\n-----END PRIVATE KEY-----\"\n}";
    }
    void TearDown() override { std::remove(keyFile.c_str()); }
};

TEST_F(AuthenticatorTest, LoadApiKeys) {
    EXPECT_NO_THROW({ Authenticator auth(keyFile); });
}

TEST_F(AuthenticatorTest, GenerateJwtStructure) {
    Authenticator auth(keyFile);
    std::string jwt = auth.createJwt();
    EXPECT_FALSE(jwt.empty());
    EXPECT_EQ(std::count(jwt.begin(), jwt.end(), '.'), 2);
}

TEST(MessageParserTest, ParseMarketTrades) {
    std::string json = R"({
        \"events\": [{
            \"trades\": [{
                \"product_id\": \"BTC-USD\",
                \"trade_id\": \"1\",
                \"price\": \"100.0\",
                \"size\": \"0.5\",
                \"side\": \"BUY\"
            }]
        }]
    })";
    auto trades = MessageParser::parseMarketTrades(json);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].product_id, "BTC-USD");
    EXPECT_EQ(trades[0].trade_id, "1");
    EXPECT_DOUBLE_EQ(trades[0].price, 100.0);
    EXPECT_DOUBLE_EQ(trades[0].size, 0.5);
    EXPECT_EQ(trades[0].side, AggressorSide::Buy);
}

TEST(MessageParserTest, ParseL2Update) {
    std::string json = R"({
        \"product_id\": \"BTC-USD\",
        \"updates\": [
            {\"side\": \"bid\", \"price_level\": \"100.0\", \"new_quantity\": \"2\"},
            {\"side\": \"offer\", \"price_level\": \"101.0\", \"new_quantity\": \"3\"}
        ]
    })";
    auto book = MessageParser::parseL2Update(json);
    EXPECT_EQ(book.product_id, "BTC-USD");
    ASSERT_EQ(book.bids.size(), 1u);
    ASSERT_EQ(book.asks.size(), 1u);
    EXPECT_DOUBLE_EQ(book.bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(book.bids[0].size, 2.0);
    EXPECT_DOUBLE_EQ(book.asks[0].price, 101.0);
    EXPECT_DOUBLE_EQ(book.asks[0].size, 3.0);
}

class DataCacheTest : public ::testing::Test {
protected:
    DataCache cache;
};

TEST_F(DataCacheTest, RecentTrades) {
    Trade t1{std::chrono::system_clock::now(), "BTC-USD", "1", AggressorSide::Buy, 100.0, 0.1};
    Trade t2{std::chrono::system_clock::now(), "BTC-USD", "2", AggressorSide::Sell, 101.0, 0.2};
    cache.addTrade(t1);
    cache.addTrade(t2);
    auto trades = cache.recentTrades("BTC-USD");
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].trade_id, "1");
    EXPECT_EQ(trades[1].trade_id, "2");
}

TEST_F(DataCacheTest, LiveOrderBook) {
    OrderBook snapshot;
    snapshot.product_id = "BTC-USD";
    snapshot.bids.push_back({100.0, 1.0});
    snapshot.asks.push_back({101.0, 1.0});
    snapshot.timestamp = std::chrono::system_clock::now();

    cache.initializeLiveOrderBook("BTC-USD", snapshot);
    cache.updateLiveOrderBook("BTC-USD", "bid", 100.0, 2.0);
    cache.updateLiveOrderBook("BTC-USD", "offer", 101.0, 0.0); // remove ask

    auto book = cache.getLiveOrderBook("BTC-USD");
    ASSERT_EQ(book.bids.size(), 1u);
    EXPECT_DOUBLE_EQ(book.bids[0].size, 2.0);
    EXPECT_TRUE(book.asks.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
