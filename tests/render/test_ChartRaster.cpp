// Whole-pixel chart mapping (docs/research/2026-10-08-whole-pixel-mapping.md section 5,
// checks A1-A8): the raster camera (render/ChartRaster.hpp) puts row and column edges on
// device pixel edges, moves a drag by whole device pixels, steps the integer pixels per
// row/column with a 0.1 px band, keeps the zoom anchor across a step, follows the device
// pixel ratio, never draws 0 px cells, commits a drag exactly as drawn (with
// GridViewState) and fits price on integer pixels per row. Pure CPU.
#include "render/ChartRaster.hpp"
#include "render/GridViewState.hpp"
#include "heatmap/HeatmapSpanPlanner.hpp"
#include <iostream>
#include <iomanip>

#include <QCoreApplication>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {
using namespace chart_raster;
constexpr int64_t kMinute = 60'000;
constexpr int64_t kEpoch = 1'790'000'000'000; // 2026

double fracDistance(double v) { return std::abs(v - std::round(v)); }

// A view of `rowsPx` device px per row and `colsPx` per column over the item.
RasterInputs viewOf(double rowsPx, double colsPx, double tick, double tfMs, double w, double h, double dpr,
                    double topPrice = 100'000.0, int64_t start = kEpoch + 7 * kMinute / 3) {
    RasterInputs in;
    in.tfMs = tfMs;
    in.tick = tick;
    in.itemWidthLogical = w;
    in.itemHeightLogical = h;
    in.dpr = dpr;
    const double heightDev = devicePixels(h, dpr), widthDev = devicePixels(w, dpr);
    in.maxPrice = topPrice;
    in.minPrice = topPrice - heightDev * tick / rowsPx;
    in.timeStart = start;
    in.timeEnd = start + int64_t(std::llround(widthDev * tfMs / colsPx));
    return in;
}

