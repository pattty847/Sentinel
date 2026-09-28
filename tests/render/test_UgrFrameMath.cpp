#include <gtest/gtest.h>

#include "render/UgrFrameMath.hpp"

namespace {

TEST(UgrFrameMath, ApplyDragPan_AdjustsViewportOnlyWhileDragging) {
    UgrFrameMath::ViewportState viewport;
    viewport.valid = true;
    viewport.timeStart = 1000.0;
    viewport.timeEnd = 2000.0;
    viewport.minPrice = 100.0;
    viewport.maxPrice = 200.0;
    viewport.panVisualOffset = QPointF(50.0, -20.0);
    viewport.dragging = true;

    const QRectF bounds(0.0, 0.0, 100.0, 100.0);
    const auto adjusted = UgrFrameMath::applyDragPan(viewport, bounds);

    EXPECT_DOUBLE_EQ(adjusted.timeStart, 500.0);
    EXPECT_DOUBLE_EQ(adjusted.timeEnd, 1500.0);
    EXPECT_DOUBLE_EQ(adjusted.minPrice, 80.0);
    EXPECT_DOUBLE_EQ(adjusted.maxPrice, 180.0);

    viewport.dragging = false;
    const auto unchanged = UgrFrameMath::applyDragPan(viewport, bounds);
    EXPECT_DOUBLE_EQ(unchanged.timeStart, 1000.0);
    EXPECT_DOUBLE_EQ(unchanged.timeEnd, 2000.0);
}

TEST(UgrFrameMath, ComputeRenderRects_ClipsToOverlapWindow) {
    UgrFrameMath::ViewportState viewport;
    viewport.valid = true;
    viewport.timeStart = 1000.0;
    viewport.timeEnd = 3000.0;
    viewport.minPrice = 110.0;
    viewport.maxPrice = 180.0;

    UgrFrameMath::GridState grid;
    grid.gridWidth = 10;
    grid.gridHeight = 100;
    grid.cadenceMs = 100;
    grid.timeOriginMs = 1000;
    grid.lastSliceStartMs = 1900;
    grid.filledColumns = 10;
    grid.tickSize = 1.0;
    grid.dataMinPrice = 100.0;
    grid.dataMaxPrice = 200.0;

    const QRectF bounds(0.0, 0.0, 1000.0, 500.0);
    const auto rects = UgrFrameMath::computeRenderRects(bounds, viewport, grid);

    EXPECT_GT(rects.drawRect.width(), 0.0);
    EXPECT_GT(rects.srcRect.width(), 0.0);
    EXPECT_TRUE(rects.dataStartValid);
    EXPECT_LE(rects.srcRect.right(), static_cast<double>(grid.gridWidth));
    EXPECT_LE(rects.srcRect.bottom(), static_cast<double>(grid.gridHeight));
}

TEST(UgrFrameMath, ComputeRenderRects_ForceFullUsesFullRects) {
    UgrFrameMath::ViewportState viewport;
    viewport.valid = true;
    viewport.timeStart = 1000.0;
    viewport.timeEnd = 3000.0;
    viewport.minPrice = 100.0;
    viewport.maxPrice = 200.0;

    UgrFrameMath::GridState grid;
    grid.gridWidth = 8;
    grid.gridHeight = 16;
    grid.cadenceMs = 100;
    grid.timeOriginMs = 1000;
    grid.lastSliceStartMs = 1700;
    grid.filledColumns = 8;
    grid.tickSize = 1.0;
    grid.dataMinPrice = 100.0;
    grid.dataMaxPrice = 200.0;
    grid.forceFull = true;

    const QRectF bounds(0.0, 0.0, 640.0, 320.0);
    const auto rects = UgrFrameMath::computeRenderRects(bounds, viewport, grid);

    EXPECT_EQ(rects.drawRect, bounds);
    EXPECT_EQ(rects.srcRect, QRectF(0.0, 0.0, 8.0, 16.0));
}

} // namespace
