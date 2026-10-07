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

} // namespace