// A1: 2,000 random views (preset ticks across BTC/ETH/PEPE price scales, timeframes,
// dpr 1 and 2, sizes, anchors, drags, hysteresis states): every row edge and every
// column edge of the camera and of its TimeAxisMapping is on a whole device pixel.
TEST(ChartRaster, A1_RowAndColumnEdgesLandOnDevicePixels) {
    std::mt19937_64 rng(20261008);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    const double ladder[] = {1, 2, 2.5, 5};
    const int64_t tfs[] = {1'000, 60'000, 300'000, 3'600'000};
    struct Scale { double price; int lo, hi; } scales[] = {{100'000, -2, 1}, {3'000, -2, 0}, {0.00001, -9, -7}};
    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        const Scale scale = scales[i % 3];
        const int exponent = scale.lo + int(rng() % uint64_t(scale.hi - scale.lo + 1));
        const double tick = ladder[rng() % 4] * std::pow(10.0, exponent);
        const double tf = double(tfs[rng() % 4]);
        const double dpr = (rng() % 2) ? 2.0 : 1.0;
        const double w = std::round(uniform(200, 2000) * 2) / 2, h = std::round(uniform(100, 1200) * 2) / 2;
        auto in = viewOf(uniform(1.0, 40.0), uniform(1.0, 160.0), tick, tf, w, h, dpr,
                         scale.price * uniform(0.9, 1.1), kEpoch + int64_t(uniform(0, 1e9)));
        in.anchorFracX = uniform(0, 1);
        in.anchorFracY = uniform(0, 1);
        in.dragLogicalPx = QPointF(uniform(-50, 50), uniform(-50, 50));
        const RasterStep prev{int(rng() % 4) ? int(rng() % 40) : 0, int(rng() % 4) ? int(rng() % 160) : 0};
        const RasterCamera cam = computeRaster(in, prev);
        ASSERT_TRUE(cam.valid) << "i=" << i;
        const TimeAxisMapping map = toMapping(cam);
        ASSERT_TRUE(map.valid);
        SCOPED_TRACE(testing::Message() << "i=" << i << " tick=" << tick << " tf=" << tf << " dpr=" << dpr
                                        << " P=" << cam.rowPx << " C=" << cam.colPx);
        const int64_t b0 = int64_t(std::floor(cam.drawnMinPrice / tick)) - 1;
        const int64_t b1 = int64_t(std::ceil(cam.drawnMaxPrice / tick)) + 1;
        for (int64_t b = b0; b <= b1; b += std::max<int64_t>(1, (b1 - b0) / 37)) {
            const double price = double(b) * tick;
            ASSERT_LT(fracDistance(cam.yDev(price)), 1e-6) << "row edge b=" << b << " y=" << cam.yDev(price);
            ASSERT_LT(fracDistance(map.priceToScreenY(price) * dpr), 1e-6) << "mapping row edge b=" << b;
            ++checked;
        }
        const int64_t k0 = int64_t(std::floor(cam.drawnStartMs / tf)) - 1;
        const int64_t k1 = int64_t(std::ceil(cam.drawnEndMs / tf)) + 1;
        for (int64_t k = k0; k <= k1; k += std::max<int64_t>(1, (k1 - k0) / 37)) {
            const double t = double(k) * tf;
            ASSERT_LT(fracDistance(cam.xDev(t)), 1e-6) << "column edge k=" << k << " x=" << cam.xDev(t);
            ASSERT_LT(fracDistance(map.timeToScreenX(t) * dpr), 1e-6) << "mapping column edge k=" << k;
            ++checked;
        }
        // The drawn window is the device-integer surface (its ms bounds are doubles at
        // epoch scale: ~1e-4 ms of rounding, far below a pixel).
        EXPECT_NEAR(cam.yDev(cam.drawnMaxPrice), 0.0, 1e-6);
        EXPECT_NEAR(cam.yDev(cam.drawnMinPrice), double(cam.heightDev), 1e-6);
        EXPECT_NEAR(cam.xDev(cam.drawnStartMs), 0.0, 1e-4);
        EXPECT_NEAR(cam.xDev(cam.drawnEndMs), double(cam.widthDev), 1e-4);
        EXPECT_DOUBLE_EQ(map.cellH * dpr, double(cam.rowPx));
        EXPECT_DOUBLE_EQ(map.cellW * dpr, double(cam.colPx));
    }
    EXPECT_GT(checked, 100'000);
}

// A2: a drag of 0.07 logical px steps at dpr 2 moves the picture by exactly
// llround(drag * dpr) device pixels in m and n, P and C never change, every row is P tall.
TEST(ChartRaster, A2_ADragMovesByWholeDevicePixels) {
    const RasterInputs base = viewOf(2.4, 16.4, 1.0, double(kMinute), 640, 320, 2.0);
    const RasterCamera still = computeRaster(base, {});
    ASSERT_TRUE(still.valid);
    ASSERT_EQ(still.rowPx, 2);
    ASSERT_EQ(still.colPx, 16);
    RasterStep prev = still.step();
    int moves = 0;
    for (int i = 1; i <= 500; ++i) {
        RasterInputs in = base;
        in.dragLogicalPx = QPointF(0.07 * i, -0.07 * i * 0.6);
        const RasterCamera cam = computeRaster(in, prev);
        ASSERT_TRUE(cam.valid);
        EXPECT_EQ(cam.rowPx, still.rowPx) << "step " << i;
        EXPECT_EQ(cam.colPx, still.colPx) << "step " << i;
        EXPECT_EQ(still.leftColIndex - cam.leftColIndex, std::llround(in.dragLogicalPx.x() * 2.0)) << "step " << i;
        EXPECT_EQ(cam.topRowIndex - still.topRowIndex, std::llround(in.dragLogicalPx.y() * 2.0)) << "step " << i;
        const double b = std::floor(cam.drawnMaxPrice) - 3;
        for (int r = 0; r < 20; ++r)
            EXPECT_DOUBLE_EQ(cam.yDev(b - r - 1) - cam.yDev(b - r), double(cam.rowPx)) << "step " << i;
        moves += cam.leftColIndex != still.leftColIndex;
        prev = cam.step();
    }
    EXPECT_GT(moves, 400);
}

// A3: the integer step band: a slow zoom 2.0 -> 3.0 -> 2.0 steps once each way, at
// 2.6 and 2.4; jitter around 2.5 never flips.
TEST(ChartRaster, A3_TheStepBandHoldsTheIntegerAcrossTheHalf) {
    const double h = 300;
    auto rowsAt = [&](double r, RasterStep prev) {
        return computeRaster(viewOf(r, 16, 1.0, double(kMinute), 640, h, 1.0), prev);
    };
    RasterStep prev{};
    std::vector<std::pair<double, int>> flips; // r at which P changed, new P
    int last = 0;
    auto visit = [&](double r) {
        const RasterCamera cam = rowsAt(r, prev);
        ASSERT_TRUE(cam.valid);
        if (last && cam.rowPx != last) flips.push_back({r, cam.rowPx});
        last = cam.rowPx;
        prev = cam.step();
    };
    for (int i = 0; i <= 100; ++i) visit(2.0 + 0.01 * i);
    for (int i = 100; i >= 0; --i) visit(2.0 + 0.01 * i);
    ASSERT_EQ(flips.size(), 2u);
    EXPECT_EQ(flips[0].second, 3);
    EXPECT_GE(flips[0].first, 2.6 - 1e-9) << "2 -> 3 only beyond 2.5 + 0.1";
    EXPECT_LE(flips[0].first, 2.61 + 1e-9);
    EXPECT_EQ(flips[1].second, 2);
    EXPECT_LE(flips[1].first, 2.4 + 1e-9) << "3 -> 2 only below 2.5 - 0.1";
    EXPECT_GE(flips[1].first, 2.39 - 1e-9);
    // Jitter +-0.05 around 2.5 (both starting states): no flip.
    for (const int start : {2, 3}) {
        prev = {start, 16};
        for (int i = 0; i < 200; ++i) {
            const RasterCamera cam = rowsAt(2.5 + ((i % 2) ? 0.05 : -0.05) * ((i % 7) / 6.0), prev);
            EXPECT_EQ(cam.rowPx, start) << "jitter step " << i;
            prev = cam.step();
        }
    }
    EXPECT_EQ(stepPixels(2.61, 2), 3);
    EXPECT_EQ(stepPixels(2.59, 2), 2);
    EXPECT_EQ(stepPixels(2.39, 3), 2);
    EXPECT_EQ(stepPixels(2.41, 3), 3);
}

// A4: the anchor (30% down) stays within half a device pixel of where the continuous
// camera puts it, before and after P steps 2 -> 3.
TEST(ChartRaster, A4_TheAnchorStaysPutAcrossAnIntegerStep) {
    const double h = 500, tick = 1.0, anchorPrice = 100'000.0 - 123.25;
    RasterStep prev{2, 0}; // drawn at 2 px rows before the zoom
    int seenP[4] = {};
    for (const double r : {2.55, 2.59, 2.61, 2.65}) {
        RasterInputs in;
        in.tfMs = double(kMinute);
        in.tick = tick;
        in.itemWidthLogical = 640;
        in.itemHeightLogical = h;
        in.dpr = 1.0;
        in.timeStart = kEpoch;
        in.timeEnd = kEpoch + 40 * kMinute;
        const double span = h * tick / r;
        in.maxPrice = anchorPrice + 0.3 * span; // zoomed about the anchor
        in.minPrice = in.maxPrice - span;
        in.anchorFracY = 0.3;
        const RasterCamera cam = computeRaster(in, prev);
        ASSERT_TRUE(cam.valid);
        EXPECT_LE(std::abs(cam.yDev(anchorPrice) - 0.3 * h), 0.5) << "r=" << r << " P=" << cam.rowPx;
        ++seenP[std::min(cam.rowPx, 3)];
        prev = cam.step();
    }
    EXPECT_EQ(seenP[2], 2);
    EXPECT_EQ(seenP[3], 2) << "the step happened";
}

// A5: the same logical view at dpr 1 and 2: twice the device pixels per row (+-1),
// the same logical row height within half a logical pixel.
TEST(ChartRaster, A5_TheDevicePixelRatioScalesTheIntegers) {
    for (const double r1 : {1.0, 1.7, 2.4, 3.3, 5.0, 8.6, 13.2}) {
        const RasterInputs one = viewOf(r1, r1 * 4, 1.0, double(kMinute), 640.5, 320.5, 1.0);
        RasterInputs two = one;
        two.dpr = 2.0;
        const RasterCamera a = computeRaster(one, {}), b = computeRaster(two, {});
        ASSERT_TRUE(a.valid && b.valid);
        EXPECT_EQ(a.heightDev, 320);
        EXPECT_EQ(b.heightDev, 641);
        EXPECT_EQ(b.widthDev, 1281);
        EXPECT_LE(std::abs(b.rowPx - 2 * a.rowPx), 1) << "r1=" << r1;
        EXPECT_LE(std::abs(b.colPx - 2 * a.colPx), 1) << "r1=" << r1;
        EXPECT_LE(std::abs(b.rowPx / 2.0 - a.rowPx), 0.5) << "r1=" << r1;
        EXPECT_DOUBLE_EQ(toMapping(b).drawRect.height(), 320.5);
    }
}

// A6: never a 0 px row or column: r in [1, 1.4] from nothing is 1; a (hypothetical)
// 0.4 is 1; from 2 the band holds until 1.4, then 1.
TEST(ChartRaster, A6_CellsAreAtLeastOneDevicePixel) {
    for (double r = 1.0; r <= 1.4 + 1e-9; r += 0.05) {
        const RasterCamera cam = computeRaster(viewOf(r, r, 1.0, double(kMinute), 640, 320, 1.0), {});
        ASSERT_TRUE(cam.valid);
        EXPECT_EQ(cam.colPx, 1) << "rCol=" << r;
        EXPECT_EQ(cam.rowPx, 1) << "rRow=" << r;
    }
    const RasterCamera thin = computeRaster(viewOf(0.4, 0.4, 1.0, double(kMinute), 640, 320, 1.0), {});
    ASSERT_TRUE(thin.valid);
    EXPECT_EQ(thin.rowPx, 1);
    EXPECT_EQ(thin.colPx, 1);
    EXPECT_EQ(stepPixels(0.4, 0), 1);
    EXPECT_EQ(stepPixels(0.4, 1), 1);
    EXPECT_EQ(stepPixels(1.41, 2), 2) << "from 2 the band holds down to 1.4";
    EXPECT_EQ(stepPixels(1.39, 2), 1);
    // Tick 0 (none drawn yet): rows of one device pixel.
    RasterInputs none = viewOf(2.4, 16, 1.0, double(kMinute), 640, 320, 2.0);
    none.tick = 0;
    const RasterCamera unticked = computeRaster(none, {});
    ASSERT_TRUE(unticked.valid);
    EXPECT_EQ(unticked.rowPx, 1);
}

// A7: GridViewState commits a drag through the raster camera's PanShift: the camera
// after the release equals the camera during the drag (same m and n), for whole and
// fractional device-pixel drags, horizontally and vertically.
struct RasterHost { // what UnifiedGridRenderer feeds the camera
    double tf = double(kMinute), tick = 1.0, w = 640, h = 320, dpr = 2.0;
    RasterStep step;
    RasterInputs inputs(const GridViewState &v, bool withDrag) const {
        RasterInputs in;
        in.timeStart = v.getVisibleTimeStart();
        in.timeEnd = v.getVisibleTimeEnd();
        in.minPrice = v.getMinPrice();
        in.maxPrice = v.getMaxPrice();
        in.dragLogicalPx = withDrag && v.isDragging() ? v.getPanVisualOffset() : QPointF();
        in.tfMs = tf;
        in.tick = tick;
        in.itemWidthLogical = w;
        in.itemHeightLogical = h;
        in.dpr = dpr;
        in.anchorFracX = v.rasterAnchor().fracX;
        in.anchorFracY = v.rasterAnchor().fracY;
        return in;
    }
};
TEST(ChartRaster, A7_AReleaseCommitsExactlyWhatTheDragDrew) {
    for (const QPointF drag : {QPointF(0, 3.5), QPointF(3.5, 0), QPointF(3.3, -3.3), QPointF(-1.65, 2.2),
                               QPointF(0.3, 0.3), QPointF(11.15, -7.45)}) {
        SCOPED_TRACE(testing::Message() << "drag=" << drag.x() << "," << drag.y());
        RasterHost host;
        GridViewState v;
        v.setViewportSize(host.w, host.h);
        v.enableAutoScroll(false);
        // r = 2.4 rows and 16.4 columns per device px (P = 2, C = 16).
        const double heightDev = host.h * host.dpr, widthDev = host.w * host.dpr;
        const qint64 start = kEpoch + 7 * kMinute / 3 + 12'345;
        v.setViewport(start, start + qint64(std::llround(widthDev * host.tf / 16.4)), 100'000 - heightDev / 2.4 + 0.37,
                      100'000.37);
        v.setRasterAnchor(0.37, 0.61);
        v.setPanShift([&](QPointF d, qint64 &timeShift, double &priceShift) {
            int64_t t = 0;
            if (!panShift(host.inputs(v, false), host.step, d, t, priceShift)) return false;
            timeShift = t;
            return true;
        });
        host.step = computeRaster(host.inputs(v, false), {}).step();
        ASSERT_EQ(host.step, (RasterStep{2, 16}));
        v.handlePanStart(QPointF(300, 150));
        v.handlePanMove(QPointF(300, 150) + drag);
        const RasterCamera during = computeRaster(host.inputs(v, true), host.step);
        v.handlePanEnd(true);
        ASSERT_FALSE(v.isDragging());
        const RasterCamera after = computeRaster(host.inputs(v, true), during.step());
        EXPECT_EQ(after.leftColIndex, during.leftColIndex);
        EXPECT_EQ(after.topRowIndex, during.topRowIndex);
        EXPECT_EQ(after.step(), during.step());
    }
}

// Review fix 4: a 1 ms stored window (any direct viewport is accepted) draws at least
// one ms per device pixel (C <= tf), and every whole-pixel drag then has an exact
// whole-ms commit.
TEST(ChartRaster, A7_HighZoomDragsStillCommitExactly) {
    for (const double tf : {1'000.0, double(kMinute), 3'600'000.0}) {
        RasterInputs in = viewOf(2.4, 16.4, 1.0, tf, 640, 320, 2.0);
        in.timeEnd = in.timeStart + 1;
        const RasterCamera cam = computeRaster(in, {});
        ASSERT_TRUE(cam.valid);
        EXPECT_EQ(cam.colPx, maxColumnPixels(tf)) << "tf " << tf;
        EXPECT_LE(double(cam.colPx), tf);
        for (const double drag : {0.5, 1.0, -1.0, 3.3, -7.7, 40.2}) {
            int64_t timeShift = 0;
            double priceShift = 0;
            ASSERT_TRUE(panShift(in, cam.step(), QPointF(drag, 0), timeShift, priceShift)) << "tf " << tf << " drag " << drag;
            RasterInputs dragged = in, committed = in;
            dragged.dragLogicalPx = QPointF(drag, 0);
            committed.timeStart += timeShift;
            committed.timeEnd += timeShift;
            EXPECT_EQ(computeRaster(committed, cam.step()).leftColIndex, computeRaster(dragged, cam.step()).leftColIndex)
                << "tf " << tf << " drag " << drag;
        }
    }
}

// A8: a price fit lands on an integer number of device pixels per row, never narrower
// than the margined span, and the camera then draws exactly the fitted window.
TEST(ChartRaster, A8_PriceFitsAreIntegerPixelSpans) {
    std::mt19937_64 rng(8);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    for (int i = 0; i < 500; ++i) {
        const double tick = (i % 3 == 0) ? 0.5 : (i % 3 == 1) ? 5.0 : 0.00000001;
        const double price = (i % 3 == 2) ? 0.00001 : 100'000;
        const int heightDev = 200 + int(rng() % 1200);
        const double span = heightDev * tick / uniform(1.05, 30.0);
        const double centre = price * uniform(0.95, 1.05);
        double lo = centre - span / 2, hi = centre + span / 2;
        ASSERT_TRUE(fitPriceToRows(lo, hi, heightDev, tick));
        const double rows = heightDev * tick / (hi - lo);
        EXPECT_NEAR(rows, std::round(rows), 1e-9) << "i=" << i;
        EXPECT_GE(hi - lo, span * (1 - 1e-12)) << "i=" << i;
        RasterInputs in;
        in.tfMs = double(kMinute);
        in.tick = tick;
        in.itemWidthLogical = 640;
        in.itemHeightLogical = heightDev;
        in.dpr = 1.0;
        in.timeStart = kEpoch;
        in.timeEnd = kEpoch + 40 * kMinute;
        in.minPrice = lo;
        in.maxPrice = hi;
        in.anchorFracY = uniform(0, 1);
        const RasterCamera cam = computeRaster(in, {int(rng() % 8), 0});
        ASSERT_TRUE(cam.valid);
        EXPECT_EQ(cam.rowPx, int(std::round(rows)));
        EXPECT_NEAR(cam.drawnMaxPrice, hi, std::abs(hi) * 1e-12 + tick * 1e-9) << "i=" << i;
        EXPECT_NEAR(cam.drawnMinPrice, lo, std::abs(lo) * 1e-12 + tick * 1e-9) << "i=" << i;
    }
    double lo = 1, hi = 2;
    EXPECT_FALSE(fitPriceToRows(lo, hi, 100, 0.0)) << "no tick: unchanged";
    EXPECT_EQ(lo, 1);
    EXPECT_EQ(hi, 2);
}

// ---------------------------------------------------------------- smooth zoom (A2)
// Every click moves the column rung by at least one device pixel per direction (no
// dead click), toward currentPx * kZoomStepRatio^clicks, and stops at the limits.
TEST(ChartRasterZoom, EveryColumnClickMovesAWholePixelRung) {
    for (int px = 1; px <= 400; ++px) {
        const int in = columnRung(px, 1, 1, 2000), out = columnRung(px, -1, 1, 2000);
        EXPECT_GT(in, px) << px;
        EXPECT_NEAR(in, std::max(px + 1.0, std::round(px * kZoomStepRatio)), 0.0) << px;
        if (px > 1) {
            EXPECT_LT(out, px) << px;
            EXPECT_NEAR(out, std::min(px - 1.0, std::round(px / kZoomStepRatio)), 0.0) << px;
        } else {
            EXPECT_EQ(out, 1) << "one px per column is the floor";
        }
    }
    EXPECT_EQ(columnRung(16, 3, 1, 2000), 31) << "16 -> 20 -> 25 -> 31";
    EXPECT_EQ(columnRung(20, 1, 1, 22), 22) << "the zoom-in limit";
    EXPECT_EQ(columnRung(22, 1, 1, 22), 22) << "at the limit: no step";
    EXPECT_EQ(columnRung(4, -1, 3, 0), 3);
    EXPECT_EQ(columnRung(3, -1, 3, 0), 3);
    EXPECT_EQ(columnRung(2, -1, 3, 0), 2) << "below the floor already: never a step the wrong way";
    EXPECT_EQ(nearestColumnRung(7.49, 1, 0), 7);
    EXPECT_EQ(nearestColumnRung(7.51, 1, 0), 8);
    EXPECT_EQ(nearestColumnRung(0.2, 1, 0), 1);
    EXPECT_EQ(nearestColumnRung(30.0, 1, 12), 12);
}

// Price rungs land on whole px per row at the tick the chart draws there. With one
// tick: P -> next whole P toward x1.25. With an Auto-like tick (finer rows switch the
// tick when rows grow past 2x its minimum), a click across the switch lands on the
// predicted tick (2 px -> 4 px, as TapeSurf's Auto tick shows).
TEST(ChartRasterZoom, PriceClicksLandOnWholeRowsAtThePredictedTick) {
    const int H = 600;
    const TickAt fixed = [](double) { return 5.0; };
    RowRung r{5.0, 2};
    std::vector<int> seen;
    for (int i = 0; i < 8; ++i) {
        const RowRung next = rowRung(r, 1, H, 0, 0, fixed);
        EXPECT_EQ(next.tick, 5.0);
        EXPECT_GT(next.rowPx, r.rowPx) << "every click moves";
        r = next;
        seen.push_back(r.rowPx);
    }
    EXPECT_EQ(seen, (std::vector<int>{3, 4, 5, 6, 8, 10, 13, 16}));
    for (int i = 0; i < 20; ++i) {
        const RowRung next = rowRung(r, -1, H, 0, 0, fixed);
        if (next == r) break;
        EXPECT_LT(next.rowPx, r.rowPx);
        r = next;
    }
    EXPECT_EQ(r.rowPx, 1) << "zoom-out ends at one row per pixel";
    // Auto: $10 rows below 4 px switch to $5 rows... modelled as: span s -> the finest
    // tick in {1, 2, 5, 10} whose rows are at least 2 px.
    const TickAt autoTick = [&](double span) {
        for (const double t : {1.0, 2.0, 5.0, 10.0})
            if (double(H) * t / span >= 2.0 - 1e-9) return t;
        return 10.0;
    };
    RowRung a{10.0, 3}; // $10 rows of 3 px (span $2000)
    const RowRung in = rowRung(a, 1, H, 0, 0, autoTick);
    // x1.25 aims at 3.75 px of $10 = 1.875 px of $5: Auto draws $5 there only from 2 px.
    EXPECT_EQ(in.tick, autoTick(double(H) * in.tick / in.rowPx)) << "the rung is drawn at its tick";
    EXPECT_GT(double(in.rowPx) / in.tick, 3.0 / 10.0) << "zoomed in";
    RowRung b{5.0, 3};
    const RowRung outB = rowRung(b, -1, H, 0, 0, autoTick);
    EXPECT_EQ(outB.tick, autoTick(double(H) * outB.tick / outB.rowPx));
    EXPECT_LT(double(outB.rowPx) / outB.tick, 3.0 / 5.0) << "zoomed out";
    // The limits: a span beyond maxSpan is no rung.
    const RowRung limited = rowRung({5.0, 2}, -1, H, 0, double(H) * 5.0 / 2.0, fixed);
    EXPECT_EQ(limited, (RowRung{5.0, 2})) << "at the zoom-out limit: no step";
    // A gesture's end: the nearest whole row at the predicted tick.
    const RowRung near = nearestRowRung(2.38 / 5.0, H, 0, 0, fixed);
    EXPECT_EQ(near, (RowRung{5.0, 2}));
    EXPECT_EQ(nearestRowRung(2.62 / 5.0, H, 0, 0, fixed), (RowRung{5.0, 3}));
}

// The glide: eased, from `from` (e = 0: its scales and its anchor position) to `to`
// (e >= 1: exactly `to`, the landing frame is the rest frame); the anchor (the content
// under the cursor) moves linearly between where the two cameras draw it, so when both
// draw it at the cursor it never leaves it.
TEST(ChartRasterZoom, AGlideKeepsTheAnchorAndLandsOnTheRestCamera) {
    EXPECT_EQ(easeZoom(0), 0.0);
    EXPECT_EQ(easeZoom(1), 1.0);
    EXPECT_EQ(easeZoom(2), 1.0);
    double last = 0;
    for (int i = 1; i <= 20; ++i) {
        const double e = easeZoom(i / 20.0);
        EXPECT_GT(e, last);
        last = e;
    }
    EXPECT_GT(easeZoom(0.5), 0.8) << "ease-out: most of the way at half time";
    const double tick = 5, tf = double(kMinute);
    auto in = viewOf(2, 16, tick, tf, 640, 320, 1);
    in.anchorFracX = 0.3;
    in.anchorFracY = 0.7;
    const RasterCamera from = computeRaster(in, {});
    ASSERT_TRUE(from.valid);
    const double cursorX = 0.3 * 640, cursorY = 0.7 * 320;
    const double t = from.timeAtXDev(cursorX), p = from.priceAtYDev(cursorY);
    // The target rung about the cursor: 20 px columns, 3 px rows.
    RasterInputs target = in;
    const double span = 640.0 * tf / 20.0, priceSpan = 320.0 * tick / 3.0;
    target.timeStart = std::llround(t - 0.3 * span);
    target.timeEnd = target.timeStart + std::llround(span);
    target.maxPrice = p + 0.7 * priceSpan;
    target.minPrice = target.maxPrice - priceSpan;
    const RasterCamera to = computeRaster(target, from.step());
    ASSERT_TRUE(to.valid);
    ASSERT_EQ(to.colPx, 20);
    ASSERT_EQ(to.rowPx, 3);
    const RasterCamera start = glideRaster(from, to, t, p, 0.0);
    EXPECT_TRUE(start.free);
    EXPECT_NEAR(start.colPxF, 16, 1e-9);
    EXPECT_NEAR(start.rowPxF, 2, 1e-9);
    EXPECT_NEAR(start.drawnStartMs, from.drawnStartMs, 1e-3);
    EXPECT_NEAR(start.drawnMaxPrice, from.drawnMaxPrice, 1e-6);
    double lastCol = 16;
    for (int i = 1; i < 20; ++i) {
        const RasterCamera mid = glideRaster(from, to, t, p, easeZoom(i / 20.0));
        ASSERT_TRUE(mid.valid);
        EXPECT_TRUE(mid.free);
        EXPECT_GT(mid.colPxF, lastCol);
        lastCol = mid.colPxF;
        const double ax = from.xDev(t) + (to.xDev(t) - from.xDev(t)) * easeZoom(i / 20.0);
        EXPECT_NEAR(mid.xDev(t), ax, 1e-6) << "anchor time on its path";
        EXPECT_LE(std::abs(mid.xDev(t) - cursorX), 0.5 + 1e-9) << "within half a device px of the cursor";
        EXPECT_LE(std::abs(mid.yDev(p) - cursorY), 0.5 + 1e-9);
        const auto mapping = toMapping(mid);
        EXPECT_NEAR(mapping.cellW, mid.colPxF, 1e-12);
        EXPECT_NEAR(mapping.timeToScreenX(t), mid.xDev(t), 1e-6) << "the frame mapping is the glide camera";
    }
    const RasterCamera landed = glideRaster(from, to, t, p, 1.0);
    EXPECT_FALSE(landed.free);
    EXPECT_EQ(landed.leftColIndex, to.leftColIndex);
    EXPECT_EQ(landed.topRowIndex, to.topRowIndex);
    EXPECT_EQ(landed.drawnStartMs, to.drawnStartMs);
    EXPECT_EQ(landed.drawnMaxPrice, to.drawnMaxPrice);
    // A drag during a glide moves the free camera by whole device pixels.
    const RasterCamera moved = shiftedRaster(glideRaster(from, to, t, p, 0.5), 7, -3);
    const RasterCamera half = glideRaster(from, to, t, p, 0.5);
    EXPECT_NEAR(moved.xDev(t) - half.xDev(t), 7, 1e-9);
    EXPECT_NEAR(moved.yDev(p) - half.yDev(p), -3, 1e-9);
}

// Fix round 1, finding 5: batched clicks walk the same rungs as single clicks, in both
// directions, at every small size (the one-pixel rule applies per click).
TEST(ChartRasterZoom, BatchedClicksWalkTheSingleClickLadder) {
    const TickAt fixed = [](double) { return 10.0; };
    for (int px = 1; px <= 60; ++px)
        for (const int clicks : {2, 3, 4, -2, -3, -4}) {
            int single = px;
            RowRung row{10.0, px}, rowSingle = row;
            for (int i = 0; i < std::abs(clicks); ++i) {
                single = columnRung(single, clicks > 0 ? 1 : -1, 1, 2000);
                rowSingle = rowRung(rowSingle, clicks > 0 ? 1 : -1, 600, 0, 0, fixed);
            }
            EXPECT_EQ(columnRung(px, clicks, 1, 2000), single) << px << " px, " << clicks << " clicks";
            EXPECT_EQ(rowRung(row, clicks, 600, 0, 0, fixed), rowSingle) << px << " px rows, " << clicks << " clicks";
        }
    EXPECT_EQ(columnRung(1, 4, 1, 2000), 5) << "1 -> 2 -> 3 -> 4 -> 5";
    EXPECT_EQ(rowRung({10.0, 2}, 2, 600, 0, 0, fixed), (RowRung{10.0, 4})) << "2 -> 3 -> 4";
    EXPECT_EQ(columnRung(3, 4, 1, 5), 5) << "a batch stops at the limit";
}

// Fix round 1, finding 3: a glide whose start was drawn at another device pixel ratio
// (a move to another screen) puts the start on the target's surface: the anchor keeps
// its logical position (it is not interpolated across two device coordinate systems).
TEST(ChartRasterZoom, AGlideAcrossADevicePixelRatioChangeKeepsTheAnchor) {
    const double tick = 5, tf = double(kMinute);
    auto in = viewOf(2, 16, tick, tf, 640, 320, 1);
    in.anchorFracX = 0.5;
    in.anchorFracY = 0.5;
    const RasterCamera from = computeRaster(in, {});
    ASSERT_TRUE(from.valid);
    const double t = from.timeAtXDev(320), p = from.priceAtYDev(160);
    RasterInputs target = in; // the same view at dpr 2 (device px double)
    target.dpr = 2;
    const RasterCamera to = computeRaster(target, {});
    ASSERT_TRUE(to.valid);
    ASSERT_EQ(to.widthDev, 1280);
    // The anchor's logical position moves linearly from where `from` draws it (320, 160)
    // to where `to` draws it (within half a device pixel of each other).
    const double x1 = to.xDev(t) / 2, y1 = to.yDev(p) / 2;
    ASSERT_LE(std::abs(x1 - 320), 0.75);
    for (int i = 0; i <= 10; ++i) {
        const double e = i / 10.0;
        const RasterCamera mid = glideRaster(from, to, t, p, e);
        ASSERT_TRUE(mid.valid);
        EXPECT_EQ(mid.dpr, 2.0);
        EXPECT_NEAR(mid.xDev(t) / mid.dpr, 320 + (x1 - 320) * e, 1e-6) << "e " << e;
        EXPECT_NEAR(mid.yDev(p) / mid.dpr, 160 + (y1 - 160) * e, 1e-6) << "e " << e;
    }
    const RasterCamera moved = onSurface(from, 2, 1280, 640);
    EXPECT_NEAR(moved.xDev(t), 2 * from.xDev(t), 1e-9);
    EXPECT_NEAR(moved.drawnStartMs, from.drawnStartMs, 1e-6) << "the same logical picture";
}

// The continuous camera (a pinch): fractional px per row and column from the stored
// view, the anchor exactly at its fraction.
TEST(ChartRasterZoom, TheContinuousCameraDrawsTheStoredViewExactly) {
    auto in = viewOf(2.37, 13.61, 5, double(kMinute), 640, 320, 2);
    in.anchorFracX = 0.25;
    in.anchorFracY = 0.6;
    const RasterCamera cam = continuousRaster(in);
    ASSERT_TRUE(cam.valid);
    EXPECT_TRUE(cam.free);
    EXPECT_NEAR(cam.rowPxF, 2.37, 1e-9);
    EXPECT_NEAR(cam.colPxF, 1280.0 * kMinute / double(in.timeEnd - in.timeStart), 1e-9);
    EXPECT_NEAR(cam.drawnStartMs, double(in.timeStart), 1e-3);
    EXPECT_NEAR(cam.drawnEndMs, double(in.timeEnd), 1e-3);
    EXPECT_NEAR(cam.drawnMaxPrice, in.maxPrice, 1e-6);
    EXPECT_NEAR(cam.drawnMinPrice, in.minPrice, 1e-6);
}

// Follow-live: no shift within one drawn pixel of the target (or past it), else whole
// buckets.
TEST(ChartRasterZoom, FollowShiftsByWholeBucketsOutsideOneDrawnPixel) {
    const int64_t tf = kMinute;
    EXPECT_EQ(followShift(1'000'000, 1'000'000, tf, 7), 0);
    EXPECT_EQ(followShift(1'000'000, 999'000, tf, 7), 0) << "past the target";
    EXPECT_EQ(followShift(1'000'000, 1'000'000 + 8'572, tf, 7), 0) << "within ceil(tf / C) ms";
    EXPECT_EQ(followShift(1'000'000, 1'000'000 + 8'573, tf, 7), tf) << "one bucket";
    EXPECT_EQ(followShift(1'000'000, 1'000'000 + tf + 1, tf, 7), 2 * tf);
    EXPECT_EQ(followShift(1'000'000, 1'000'000 + 123, tf, 0), 123) << "no camera: exact";
    // Astra's case after the 1m -> 5m switch (C = 7): the end keeps the drawn 49 px of
    // padding (2,100,000 ms), the nominal padding is 2,121,429 ms: inside one drawn
    // pixel (42,858 ms), so follow-live leaves the Now column where it is.
    const int64_t liveEnd = 1'790'000'100'000 / (5 * kMinute) * (5 * kMinute);
    EXPECT_EQ(followShift(liveEnd + 2'100'000, liveEnd + 2'121'429, 5 * kMinute, 7), 0);
}
// A3 feasibility witness: dropping the two coarse outer columns on one zoom-in
// changes Auto's admissible ticks. All candles (including their 8% margin) need
// a price span of 180. The previous, valid fit is 200 at tick 2.5 / P=4.
// For every span in [180, 200], Auto chooses tick 2. Its adjacent whole-row
// spans are 160 (clips) and 213 1/3 (widens): relaxed T2 must choose the latter.
TEST(ChartRasterZoom, A3ResolutionBoundaryChoosesTheTightestContainingFit) {
    using namespace heatmap;
    constexpr int H = 320;
    constexpr double centre = 100000, need = 180, previous = 200;
    ResolutionSummary summary;
    summary.tfMs = kMinute;
    summary.priceScale = 100;
    for (int i = 0; i < 10; ++i) {
        const int64_t common = i == 0 || i == 9 ? 250 : 1;
        summary.columns.push_back({i * kMinute,
            {{"fixture", BucketState::Present, {common}, common, {{0, 20000000}}}}});
    }
    double lo = centre - need / 2, hi = centre + need / 2;
    ASSERT_TRUE(fitPriceToRows(lo, hi, H, 2.5));
    ASSERT_DOUBLE_EQ(hi - lo, previous);
    ASSERT_EQ(autoTickUnits(summary, 250, 0, 10 * kMinute, lo, hi, H), 250);
    ASSERT_EQ(columnRung(64, 1, 1, 160), 80); // 10m -> 8m, centred: drops exactly the outer columns
    ASSERT_EQ(autoTickUnits(summary, 250, kMinute, 9 * kMinute, lo, hi, H), 200);
    const auto solved = solveAutoPriceFit(centre - need / 2, centre + need / 2, H, 2.5, 1, previous,
        [&](double low, double high) {
            return fromUnits(autoTickUnits(summary, 250, kMinute, 9 * kMinute, low, high, H), 100);
        });
    ASSERT_TRUE(solved);
    EXPECT_EQ(solved->tick, 2);
    EXPECT_EQ(solved->rowPx, 3);
    EXPECT_NEAR(solved->hi - solved->lo, 640.0 / 3, 1e-9);
    EXPECT_LE(solved->lo, centre - need / 2);
    EXPECT_GE(solved->hi, centre + need / 2);
    EXPECT_GT(solved->hi - solved->lo, previous); // containment wins, in one glide
    EXPECT_EQ(autoTickUnits(summary, 250, kMinute, 9 * kMinute, solved->lo, solved->hi, H), 200);
}

TEST(ChartRasterZoom, A3SolvesTheTickBeforePublishingTheFit) {
    const auto predict = [](double lo, double hi) {
        return heatmap::fromUnits(heatmap::autoTickUnits(50, 1, (hi - lo) * 100 / 320), 100);
    };
    const auto fit = solveAutoPriceFit(99964 - 72 * .08, 100036 + 72 * .08, 320, .5, -1, 80, predict);
    ASSERT_TRUE(fit);
    EXPECT_EQ(fit->tick, 1);
    EXPECT_EQ(fit->rowPx, 3);
    EXPECT_NEAR(fit->hi - fit->lo, 320.0 / 3, 1e-9);
    EXPECT_EQ(predict(fit->lo, fit->hi), fit->tick);
    EXPECT_GT(fit->iterations, 1);
}

TEST(ChartRasterZoom, A3PrefersAMonotonicContainingFitWhenOneExists) {
    const auto predict = [](double lo, double hi) {
        return heatmap::fromUnits(heatmap::autoTickUnits(100, 1, (hi - lo) * 100 / 320), 100);
    };
    const auto out = solveAutoPriceFit(99950, 100050, 320, 1, -1, 160, predict);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->tick, 1);
    EXPECT_EQ(out->rowPx, 2);
    EXPECT_DOUBLE_EQ(out->hi - out->lo, 160); // tight fit is 106 2/3, but narrows
    const auto in = solveAutoPriceFit(99950, 100050, 320, 1, 1, 160, predict);
    ASSERT_TRUE(in);
    EXPECT_EQ(in->rowPx, 3);
    EXPECT_NEAR(in->hi - in->lo, 320.0 / 3, 1e-9);
}

TEST(ChartRasterZoom, A3RepeatingAFractionalFitDoesNotAddARowRung) {
    const auto predict = [](double lo, double hi) {
        return heatmap::fromUnits(heatmap::autoTickUnits(100, 1, (hi - lo) * 100 / 320), 100);
    };
    const auto first = solveAutoPriceFit(99960, 100040, 320, 1, -1, 80, predict);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->rowPx, 4);
    const auto fractional = solveAutoPriceFit(99955, 100045, 320, 1, -1, first->hi - first->lo, predict);
    ASSERT_TRUE(fractional);
    ASSERT_EQ(fractional->rowPx, 3);
    const auto again = solveAutoPriceFit(99955, 100045, 320, 1, -1, fractional->hi - fractional->lo, predict);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->rowPx, 3);
    EXPECT_DOUBLE_EQ(again->lo, fractional->lo);
    EXPECT_DOUBLE_EQ(again->hi, fractional->hi);
}

TEST(ChartRasterZoom, A3CycleKeepsTheWidestCandidateAndRechecksAuto) {
    const auto predict = [](double lo, double hi) {
        return heatmap::fromUnits(heatmap::autoTickUnits(500, 1, (hi - lo) * 100 / 320), 100);
    };
    // Raw 300: tick 5 / P5 / span320 predicts 2.5; tick 2.5 / P2 /
    // span400 predicts 5. Keeping span400 breaks the cycle at tick5 / P4.
    const auto fit = solveAutoPriceFit(99850, 100150, 320, 5, 1, 500, predict);
    ASSERT_TRUE(fit);
    EXPECT_TRUE(fit->cycleBroken);
    EXPECT_EQ(fit->tick, 5);
    EXPECT_EQ(fit->rowPx, 4);
    EXPECT_DOUBLE_EQ(fit->hi - fit->lo, 400);
    EXPECT_EQ(predict(fit->lo, fit->hi), fit->tick);
    EXPECT_LE(fit->iterations, 16);
}

TEST(ChartRasterZoom, A3FitsContainBothMarginEdgesIncludingThePixelPhase) {
    std::mt19937_64 rng(20261009);
    for (int i = 0; i < 1000; ++i) {
        const double scale = i % 2 ? 100 : 1e10;
        const double centre = i % 2 ? 100000.00123 : .00001000123;
        const int64_t current = heatmap::presetLadderUnits()[4 + rng() % 12];
        const double tick = heatmap::fromUnits(current, scale);
        const double rawSpan = tick * (20 + rng() % 500);
        const int H = 320 + int(rng() % 481);
        const auto fit = solveAutoPriceFit(centre - rawSpan / 2, centre + rawSpan / 2, H, tick,
            i % 2 ? 1 : -1, rawSpan * 1.1, [&](double lo, double hi) {
                return heatmap::fromUnits(heatmap::autoTickUnits(current, 1, (hi - lo) * scale / H), scale);
            });
        ASSERT_TRUE(fit) << i;
        EXPECT_LE(fit->lo, centre - rawSpan / 2);
        EXPECT_GE(fit->hi, centre + rawSpan / 2);
        EXPECT_NEAR(H * fit->tick / (fit->hi - fit->lo), fit->rowPx, 1e-5);
    }
}

// CPU reduction of the A2 fit -> Auto proposal -> commit/refit loop. Uniform
// native cents build every offered tick; candles are minute bars centred at zero.
// Keep the original diagnostic's synthetic 8% margin for before/after comparison;
// the renderer integration tests use production kFitPriceMargin (6%), unchanged.
TEST(ChartRasterZoom, A3Trace) {
    constexpr int H = 320;
    int C = 16;
    while (C < 160) C = columnRung(C, 1, 1, 160); // zoom in to the four-column floor
    int64_t tick = 50;
    struct Candle { int minute; double lo, hi; };
    std::vector<Candle> candles;
    for (int i = -64; i < 64; ++i) {
        const double radius = 8 + 7 * std::max(std::abs(i), std::abs(i + 1));
        candles.push_back({i, 100000 - radius, 100000 + radius});
    }
    auto range = [&](int col) {
        const double half = 320.0 / col;
        double lo = 100000, hi = 100000;
        for (const auto& bar : candles)
            if (bar.minute < half && bar.minute + 1 > -half) {
                lo = std::min(lo, bar.lo);
                hi = std::max(hi, bar.hi);
            }
        return std::pair{lo, hi};
    };
    auto fit = [&](int col, int64_t t) {
        const auto [low, high] = range(col);
        const double margin = (high - low) * 0.08;
        double lo = low - margin, hi = high + margin;
        fitPriceToRows(lo, hi, H, double(t) / 100);
        return std::pair{lo, hi};
    };
    auto predict = [&](std::pair<double, double> w) {
        return heatmap::autoTickUnits(tick, 1, (w.second - w.first) * 100 / H);
    };
    for (int warm = 0; warm < 8; ++warm) tick = predict(fit(C, tick));
    const auto flags = std::cout.flags();
    const auto precision = std::cout.precision();
    std::cout << std::fixed << std::setprecision(5);
    for (int click = 0; click < 16; ++click) {
        C = columnRung(C, click < 8 ? -1 : 1, 1, 160);
        const auto [low, high] = range(C);
        std::cout << "TRACE " << (click < 8 ? "out" : "in") << click % 8 + 1
                  << " minutes=" << 640.0 / C << " candles=[" << low << "," << high << "]";
        int rebases = 0;
        auto w = fit(C, tick);
        for (int round = 0; round < 8; ++round) {
            const auto next = predict(w);
            std::cout << " fit=[" << w.first << "," << w.second << "] tick=" << tick
                      << " predicted=" << next << " P=" << H * (double(tick) / 100) / (w.second - w.first);
            if (next == tick) break;
            tick = next;
            const auto after = fit(C, tick);
            rebases += after != w;
            w = after;
        }
        std::cout << " modeledGlides=1 fitRetargets=" << rebases << " final=[" << w.first << "," << w.second << "]\n";
    }
    C = 160;
    tick = 50;
    double previous = fit(C, tick).second - fit(C, tick).first;
    for (int click = 0; click < 16; ++click) {
        const int direction = click < 8 ? -1 : 1;
        C = columnRung(C, direction, 1, 160);
        const auto [lo, hi] = range(C);
        const double margin = (hi - lo) * .08;
        const auto solved = solveAutoPriceFit(lo - margin, hi + margin, H, double(tick) / 100,
            direction, previous, [&](double low, double high) {
                return heatmap::fromUnits(predict({low, high}), 100);
            });
        ASSERT_TRUE(solved);
        EXPECT_EQ(double(predict({solved->lo, solved->hi})) / 100, solved->tick);
        tick = heatmap::toUnits(solved->tick, 100); // commit; a second prediction must be unchanged
        EXPECT_EQ(double(predict({solved->lo, solved->hi})) / 100, solved->tick);
        EXPECT_LE(solved->lo, lo - margin);
        EXPECT_GE(solved->hi, hi + margin);
        std::cout << "SOLVED " << (direction < 0 ? "out" : "in") << click % 8 + 1
                  << " minutes=" << 640.0 / C << " final=[" << solved->lo << "," << solved->hi
                  << "] tick=" << solved->tick << " P=" << solved->rowPx
                  << " iterations=" << solved->iterations << " cycle=" << solved->cycleBroken << "\n";
        previous = solved->hi - solved->lo;
    }
    auto rest = fit(C, tick);
    int changes = 0;
    for (int update = 0; update < 16; ++update) {
        // Real revisions to a visible candle, all inside the existing extrema.
        candles[64].hi += 0.1;
        auto next = fit(C, tick);
        changes += next != rest;
        rest = next;
    }
    EXPECT_EQ(changes, 0);
    candles[64].hi = 100100; // a new visible high
    EXPECT_NE(fit(C, tick), rest);
    std::cout << "REST refits changed=" << changes << "/16 interior revisions; 1/1 new extreme\n";
    std::cout.flags(flags);
    std::cout.precision(precision);
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
