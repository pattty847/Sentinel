/*
Sentinel — TimeAxisMapping unit tests
Validates mapping math, bucket alignment, edge cases, and legacy equivalence.
*/
#include <gtest/gtest.h>
#include <cmath>
#include "render/TimeAxisMapping.hpp"

namespace {

// Helper: build a standard mapping for testing
TimeAxisMapping makeStandardMapping() {
    TimeAxisMapping m;
    m.dataStartMs   = 1000.0;
    m.dataEndMs     = 6000.0;   // 5 columns * 1000ms each
    m.actualDataStartMs = 1000.0;
    m.actualDataEndMs   = 6000.0;
    m.dataMinPrice  = 100.0;
    m.dataMaxPrice  = 200.0;
    m.appendMs      = 1000.0;
    m.tickSize      = 1.0;      // 100 rows
    m.gridWidth     = 5;
    m.gridHeight    = 100;
    m.filledColumns = 5;

    m.viewStartMs   = 1000.0;
    m.viewEndMs     = 6000.0;
    m.viewMinPrice  = 100.0;
    m.viewMaxPrice  = 200.0;

    // Full view: srcRect covers entire grid, drawRect is the screen rect
    m.srcRect  = QRectF(0, 0, 5, 100);
    m.drawRect = QRectF(0, 0, 500, 1000);
    m.cellW    = 100.0;   // 500px / 5 columns
    m.cellH    = 10.0;    // 1000px / 100 rows
    m.timeOffset = 0.0f;
    m.valid    = true;

    return m;
}

} // namespace

// ============================================================================
// Mapping math correctness
// ============================================================================

TEST(TimeAxisMapping, TimeToScreenX_LeftEdge) {
    const auto m = makeStandardMapping();
    EXPECT_DOUBLE_EQ(m.timeToScreenX(m.dataStartMs), m.drawRect.x());
}

TEST(TimeAxisMapping, TimeToScreenX_RightEdge) {
    const auto m = makeStandardMapping();
    // dataEndMs = dataStartMs + gridWidth * appendMs = 6000
    // srcRect spans 0..5, column 5 is at x = srcRect.x + srcRect.width
    // timeToScreenX(6000) = drawRect.x + drawRect.width * ((5 - 0) / 5) = 500
    EXPECT_DOUBLE_EQ(m.timeToScreenX(m.dataEndMs), m.drawRect.x() + m.drawRect.width());
}

TEST(TimeAxisMapping, TimeToScreenX_Midpoint) {
    const auto m = makeStandardMapping();
    const double midTime = (m.dataStartMs + m.dataEndMs) / 2.0;
    const double expectedX = m.drawRect.x() + m.drawRect.width() / 2.0;
    EXPECT_NEAR(m.timeToScreenX(midTime), expectedX, 1e-9);
}

TEST(TimeAxisMapping, PriceToScreenY_TopEdge) {
    const auto m = makeStandardMapping();
    // dataMaxPrice at top of screen
    EXPECT_DOUBLE_EQ(m.priceToScreenY(m.dataMaxPrice), m.drawRect.y());
}

TEST(TimeAxisMapping, PriceToScreenY_BottomEdge) {
    const auto m = makeStandardMapping();
    EXPECT_DOUBLE_EQ(m.priceToScreenY(m.dataMinPrice), m.drawRect.y() + m.drawRect.height());
}

TEST(TimeAxisMapping, RoundTrip_TimeX) {
    const auto m = makeStandardMapping();
    const double testTime = 3500.0;
    const double screenX = m.timeToScreenX(testTime);
    const double recoveredTime = m.screenXToTime(screenX);
    EXPECT_NEAR(recoveredTime, testTime, 1e-9);
}

TEST(TimeAxisMapping, RoundTrip_PriceY) {
    const auto m = makeStandardMapping();
    const double testPrice = 150.0;
    const double screenY = m.priceToScreenY(testPrice);
    const double recoveredPrice = m.screenYToPrice(screenY);
    EXPECT_NEAR(recoveredPrice, testPrice, 1e-9);
}

// ============================================================================
// Bucket alignment
// ============================================================================

TEST(TimeAxisMapping, BucketStartMsForTime_MidBucket) {
    TimeAxisMapping m;
    m.appendMs = 60000.0;
    EXPECT_EQ(m.bucketStartMsForTime(60500.0), 60000);
}

