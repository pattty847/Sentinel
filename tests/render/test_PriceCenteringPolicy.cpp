#include "render/PriceCenteringPolicy.hpp"

#include <gtest/gtest.h>

TEST(PriceCenteringPolicy, CentersOnBookMidWithGivenSpan) {
    PriceCenteringPolicy policy;
    policy.setTrade(83100.0);
    policy.setBook(83186.0, 83188.0);
    const auto range = policy.consume(90.0, true);
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->min, 83142.0);
    EXPECT_DOUBLE_EQ(range->max, 83232.0);
    EXPECT_FALSE(policy.consume(90.0, true));
}

TEST(PriceCenteringPolicy, SymbolChangeDropsOldQuotesAndUsesTradeFallback) {
    PriceCenteringPolicy policy;
    policy.setBook(83186.0, 83188.0);
    ASSERT_TRUE(policy.consume(90.0, true));
    policy.reset();
    EXPECT_FALSE(policy.consume(90.0, true));
    policy.setTrade(42000.0);
    const auto range = policy.consume(100.0, false);
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->min, 41950.0);
    EXPECT_DOUBLE_EQ(range->max, 42050.0);
    policy.setBook(42002.0, 42004.0);
    const auto upgraded = policy.consume(100.0, false);
    ASSERT_TRUE(upgraded);
    EXPECT_DOUBLE_EQ(upgraded->min, 41953.0);
    EXPECT_DOUBLE_EQ(upgraded->max, 42053.0);
}

TEST(PriceCenteringPolicy, FollowReenableUsesCurrentSpan) {
    PriceCenteringPolicy policy;
    policy.setBook(100.0, 102.0);
    ASSERT_TRUE(policy.consume(40.0, true));
    policy.requestFollow();
    const auto range = policy.consume(12.0, true);
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->min, 95.0);
    EXPECT_DOUBLE_EQ(range->max, 107.0);
}

TEST(PriceCenteringPolicy, ManualPriceInteractionCancelsPendingCenter) {
    PriceCenteringPolicy policy;
    policy.setTrade(100.0);
    policy.cancel();
    EXPECT_FALSE(policy.consume(20.0, false));
    policy.setBook(101.0, 103.0);
    EXPECT_FALSE(policy.consume(20.0, false));
    policy.requestFollow();
    const auto range = policy.consume(20.0, true);
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->min, 92.0);
    EXPECT_DOUBLE_EQ(range->max, 112.0);
}
