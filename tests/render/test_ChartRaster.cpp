// Whole-pixel chart mapping (docs/research/2026-10-08-whole-pixel-mapping.md section 5,
// checks A1-A8): the raster camera (render/ChartRaster.hpp) puts row and column edges on
// device pixel edges, moves a drag by whole device pixels, steps the integer pixels per
// row/column with a 0.1 px band, keeps the zoom anchor across a step, follows the device
// pixel ratio, never draws 0 px cells, commits a drag exactly as drawn (with
// GridViewState) and fits price on integer pixels per row. Pure CPU.
#include "render/ChartRaster.hpp"
#include "render/GridViewState.hpp"

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

// Review fix 4: a 1 ms stored window (any direct viewport is accepted) draws at most
// one ms per device pixel, and every whole-pixel drag then has an exact whole-ms commit.
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
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
