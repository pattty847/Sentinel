#include <gtest/gtest.h>

#include "servermodel/PriceLadder.hpp"

using recording::ladderTick;

TEST(PriceLadder, DeepFiveDollarGrid) {
    // Native $5: 5, 10, 20, 25, 50, 100, 200, 250, 500, ...
    EXPECT_DOUBLE_EQ(ladderTick(1.0, 5.0, 100.0), 5.0);
    EXPECT_DOUBLE_EQ(ladderTick(6.0, 5.0, 100.0), 10.0);
    EXPECT_DOUBLE_EQ(ladderTick(11.0, 5.0, 100.0), 20.0);
    EXPECT_DOUBLE_EQ(ladderTick(21.0, 5.0, 100.0), 25.0);
    EXPECT_DOUBLE_EQ(ladderTick(26.0, 5.0, 100.0), 50.0);
    EXPECT_DOUBLE_EQ(ladderTick(160.0, 5.0, 100.0), 200.0);
    EXPECT_DOUBLE_EQ(ladderTick(201.0, 5.0, 100.0), 250.0);
}

TEST(PriceLadder, NearOneDollarGridSkipsTwoFifty) {
    // Native $1: 1, 2, 5, 10, 20, 25, 50 ... ($2.5 is not a multiple of $1).
    EXPECT_DOUBLE_EQ(ladderTick(0.3, 1.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(ladderTick(1.5, 1.0, 100.0), 2.0);
    EXPECT_DOUBLE_EQ(ladderTick(2.1, 1.0, 100.0), 5.0);
    EXPECT_DOUBLE_EQ(ladderTick(21.0, 1.0, 100.0), 25.0);
}

TEST(PriceLadder, CentGridIncludesTwoPointFive) {
    EXPECT_DOUBLE_EQ(ladderTick(2.1, 0.01, 100.0), 2.5);
    EXPECT_DOUBLE_EQ(ladderTick(0.021, 0.01, 100.0), 0.05);  // 0.025 is not a whole cent
}

TEST(PriceLadder, BadInputs) {
    EXPECT_DOUBLE_EQ(ladderTick(10.0, 0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(ladderTick(std::nan(""), 5.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(ladderTick(10.0, 5.0, 0.0), 0.0);
}
