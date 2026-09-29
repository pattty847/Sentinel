#include "FrameStatsWindow.hpp"
#include <gtest/gtest.h>
#include <limits>

TEST(FrameStatsWindow, PercentilesAndOneSecondRate) {
    FrameStatsWindow window;
    for (int i = 1; i <= 20; ++i) window.add(1000 + i * 40, static_cast<double>(i));
    const auto stats = window.summarize(1900);
    EXPECT_EQ(stats.samples, 20);
    EXPECT_DOUBLE_EQ(stats.p50Ms, 10.0);
    EXPECT_DOUBLE_EQ(stats.p95Ms, 19.0);
    EXPECT_DOUBLE_EQ(stats.renderRateHz, 20.0);
}

TEST(FrameStatsWindow, ExpiresOldFramesAndRejectsInvalidDurations) {
    FrameStatsWindow window;
    window.add(1000, 2.0);
    window.add(1999, 4.0);
    window.add(2000, std::numeric_limits<double>::quiet_NaN());
    window.add(2000, -1.0);
    auto stats = window.summarize(2000);
    EXPECT_EQ(stats.samples, 1);
    EXPECT_DOUBLE_EQ(stats.p50Ms, 4.0);
    EXPECT_EQ(window.summarize(2999).samples, 0);
}

TEST(FrameStatsWindow, FixedCapacityKeepsNewestSamples) {
    FrameStatsWindow window;
    for (std::size_t i = 0; i < FrameStatsWindow::Capacity + 10; ++i)
        window.add(1000 + static_cast<int64_t>(i), static_cast<double>(i));
    const auto stats = window.summarize(2000);
    EXPECT_EQ(stats.samples, static_cast<int>(FrameStatsWindow::Capacity));
    EXPECT_DOUBLE_EQ(stats.p50Ms, 265.0);
    EXPECT_DOUBLE_EQ(stats.p95Ms, 496.0);
    window.clear();
    EXPECT_EQ(window.summarize(2000).samples, 0);
}
