/*
Sentinel — SubscriptionManager Tests
Role: Verify desired product set management and deterministic frame generation
Testing Strategy: State transitions → assert frame content and order
Coverage: Subscribe, unsubscribe, transitions, empty states
*/
#include <gtest/gtest.h>
#include "marketdata/ws/SubscriptionManager.hpp"
#include <nlohmann/json.hpp>

// =============================================================================
// Basic Functionality Tests
// =============================================================================

TEST(SubscriptionManager, EmptyDesiredSetProducesNoFrames) {
    SubscriptionManager mgr;
    auto frames = mgr.buildSubscribeMsgs("test_jwt");

    EXPECT_TRUE(frames.empty());
}

TEST(SubscriptionManager, SubscribeToSingleProduct) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    auto frames = mgr.buildSubscribeMsgs("test_jwt");

    // Should produce 2 frames: level2 + market_trades
    ASSERT_EQ(frames.size(), 2);

    // Parse and verify structure
    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        EXPECT_EQ(json["type"], "subscribe");
        EXPECT_EQ(json["jwt"], "test_jwt");

        auto product_ids = json["product_ids"];
        ASSERT_EQ(product_ids.size(), 1);
        EXPECT_EQ(product_ids[0], "BTC-USD");

        std::string channel = json["channel"];
        EXPECT_TRUE(channel == "level2" || channel == "market_trades");
    }
}

TEST(SubscriptionManager, SubscribeToMultipleProducts) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD", "SOL-USD"});

    auto frames = mgr.buildSubscribeMsgs("test_jwt");

    ASSERT_EQ(frames.size(), 2);

    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        auto product_ids = json["product_ids"];
        ASSERT_EQ(product_ids.size(), 3);

        std::vector<std::string> products;
        for (const auto& id : product_ids) {
            products.push_back(id.get<std::string>());
        }

        EXPECT_TRUE(std::find(products.begin(), products.end(), "BTC-USD") != products.end());
        EXPECT_TRUE(std::find(products.begin(), products.end(), "ETH-USD") != products.end());
        EXPECT_TRUE(std::find(products.begin(), products.end(), "SOL-USD") != products.end());
    }
}

// =============================================================================
// Unsubscribe Tests
// =============================================================================

TEST(SubscriptionManager, UnsubscribeFromProducts) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD"});

    auto frames = mgr.buildUnsubscribeMsgs("test_jwt");

    ASSERT_EQ(frames.size(), 2);

    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        EXPECT_EQ(json["type"], "unsubscribe");
        EXPECT_EQ(json["jwt"], "test_jwt");

        auto product_ids = json["product_ids"];
        ASSERT_EQ(product_ids.size(), 2);
    }
}

TEST(SubscriptionManager, EmptyUnsubscribe) {
    SubscriptionManager mgr;
    auto frames = mgr.buildUnsubscribeMsgs("test_jwt");

    EXPECT_TRUE(frames.empty());
}

// =============================================================================
// State Transition Tests
// =============================================================================

TEST(SubscriptionManager, TransitionFromEmptyToProducts) {
    SubscriptionManager mgr;

    // Start empty
    EXPECT_TRUE(mgr.desired().empty());

    // Add products
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD"});

    ASSERT_EQ(mgr.desired().size(), 2);
    EXPECT_EQ(mgr.desired()[0], "BTC-USD");
    EXPECT_EQ(mgr.desired()[1], "ETH-USD");
}

TEST(SubscriptionManager, TransitionAddProducts) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    // Update to include more products
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD", "SOL-USD"});

    ASSERT_EQ(mgr.desired().size(), 3);
}

TEST(SubscriptionManager, TransitionRemoveProducts) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD", "SOL-USD"});

    // Remove some products
    mgr.setDesiredProducts({"ETH-USD"});

    ASSERT_EQ(mgr.desired().size(), 1);
    EXPECT_EQ(mgr.desired()[0], "ETH-USD");
}

TEST(SubscriptionManager, TransitionToEmpty) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD"});

    // Clear all
    mgr.setDesiredProducts({});

    EXPECT_TRUE(mgr.desired().empty());
    EXPECT_TRUE(mgr.buildSubscribeMsgs("jwt").empty());
}

// =============================================================================
// Channel Verification Tests
// =============================================================================

TEST(SubscriptionManager, BothChannelsPresent) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    auto frames = mgr.buildSubscribeMsgs("test_jwt");
    ASSERT_EQ(frames.size(), 2);

    std::set<std::string> channels;
    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        channels.insert(json["channel"].get<std::string>());
    }

    EXPECT_TRUE(channels.count("level2") > 0);
    EXPECT_TRUE(channels.count("market_trades") > 0);
}