TEST(TimeAxisMapping, BucketStartMsForTime_ExactBoundary) {
    TimeAxisMapping m;
    m.appendMs = 60000.0;
    EXPECT_EQ(m.bucketStartMsForTime(120000.0), 120000);
}

TEST(TimeAxisMapping, BucketIndexForTime_FractionalColumn) {
    const auto m = makeStandardMapping();
    // 2.5 columns into the data → column index 2
    const double timeMs = m.dataStartMs + 2.5 * m.appendMs;
    EXPECT_EQ(m.bucketIndexForTime(timeMs), 2);
}

TEST(TimeAxisMapping, BucketIndexForTime_FirstColumn) {
    const auto m = makeStandardMapping();
    EXPECT_EQ(m.bucketIndexForTime(m.dataStartMs), 0);
}

TEST(TimeAxisMapping, TimeToColumnF) {
    const auto m = makeStandardMapping();
    EXPECT_NEAR(m.timeToColumnF(m.dataStartMs + 2.5 * m.appendMs), 2.5, 1e-9);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(TimeAxisMapping, Invalid_TimeToScreenX_ReturnsZero) {
    TimeAxisMapping m;  // valid = false by default
    EXPECT_DOUBLE_EQ(m.timeToScreenX(1000.0), 0.0);
}

TEST(TimeAxisMapping, Invalid_PriceToScreenY_ReturnsZero) {
    TimeAxisMapping m;
    EXPECT_DOUBLE_EQ(m.priceToScreenY(150.0), 0.0);
}

TEST(TimeAxisMapping, EmptySrcRect_TimeToScreenX_ReturnsZero) {
    TimeAxisMapping m;
    m.valid = true;
    m.srcRect = QRectF(0, 0, 0, 0);  // empty
    m.appendMs = 1000.0;
    EXPECT_DOUBLE_EQ(m.timeToScreenX(1000.0), 0.0);
}

TEST(TimeAxisMapping, ZeroAppendMs_BucketStartReturnsZero) {
    TimeAxisMapping m;
    m.appendMs = 0.0;
    EXPECT_EQ(m.bucketStartMsForTime(60500.0), 0);
}

TEST(TimeAxisMapping, ZeroAppendMs_BucketIndexReturnsZero) {
    TimeAxisMapping m;
    m.appendMs = 0.0;
    m.gridWidth = 10;
    EXPECT_EQ(m.bucketIndexForTime(1000.0), 0);
}

TEST(TimeAxisMapping, PartiallyFilledGrid) {
    TimeAxisMapping m = makeStandardMapping();
    m.filledColumns = 3;
    m.actualDataStartMs = m.dataEndMs - 3.0 * m.appendMs;  // only last 3 columns
    m.actualDataEndMs = m.dataEndMs;
    EXPECT_DOUBLE_EQ(m.actualDataStartMs, 3000.0);
    EXPECT_DOUBLE_EQ(m.visibleDataStartMs(), 3000.0);  // max(viewStart=1000, actualStart=3000)
}

// ============================================================================
// Visible range helpers
// ============================================================================

TEST(TimeAxisMapping, VisibleDataRange_FullView) {
    const auto m = makeStandardMapping();
    EXPECT_DOUBLE_EQ(m.visibleDataStartMs(), 1000.0);
    EXPECT_DOUBLE_EQ(m.visibleDataEndMs(), 6000.0);
}

TEST(TimeAxisMapping, VisibleDataRange_PannedView) {
    auto m = makeStandardMapping();
    m.viewStartMs = 2000.0;
    m.viewEndMs   = 5000.0;
    EXPECT_DOUBLE_EQ(m.visibleDataStartMs(), 2000.0);
    EXPECT_DOUBLE_EQ(m.visibleDataEndMs(), 5000.0);
}

TEST(TimeAxisMapping, VisibleDataRange_ViewBeyondData) {
    auto m = makeStandardMapping();
    m.viewStartMs = 0.0;
    m.viewEndMs   = 10000.0;
    EXPECT_DOUBLE_EQ(m.visibleDataStartMs(), 1000.0);  // clamped to actualDataStart
    EXPECT_DOUBLE_EQ(m.visibleDataEndMs(), 6000.0);     // clamped to actualDataEnd
}

// ============================================================================
// Legacy equivalence: verify TimeAxisMapping produces same pixels as old math
// ============================================================================

TEST(TimeAxisMapping, LegacyEquivalence_CandleX) {
    // Simulate the old candle X math and verify TimeAxisMapping matches.
    // Old math: x = drawRect.x + (colF - anchorBaseColF) * cellW
    // where colF = (candleTime - actualDataStartMs) / appendMs
    //       anchorBaseColF = srcRect.x - (actualDataStartMs - dataStartMs) / appendMs

    const auto m = makeStandardMapping();
    const double candleTime = 3000.0;

    // Old math
    const double baseColF = m.srcRect.x();
    const double shiftCols = (m.actualDataStartMs - m.dataStartMs) / m.appendMs;
    const double anchorBaseColF = baseColF - shiftCols;
    const double colF = (candleTime - m.actualDataStartMs) / m.appendMs;
    const double oldX = m.drawRect.x() + (colF - anchorBaseColF) * m.cellW;

    // New math
    const double newX = m.timeToScreenX(candleTime);

    EXPECT_NEAR(newX, oldX, 0.5);
}

TEST(TimeAxisMapping, LegacyEquivalence_CandleY) {
    // Old math: y = bounds.y + bounds.height * ((maxPrice - price) / priceSpan)
    const auto m = makeStandardMapping();
    const double price = 150.0;

    const double priceSpan = m.dataMaxPrice - m.dataMinPrice;
    const double oldY = m.drawRect.y() + m.drawRect.height() * ((m.dataMaxPrice - price) / priceSpan);

    const double newY = m.priceToScreenY(price);

    EXPECT_NEAR(newY, oldY, 0.5);
}

TEST(TimeAxisMapping, LegacyEquivalence_ZoomedSubRange) {
    // Test with a zoomed-in view (srcRect is a sub-range of the grid)
    TimeAxisMapping m;
    m.dataStartMs   = 0.0;
    m.dataEndMs     = 10000.0;
    m.actualDataStartMs = 0.0;
    m.actualDataEndMs   = 10000.0;
    m.dataMinPrice  = 50.0;
    m.dataMaxPrice  = 150.0;
    m.appendMs      = 1000.0;
    m.tickSize      = 1.0;
    m.gridWidth     = 10;
    m.gridHeight    = 100;
    m.filledColumns = 10;
    m.viewStartMs   = 2000.0;
    m.viewEndMs     = 8000.0;
    m.viewMinPrice  = 70.0;
    m.viewMaxPrice  = 130.0;

    // srcRect: columns 2..8 (6 columns), rows 20..80 (60 rows)
    m.srcRect  = QRectF(2, 20, 6, 60);
    m.drawRect = QRectF(10, 10, 600, 600);
    m.cellW    = 600.0 / 6.0;   // 100 px per column
    m.cellH    = 600.0 / 60.0;  // 10 px per row
    m.timeOffset = 0.0f;
    m.valid    = true;

    // Test a candle at time 4000 (column 4, srcFrac = (4-2)/6 = 1/3)
    const double candleTime = 4000.0;
    const double expectedX = 10.0 + 600.0 * (1.0 / 3.0);
    EXPECT_NEAR(m.timeToScreenX(candleTime), expectedX, 0.5);

    // Old candle math equivalence
    const double colF = (candleTime - m.actualDataStartMs) / m.appendMs;  // 4.0
    const double anchorBaseColF = m.srcRect.x() - (m.actualDataStartMs - m.dataStartMs) / m.appendMs;  // 2.0
    const double oldX = m.drawRect.x() + (colF - anchorBaseColF) * m.cellW;  // 10 + (4-2)*100 = 210
    EXPECT_NEAR(m.timeToScreenX(candleTime), oldX, 0.5);

    // Price at 100 → row = (150-100)/1 = 50, srcFrac = (50-20)/60 = 0.5
    const double price = 100.0;
    const double expectedY = 10.0 + 600.0 * 0.5;
    EXPECT_NEAR(m.priceToScreenY(price), expectedY, 0.5);
}

TEST(TimeAxisMapping, BucketWidthPx) {
    const auto m = makeStandardMapping();
    EXPECT_DOUBLE_EQ(m.bucketWidthPx(), 100.0);
}
