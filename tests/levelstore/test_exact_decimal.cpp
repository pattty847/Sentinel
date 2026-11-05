#include "levelstore/ExactDecimal.hpp"
#include <gtest/gtest.h>

using namespace sentinel::levelstore;

TEST(ExactDecimalTests, CanonicalRoundTripAndRange) {
    for (const auto text : {"0", "1", "0.000000000000000001", "9223372036854775807",
                            "9.223372036854775807", "100.001", "0.03"}) {
        const auto result = parseDecimal(text);
        ASSERT_EQ(result.status, DecimalStatus::Exact) << text;
        EXPECT_EQ(decimalText(result.value), text);
        EXPECT_EQ(parseDecimal(decimalText(result.value)).value, result.value);
    }
    EXPECT_EQ(parseDecimal("9223372036854775808").status, DecimalStatus::Overflow);
    EXPECT_EQ(parseDecimal("9.223372036854775808").status, DecimalStatus::Overflow);
    EXPECT_EQ(parseDecimal("00000001.23000").value, (ExactDecimal{123, 2}));
    EXPECT_EQ(parseDecimal(std::string(4096, '0') + "1.00").value, (ExactDecimal{1, 0}));
    EXPECT_EQ(parseDecimal("0.000").value, (ExactDecimal{0, 0}));
}

TEST(ExactDecimalTests, InvalidIsDifferentFromOverflow) {
    for (const auto text : {"", "-1", "+1", "-0", "1e2", "1E-2", "nan", "inf", " 1", "1 ",
                            ".1", "1.", "1..2", "1.2.3", "9223372036854775808x"})
        EXPECT_EQ(parseDecimal(text).status, DecimalStatus::Invalid) << text;
}

TEST(ExactDecimalTests, TransitionsAreExactInBothDirections) {
    EXPECT_EQ(decimalAtScale({123, 2}, 4), 12300);
    EXPECT_EQ(decimalAtScale({12300, 4}, 2), 123);
    EXPECT_FALSE(decimalAtScale({12301, 4}, 2));
    EXPECT_EQ(decimalAtScale({INT64_MAX, 0}, 0), INT64_MAX);
    EXPECT_FALSE(decimalAtScale({INT64_MAX, 0}, 1));
    EXPECT_EQ(decimalAtScale({0, 0}, UINT32_MAX), 0);
    EXPECT_EQ(decimalAtScale({1, 0}, 18), 1000000000000000000LL);
    EXPECT_FALSE(decimalAtScale({1, 0}, 19));
}

TEST(ExactDecimalTests, GridDoesNotRoundOrRequireScaledInt64Headroom) {
    EXPECT_TRUE(onDecimalGrid({125, 2}, {5, 2}));
    EXPECT_FALSE(onDecimalGrid({123, 2}, {5, 2}));
    EXPECT_FALSE(onDecimalGrid({100001, 3}, {1, 2}));
    EXPECT_TRUE(onDecimalGrid({INT64_MAX, 0}, {1, 18}));
    EXPECT_TRUE(onDecimalGrid({0, 0}, {5, 2}));
    EXPECT_FALSE(onDecimalGrid({1, 0}, {0, 0}));
    EXPECT_FALSE(onDecimalGrid({INT64_MAX, 0}, {3, 18}));
}
