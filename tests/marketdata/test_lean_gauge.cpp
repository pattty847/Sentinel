#include "marketdata/analytics/LeanGauge.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <cmath>
using namespace sentinel::book;
namespace {
struct LeanFixture : testing::Test {
    std::array<double, 501> bids{}, asks{};
    LeanBook book{bids, asks, 0, 1, 199, 201};
    LeanGauge gauge;
    void update(int64_t time = 0, uint64_t symbol = 0) { gauge.update(book, symbol, time); }
};
TEST_F(LeanFixture, ExactBoundariesAndDisagreement) {
    bids[199] = 1; asks[201] = 1;
    bids[195] = 3; bids[185] = 5; bids[150] = 7; bids[100] = 11;
    asks[205] = 1; asks[215] = 2; asks[250] = 13; asks[300] = 17;
    bids[99] = asks[301] = 1000;
    update();
    const std::array<double, 4> b{4, 9, 16, 27}, a{2, 4, 17, 34};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(gauge.bands()[i].bid, b[i]);
        EXPECT_DOUBLE_EQ(gauge.bands()[i].ask, a[i]);
        EXPECT_NEAR(gauge.bands()[i].imbalance, (b[i] - a[i]) / (b[i] + a[i]), 1e-14);
    }
    EXPECT_TRUE(gauge.disagreement());
    asks[300] = 0; update(); EXPECT_FALSE(gauge.disagreement());
    // Decimal native grid: exact ±5 stays included.
    book.tick = .1; book.bidTop = 199; book.askTop = 201;
    update(); EXPECT_DOUBLE_EQ(gauge.bands()[0].bid, 16);
    bids.fill(0); asks.fill(0); book.tick = 2; book.bidTop = 199; book.askTop = 200;
    bids[199] = asks[200] = 1; bids[197] = 3; asks[202] = 2;
    update(); // half-slot mid: the levels at 197 and 202 are exactly five away
    EXPECT_EQ(gauge.bands()[0].bid, 4); EXPECT_EQ(gauge.bands()[0].ask, 3);
}
TEST_F(LeanFixture, DisagreementNeedsBothBandsAtLeastHalfAPercent) {
    // Near (+-5) and far (+-100) bands with opposite signs; one band rounding to 0% is no disagreement.
    const auto set = [&](double nearBid, double nearAsk, double farBid, double farAsk) {
        bids.fill(0); asks.fill(0);
        bids[199] = nearBid; asks[201] = nearAsk; bids[150] = farBid; asks[250] = farAsk;
        update();
    };
    set(100.4, 99.6, 0, 50); // near +0.4%, far strongly ask
    ASSERT_NEAR(gauge.bands()[0].imbalance, .004, 1e-12);
    EXPECT_LT(gauge.bands()[3].imbalance, -.1);
    EXPECT_FALSE(gauge.disagreement());
    set(100.6, 99.4, 0, 50); // near +0.6%
    ASSERT_NEAR(gauge.bands()[0].imbalance, .006, 1e-12);
    EXPECT_TRUE(gauge.disagreement());
    set(10, 1, 20, 29.24); // far: 30 bid vs 30.24 ask, -0.4%: rounds to 0%, no disagreement
    EXPECT_GT(gauge.bands()[0].imbalance, .5);
    EXPECT_GT(std::abs(gauge.bands()[3].imbalance), 0.);
    EXPECT_LT(std::abs(gauge.bands()[3].imbalance), .005);
    EXPECT_FALSE(gauge.disagreement());
}
TEST_F(LeanFixture, DepthIsMonotonicAndMirrored) {
    bids[199] = asks[201] = 2; bids[185] = asks[215] = 8; bids[100] = asks[300] = 10;
    update();
    EXPECT_EQ(gauge.bidDepth()[0], 0);
    EXPECT_EQ(gauge.bidDepth()[14], 2);
    EXPECT_EQ(gauge.bidDepth()[15], 10);
    EXPECT_EQ(gauge.bidDepth()[100], 20);
    for (size_t i = 1; i < LeanGauge::DepthPoints; ++i) {
        EXPECT_GE(gauge.bidDepth()[i], gauge.bidDepth()[i - 1]);
        EXPECT_EQ(gauge.bidDepth()[i], gauge.askDepth()[i]);
    }
}
TEST_F(LeanFixture, FractionalDistanceDepthDoesNotJumpEarly) {
    book.tick = .1;
    bids[199] = asks[201] = 2;
    bids[185] = asks[215] = 8;
    update();
    EXPECT_EQ(gauge.bidDepth()[0], 0);
    EXPECT_EQ(gauge.bidDepth()[1], 2);
    EXPECT_EQ(gauge.bidDepth()[2], 10);
    EXPECT_EQ(gauge.bidDepth(), gauge.askDepth());
}
TEST_F(LeanFixture, LockedNativeBucketCountsZeroDistanceAndKeepsHistory) {
    book.tick = .1;
    bids[199] = 3; asks[201] = 1;
    update(); update(500);
    bids.fill(0); asks.fill(0);
    book.bidTop = book.askTop = 200;
    bids[200] = 9; asks[200] = 3;
    update(1000);
    EXPECT_EQ(gauge.state(), LeanGauge::State::TwoSided);
    ASSERT_EQ(gauge.historySize(), 3u);
    EXPECT_EQ(gauge.history(0).time, 0);
    for (const auto& band : gauge.bands()) {
        EXPECT_EQ(band.bid, 9); EXPECT_EQ(band.ask, 3); EXPECT_DOUBLE_EQ(band.imbalance, .5);
    }
    EXPECT_EQ(gauge.bidDepth()[0], 9); EXPECT_EQ(gauge.askDepth()[0], 3);
}
TEST_F(LeanFixture, CrossedBookHoldsValuesAndHistoryUntilRecovery) {
    bids[199] = 3; asks[201] = 1;
    update(); update(500);
    const auto bands = gauge.bands();
    const auto bidDepth = gauge.bidDepth(), askDepth = gauge.askDepth();
    book.bidTop = 202; bids[202] = 100;
    update(1000); update(2000);
    EXPECT_EQ(gauge.state(), LeanGauge::State::Crossed);
    ASSERT_EQ(gauge.historySize(), 2u); EXPECT_EQ(gauge.history(1).time, 500);
    for (size_t i = 0; i < bands.size(); ++i) {
        EXPECT_EQ(gauge.bands()[i].bid, bands[i].bid);
        EXPECT_EQ(gauge.bands()[i].ask, bands[i].ask);
        EXPECT_EQ(gauge.bands()[i].imbalance, bands[i].imbalance);
    }
    EXPECT_EQ(gauge.bidDepth(), bidDepth); EXPECT_EQ(gauge.askDepth(), askDepth);
    book.bidTop = 199; bids[202] = 0; bids[199] = 7; update(2500);
    EXPECT_EQ(gauge.state(), LeanGauge::State::TwoSided);
    EXPECT_EQ(gauge.historySize(), 3u); EXPECT_DOUBLE_EQ(gauge.bands()[0].imbalance, .75);
    book.bidTop = 202; update(3000, 1);
    EXPECT_EQ(gauge.historySize(), 0u); EXPECT_EQ(gauge.bands()[0].bid, 0);
    book.bidTop = 199; update(3500, 1);
    book.bidTop = 202; book.tick = .1; update(4000, 1);
    EXPECT_EQ(gauge.historySize(), 0u); EXPECT_EQ(gauge.bidDepth().back(), 0);
}
TEST_F(LeanFixture, CadenceWrapAndTimeWindow) {
    bids[199] = 3; asks[201] = 1;
    update(); update(499); EXPECT_EQ(gauge.historySize(), 1u);
    update(500); EXPECT_EQ(gauge.historySize(), 2u);
    for (int i = 2; i <= 500; ++i) update(i * 500);
    ASSERT_EQ(gauge.historySize(), 241u);
    EXPECT_EQ(gauge.history(0).time, 130000);
    EXPECT_EQ(gauge.history(240).time, 250000);
    EXPECT_DOUBLE_EQ(gauge.history(0).value, .5);
    update(500000); EXPECT_EQ(gauge.historySize(), 1u);
    update(0); EXPECT_EQ(gauge.historySize(), 1u);
}
TEST_F(LeanFixture, SymbolTickAndWithdrawalResetHistory) {
    bids[199] = asks[201] = 1;
    update(); update(500); ASSERT_EQ(gauge.historySize(), 2u);
    update(1000, 1); EXPECT_EQ(gauge.historySize(), 1u);
    update(1500, 1); book.tick = .1; update(2000, 1); EXPECT_EQ(gauge.historySize(), 1u);
    book.bidTop = book.askTop = -1; update(2500, 1);
    EXPECT_EQ(gauge.historySize(), 0u); EXPECT_EQ(gauge.state(), LeanGauge::State::Empty);
    EXPECT_EQ(gauge.bands()[3].bid, 0); EXPECT_EQ(gauge.bidDepth().back(), 0);
}
TEST_F(LeanFixture, EmptyOneSidedLockedCrossedAndNonfinite) {
    book.bidTop = book.askTop = -1; update(); EXPECT_EQ(gauge.state(), LeanGauge::State::Empty);
    book.bidTop = 199; bids[199] = 3; bids[194] = 5; update();
    EXPECT_EQ(gauge.state(), LeanGauge::State::BidOnly); EXPECT_EQ(gauge.bands()[0].imbalance, 1);
    EXPECT_EQ(gauge.bands()[0].bid, 8); EXPECT_EQ(gauge.bands()[0].ask, 0);
    book.bidTop = -1; book.askTop = 201; asks[201] = 7; asks[206] = 11; update();
    EXPECT_EQ(gauge.state(), LeanGauge::State::AskOnly); EXPECT_EQ(gauge.bands()[0].imbalance, -1);
    EXPECT_EQ(gauge.bands()[0].bid, 0); EXPECT_EQ(gauge.bands()[0].ask, 18);
    book.bidTop = 201; update(); EXPECT_EQ(gauge.state(), LeanGauge::State::TwoSided);
    book.bidTop = 202; update(); EXPECT_EQ(gauge.state(), LeanGauge::State::Crossed);
    book.bidTop = 199; bids[199] = std::numeric_limits<double>::infinity(); update();
    for (const auto& band : gauge.bands()) {
        EXPECT_TRUE(std::isfinite(band.bid)); EXPECT_TRUE(std::isfinite(band.ask));
        EXPECT_TRUE(std::isfinite(band.imbalance));
    }
    EXPECT_EQ(gauge.bands()[3].bid, 5); EXPECT_EQ(gauge.bands()[3].ask, 18);
    for (size_t i = 0; i < LeanGauge::DepthPoints; ++i) {
        EXPECT_TRUE(std::isfinite(gauge.bidDepth()[i])); EXPECT_TRUE(std::isfinite(gauge.askDepth()[i]));
    }
    book.tick = 0; update(); EXPECT_EQ(gauge.state(), LeanGauge::State::Invalid);
}
}