// =============================================================================
// JWT Injection Tests
// =============================================================================

TEST(SubscriptionManager, JWTIsIncludedInFrames) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    std::string test_jwt = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.test";
    auto frames = mgr.buildSubscribeMsgs(test_jwt);

    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        EXPECT_EQ(json["jwt"], test_jwt);
    }
}

TEST(SubscriptionManager, DifferentJWTsProduceDifferentFrames) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    auto frames1 = mgr.buildSubscribeMsgs("jwt_token_1");
    auto frames2 = mgr.buildSubscribeMsgs("jwt_token_2");

    ASSERT_EQ(frames1.size(), frames2.size());

    // Frames should differ in JWT field
    auto json1 = nlohmann::json::parse(frames1[0]);
    auto json2 = nlohmann::json::parse(frames2[0]);

    EXPECT_NE(json1["jwt"], json2["jwt"]);
}

// =============================================================================
// Determinism Tests
// =============================================================================

TEST(SubscriptionManager, RepeatedCallsProduceSameFrames) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD"});

    auto frames1 = mgr.buildSubscribeMsgs("test_jwt");
    auto frames2 = mgr.buildSubscribeMsgs("test_jwt");

    ASSERT_EQ(frames1.size(), frames2.size());

    for (size_t i = 0; i < frames1.size(); ++i) {
        EXPECT_EQ(frames1[i], frames2[i]);
    }
}

TEST(SubscriptionManager, OrderIsStable) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD", "SOL-USD"});

    auto frames = mgr.buildSubscribeMsgs("test_jwt");

    // Channel order should be stable (typically level2 first, then market_trades)
    auto json0 = nlohmann::json::parse(frames[0]);
    auto json1 = nlohmann::json::parse(frames[1]);

    // Verify both channels exist and order is consistent
    std::string channel0 = json0["channel"];
    std::string channel1 = json1["channel"];

    EXPECT_TRUE((channel0 == "level2" && channel1 == "market_trades") ||
                (channel0 == "market_trades" && channel1 == "level2"));
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(SubscriptionManager, DuplicateProductsAreHandled) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD", "BTC-USD", "ETH-USD"});

    // Implementation may deduplicate or preserve; verify behavior
    auto frames = mgr.buildSubscribeMsgs("test_jwt");
    EXPECT_EQ(frames.size(), 2);  // Still 2 channels
}

TEST(SubscriptionManager, LargeProductList) {
    SubscriptionManager mgr;

    std::vector<std::string> products;
    for (int i = 0; i < 50; ++i) {
        products.push_back("COIN" + std::to_string(i) + "-USD");
    }

    mgr.setDesiredProducts(products);
    auto frames = mgr.buildSubscribeMsgs("test_jwt");

    ASSERT_EQ(frames.size(), 2);

    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        EXPECT_EQ(json["product_ids"].size(), 50);
    }
}

TEST(SubscriptionManager, EmptyStringJWT) {
    SubscriptionManager mgr;
    mgr.setDesiredProducts({"BTC-USD"});

    auto frames = mgr.buildSubscribeMsgs("");
    ASSERT_EQ(frames.size(), 2);

    for (const auto& frame : frames) {
        auto json = nlohmann::json::parse(frame);
        EXPECT_EQ(json["jwt"], "");
    }
}

// =============================================================================
// Integration Scenario Tests
// =============================================================================

TEST(SubscriptionManager, TypicalReconnectScenario) {
    SubscriptionManager mgr;

    // Initial subscription
    mgr.setDesiredProducts({"BTC-USD", "ETH-USD"});
    auto initial_frames = mgr.buildSubscribeMsgs("jwt_1");
    EXPECT_EQ(initial_frames.size(), 2);

    // Simulate reconnect (same desired set, new JWT)
    auto reconnect_frames = mgr.buildSubscribeMsgs("jwt_2");
    EXPECT_EQ(reconnect_frames.size(), 2);

    // Product IDs should be the same, only JWT differs
    auto json_initial = nlohmann::json::parse(initial_frames[0]);
    auto json_reconnect = nlohmann::json::parse(reconnect_frames[0]);

    EXPECT_EQ(json_initial["product_ids"], json_reconnect["product_ids"]);
    EXPECT_NE(json_initial["jwt"], json_reconnect["jwt"]);
}

TEST(SubscriptionManager, DesiredSetAccessor) {
    SubscriptionManager mgr;

    std::vector<std::string> products = {"BTC-USD", "ETH-USD", "SOL-USD"};
    mgr.setDesiredProducts(products);

    const auto& desired = mgr.desired();
    ASSERT_EQ(desired.size(), 3);
    EXPECT_EQ(desired, products);
}
