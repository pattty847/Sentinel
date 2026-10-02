#include <gtest/gtest.h>
#include "marketdata/ws/SubscriptionManager.hpp"
TEST(SubscriptionManager, ThreeChannelsForExactlyOneProduct) {
    for (const std::string jwt : {"", "fixture-jwt"}) {
        const auto frames = SubscriptionManager::buildSubscribeMsgs("ETH-USD", jwt);
        ASSERT_EQ(frames.size(), 3u);
        std::vector<std::string> channels;
        for (const auto& bytes : frames) {
            const auto frame = nlohmann::json::parse(bytes);
            EXPECT_EQ(frame["type"], "subscribe");
            channels.push_back(frame["channel"]);
            if (frame["channel"] == "heartbeats") EXPECT_FALSE(frame.contains("product_ids"));
            else EXPECT_EQ(frame["product_ids"], nlohmann::json::array({"ETH-USD"}));
            if (jwt.empty()) EXPECT_FALSE(frame.contains("jwt"));
            else EXPECT_EQ(frame["jwt"], jwt);
        }
        EXPECT_EQ(channels, (std::vector<std::string>{"level2", "market_trades", "heartbeats"}));
    }
}
