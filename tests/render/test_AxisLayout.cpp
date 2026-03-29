#include <gtest/gtest.h>

#include "render/AxisLayout.hpp"
#include "render/ChartTextLayout.hpp"

namespace {

ChartTextAtlas buildAtlas() {
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath =
        QStringLiteral(SENTINEL_TEST_SOURCE_DIR) +
        QStringLiteral("/resources/fonts/RobotoMono/RobotoMono-Regular.ttf");
    params.charset = QStringLiteral(
        " 0123456789.+-,:/$ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    params.fontPx = 48;
    params.pxRange = 4.0f;
    EXPECT_TRUE(atlas.build(params));
    return atlas;
}

TEST(AxisLayout, ResolveEffectiveAxisLabelPx_PrefersEnvThenConfigThenAuto) {
    EXPECT_EQ(AxisLayout::resolveEffectiveAxisLabelPx(110.0, 0, 0), 15);
    EXPECT_EQ(AxisLayout::resolveEffectiveAxisLabelPx(96.0, 14, 0), 14);
    EXPECT_EQ(AxisLayout::resolveEffectiveAxisLabelPx(96.0, 12, 15), 15);
    EXPECT_EQ(AxisLayout::resolveEffectiveAxisLabelPx(96.0, 30, 0), 15);
}

TEST(AxisLayout, MeasurePriceAxisWidthPx_TracksLabelMagnitudeWithHysteresis) {
    const ChartTextAtlas atlas = buildAtlas();
    const float axisScale = 13.0f / static_cast<float>(atlas.fontPx());

    const int smallWidth = AxisLayout::measurePriceAxisWidthPx(
        atlas, axisScale, {QStringLiteral("$1.50"), QStringLiteral("$0.00")});
    const int largeWidth = AxisLayout::measurePriceAxisWidthPx(
        atlas, axisScale, {QStringLiteral("$70750.0"), QStringLiteral("$70660.0")});

    EXPECT_GE(smallWidth, AxisLayout::kPriceAxisMinWidthPx);
    EXPECT_LT(smallWidth, largeWidth);
    EXPECT_EQ(AxisLayout::applyDimensionHysteresis(largeWidth, largeWidth - 3),
              largeWidth);
    EXPECT_EQ(AxisLayout::applyDimensionHysteresis(largeWidth, largeWidth - 4),
              largeWidth - 4);
}

TEST(AxisLayout, MeasureTimeAxisHeightPx_UsesLineHeightAndClamp) {
    const ChartTextAtlas atlas = buildAtlas();
    const float axisScale = 13.0f / static_cast<float>(atlas.fontPx());

    const int timeHeight = AxisLayout::measureTimeAxisHeightPx(atlas, axisScale);
    EXPECT_GE(timeHeight, AxisLayout::kTimeAxisMinHeightPx);
    EXPECT_LE(timeHeight, AxisLayout::kTimeAxisMaxHeightPx);
}

TEST(AxisLayout, RunFitsRect_RejectsCornerOverlapForTimeAndPrice) {
    const ChartTextAtlas atlas = buildAtlas();
    const float axisScale = 13.0f / static_cast<float>(atlas.fontPx());

    ChartTextRun priceRun;
    priceRun.text = QStringLiteral("$70750.0");
    priceRun.anchor = QPointF(204.0, 12.0);
    priceRun.scale = axisScale;
    priceRun.hAlign = ChartTextRun::HorizontalAlign::Left;
    priceRun.vAlign = ChartTextRun::VerticalAlign::Center;
    priceRun.useStableMetrics = true;
    priceRun.pixelSnap = true;

    QRectF priceBounds;
    ASSERT_TRUE(ChartTextLayout::measureRunRect(atlas, priceRun, priceBounds));
    const QRectF priceRect = priceBounds.adjusted(-1.0, -1.0, 1.0, 1.0);
    EXPECT_TRUE(AxisLayout::runFitsRect(atlas, priceRun, priceRect));

    priceRun.anchor.setY(2.0);
    EXPECT_FALSE(AxisLayout::runFitsRect(atlas, priceRun, priceRect));

    ChartTextRun timeRun;
    timeRun.text = QStringLiteral("HH:mm");
    timeRun.anchor = QPointF(200.0, 194.0);
    timeRun.scale = axisScale;
    timeRun.hAlign = ChartTextRun::HorizontalAlign::Center;
    timeRun.vAlign = ChartTextRun::VerticalAlign::Center;
    timeRun.useStableMetrics = true;
    timeRun.pixelSnap = true;

    QRectF timeBounds;
    ASSERT_TRUE(ChartTextLayout::measureRunRect(atlas, timeRun, timeBounds));
    const QRectF timeRect = timeBounds.adjusted(-1.0, -1.0, 1.0, 1.0);
    EXPECT_TRUE(AxisLayout::runFitsRect(atlas, timeRun, timeRect));

    timeRun.anchor.setX(198.0);
    EXPECT_FALSE(AxisLayout::runFitsRect(atlas, timeRun, timeRect));
}

} // namespace
