#include "PriceDisplayUnit.hpp"

#include <gtest/gtest.h>

#include <limits>

using sentinel::choosePriceDisplayUnit;
using sentinel::displayDecimals;
using sentinel::formatDisplayPrice;
using sentinel::PriceDisplayUnit;

TEST(PriceDisplayUnit, RecordedProductsAbovePennyStayPlain) {
    // BTC, ETH, SOL, AVAX, DOGE, FARTCOIN at and around recent prices.
    for (const double price : {101234.5, 3400.0, 180.0, 25.0, 0.17, 0.95, 0.01}) {
        SCOPED_TRACE(price);
        const auto unit = choosePriceDisplayUnit(price);
        EXPECT_FALSE(unit.scaled());
        EXPECT_STREQ(unit.tag(), "");
        EXPECT_EQ(unit.toDisplay(price), price);
    }
}

TEST(PriceDisplayUnit, PepeShowsInMillions) {
    const auto unit = choosePriceDisplayUnit(0.00000504);
    EXPECT_EQ(unit.exponent, 6);
    EXPECT_STREQ(unit.tag(), "x1M");
    EXPECT_EQ(formatDisplayPrice(0.00000504, 1e-8, unit), "5.04");
    EXPECT_EQ(formatDisplayPrice(0.00000499, 1e-8, unit), "4.99");
    EXPECT_EQ(formatDisplayPrice(1e-8, 1e-8, unit), "0.01"); // the tick itself
    EXPECT_EQ(formatDisplayPrice(0.0000051, 5e-8, unit), "5.10");
    EXPECT_NEAR(unit.toRaw(5.04), 0.00000504, 1e-20);
}

TEST(PriceDisplayUnit, PowerOfThousandBringsPriceIntoOneToThousand) {
    struct Case { double price; int exponent; };
    for (const auto [price, exponent] : {Case{0.0099, 3}, Case{0.005, 3}, Case{0.001, 3}, Case{0.000999, 6},
                                         Case{0.0000099, 6}, Case{0.000001, 6}, Case{0.0000005, 9},
                                         Case{1e-13, 12}, Case{1e-16, 12}}) {
        SCOPED_TRACE(price);
        const auto unit = choosePriceDisplayUnit(price);
        EXPECT_EQ(unit.exponent, exponent);
        const double shown = unit.toDisplay(price);
        if (exponent < sentinel::kMaxDisplayExponent) {
            EXPECT_GE(shown, 1.0 - 1e-9);
            EXPECT_LT(shown, 1000.0);
        }
    }
}

TEST(PriceDisplayUnit, UnknownPriceIsPlain) {
    for (const double price : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()})
        EXPECT_EQ(choosePriceDisplayUnit(price), PriceDisplayUnit{});
}

TEST(PriceDisplayUnit, DecimalsFollowTheShownStep) {
    EXPECT_EQ(displayDecimals(0.01), 2);
    EXPECT_EQ(displayDecimals(0.5), 1);
    EXPECT_EQ(displayDecimals(2.5), 1);
    EXPECT_EQ(displayDecimals(10), 0);
    EXPECT_EQ(displayDecimals(1e-8 * 1e6), 2); // float error from the scaling is not a digit
    EXPECT_EQ(displayDecimals(0), 0);
    // A plain unit keeps the raw decimals: BTC at a $0.1 tick.
    EXPECT_EQ(formatDisplayPrice(101234.5, 0.1, {}), "101234.5");
}
