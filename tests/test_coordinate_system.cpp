#include <gtest/gtest.h>
#include "../libs/gui/CoordinateSystem.h"

class CoordinateSystemTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(CoordinateSystemTest, TestWorldToScreen) {
    Viewport viewport{0, 1000, 100.0, 200.0, 800.0, 600.0};
    
    // Test center point
    QPointF result = CoordinateSystem::worldToScreen(500, 150.0, viewport);
    EXPECT_DOUBLE_EQ(result.x(), 400.0);  // Middle of time range
    EXPECT_DOUBLE_EQ(result.y(), 300.0);  // Middle of price range
    
    // Test corners
    QPointF topLeft = CoordinateSystem::worldToScreen(0, 200.0, viewport);
    EXPECT_DOUBLE_EQ(topLeft.x(), 0.0);
    EXPECT_DOUBLE_EQ(topLeft.y(), 0.0);
    
    QPointF bottomRight = CoordinateSystem::worldToScreen(1000, 100.0, viewport);
    EXPECT_DOUBLE_EQ(bottomRight.x(), 800.0);
    EXPECT_DOUBLE_EQ(bottomRight.y(), 600.0);
}

TEST_F(CoordinateSystemTest, TestRoundTrip) {
    Viewport viewport{1000, 2000, 50.0, 150.0, 1024.0, 768.0};
    
    int64_t originalTime = 1500;
    double originalPrice = 100.0;
    
    QPointF screenPos = CoordinateSystem::worldToScreen(originalTime, originalPrice, viewport);
    QPointF worldPos = CoordinateSystem::screenToWorld(screenPos, viewport);
    
    EXPECT_EQ(static_cast<int64_t>(worldPos.x()), originalTime);
    EXPECT_NEAR(worldPos.y(), originalPrice, 0.001);
}

TEST_F(CoordinateSystemTest, TestInvalidViewport) {
    Viewport invalidViewport{100, 50, 200.0, 100.0, 800.0, 600.0};  // Invalid time range
    
    QPointF result = CoordinateSystem::worldToScreen(75, 150.0, invalidViewport);
    EXPECT_EQ(result, QPointF(0, 0));  // Should return safe default
}

TEST_F(CoordinateSystemTest, TestViewportValidation) {
    Viewport validViewport{0, 1000, 100.0, 200.0, 800.0, 600.0};
    EXPECT_TRUE(CoordinateSystem::validateViewport(validViewport));
    
    Viewport invalidTime{1000, 500, 100.0, 200.0, 800.0, 600.0};
    EXPECT_FALSE(CoordinateSystem::validateViewport(invalidTime));
    
    Viewport invalidPrice{0, 1000, 200.0, 100.0, 800.0, 600.0};
    EXPECT_FALSE(CoordinateSystem::validateViewport(invalidPrice));
    
    Viewport zeroSize{0, 1000, 100.0, 200.0, 0.0, 600.0};
    EXPECT_FALSE(CoordinateSystem::validateViewport(zeroSize));
}