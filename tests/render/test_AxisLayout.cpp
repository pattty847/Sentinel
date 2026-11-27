#include <gtest/gtest.h>
#include "../support/TestPaths.hpp"

#include "render/AxisLayout.hpp"
#include "render/AxisTextService.hpp"
#include "render/GridViewState.hpp"
#include "models/PriceAxisModel.hpp"
#include "render/ChartTextLayout.hpp"

namespace {

ChartTextAtlas buildAtlas() {
    ChartTextAtlas atlas;
    ChartTextAtlas::BuildParams params;
    params.fontPath =
        testpaths::qsource() +
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

TEST(AxisLayout, AxisInkUsesSecondaryTextColour) {
    EXPECT_EQ(AxisTextService::textColor(), QColor("#B6C2CF"));
}

// The grouped price labels (no "$", step decimals) keep the axis column, and with it
// the plot, at the width the pre-grouping text gave for the same ticks; a drawn label
// that is wider (7+ integer digits: two commas outgrow the "$") widens it, never clips.
TEST(AxisLayout, PriceAxisColumnKeepsPreGroupingWidthAndFitsDrawnLabels) {
    const ChartTextAtlas atlas = buildAtlas();
    struct Case { double min, range, tick; QString firstWidthLabel; bool drawnWider; };
    for (const Case& c : {Case{83000, 1600, 0.01, QStringLiteral("$83000"), false},
                          Case{84080, 20, 0.01, QStringLiteral("$84080.00"), false},
                          Case{1'234'000, 2000, 1, QStringLiteral("$1234000"), true}}) {
        SCOPED_TRACE(c.firstWidthLabel.toStdString());
        GridViewState view;
        PriceAxisModel axis;
        axis.setViewportSize(1200.0, 900.0);
        axis.setGridViewState(&view);
        axis.setProperty("tickSize", c.tick);
        view.setViewport(1, 2, c.min, c.min + c.range);
        std::vector<AxisModel::TickSnapshot> ticks;
        axis.copyTicks(ticks);
        ASSERT_GE(ticks.size(), 2u);
        EXPECT_EQ(ticks.front().widthLabel, c.firstWidthLabel) << "the pre-grouping text, unchanged";
        EXPECT_FALSE(ticks.front().label.contains('$'));
        EXPECT_TRUE(ticks.front().label.contains(','));
        std::vector<QString> drawn, before;
        for (const auto& tick : ticks) {
            drawn.push_back(tick.label);
            before.push_back(tick.widthLabel);
        }

        AxisTextService axes(atlas);
        axes.refreshAxisLayout();
        const int initialPx = axes.priceAxisWidthPx();
        const float scale = float(axes.effectiveAxisLabelPx()) / float(atlas.fontPx());
        axes.setPriceAxisSource(&axis);
        const int basePx = AxisLayout::measurePriceAxisWidthPx(atlas, scale, before, initialPx);
        const int drawnPx = AxisLayout::measurePriceAxisWidthPx(atlas, scale, drawn, initialPx);
        EXPECT_GE(axes.priceAxisWidthPx(), drawnPx) << "every drawn label fits the column";
        if (c.drawnWider) {
            EXPECT_GT(drawnPx, basePx);
            EXPECT_EQ(axes.priceAxisWidthPx(), drawnPx);
        } else {
            EXPECT_EQ(axes.priceAxisWidthPx(), basePx) << "same ticks, same column as before the grouping";
        }
    }
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
