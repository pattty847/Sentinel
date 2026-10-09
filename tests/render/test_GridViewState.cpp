// S6b: GridViewState clamps (interaction spec rules 1, 2 and 9). The GPU heatmap
// sets maximum spans; every viewport (the zoom ladder and gestures, the Agent API,
// direct views) goes through setViewport, which clamps about the centre. Without
// spans nothing is clamped. The zoom input itself (smooth zoom, slice A2) is
// UnifiedGridRenderer's: its limits are tested in test_ugr_gpu.cpp.
#include "render/GridViewState.hpp"
#include <QCoreApplication>
#include <gtest/gtest.h>

namespace {
constexpr qint64 kMinute = 60'000;
struct Counter { // viewportChanged emissions
    int n = 0;
    explicit Counter(GridViewState &state) {
        QObject::connect(&state, &GridViewState::viewportChanged, &state, [this] { ++n; });
    }
    int count() const { return n; }
};
struct View {
    GridViewState state;
    View() {
        state.setViewportSize(1000, 500);
        state.setViewport(0, 600 * kMinute, 100'000, 100'500);
    }
    double time() const { return double(state.getVisibleTimeEnd() - state.getVisibleTimeStart()); }
    double price() const { return state.getMaxPrice() - state.getMinPrice(); }
};

TEST(GridViewStateClamps, DirectViewportsClampAboutTheirCentre) {
    View v;
    v.state.setMaxSpans(1000 * kMinute, 5000);
    Counter changed(v.state);
    v.state.setViewport(0, 5000 * kMinute, 90'000, 110'000); // e.g. POST /api/v1/viewport
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(v.time(), 1000.0 * kMinute);
    EXPECT_EQ(v.state.getVisibleTimeStart(), 2000 * kMinute);
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 97'500);
    EXPECT_DOUBLE_EQ(v.state.getMaxPrice(), 102'500);
}

TEST(GridViewStateClamps, NewLimitsReclampTheCurrentViewOnce) {
    View v; // 600 minutes
    Counter changed(v.state);
    const uint64_t version = v.state.getViewportVersion();
    v.state.setMaxSpans(300 * kMinute, 0); // e.g. a narrower chart or a finer timeframe
    EXPECT_EQ(changed.count(), 1);
    EXPECT_GT(v.state.getViewportVersion(), version);
    EXPECT_EQ(v.time(), 300.0 * kMinute);
    EXPECT_EQ(v.state.getVisibleTimeStart(), 150 * kMinute);
    v.state.setMaxSpans(300 * kMinute, 0);
    EXPECT_EQ(changed.count(), 1) << "unchanged limits do not touch the view";
}

TEST(GridViewStateClamps, WithoutLimitsNothingIsClamped) {
    View v; // no limits set
    v.state.setViewport(0, 9000 * kMinute, 1, 900'000);
    EXPECT_EQ(v.time(), 9000.0 * kMinute);
    EXPECT_DOUBLE_EQ(v.price(), 899'999);
}

// Auto price scale (docs/research/2026-10-viewport-autoscale.md): the fit replaces the
// price inside setViewport (one change), drags and keyboard pans are time only; off,
// drags pan price again.
struct AutoView : View {
    int fits = 0;
    bool haveFit = true;
    AutoView() {
        // A fit that depends on the time range: [start / 1e6, start / 1e6 + 100).
        state.setPriceFit([this](qint64 start, qint64, double &lo, double &hi) {
            ++fits;
            if (!haveFit) return false;
            lo = double(start) / 1e6;
            hi = lo + 100;
            return true;
        });
        state.setAutoPriceScale(true);
    }
};

TEST(GridViewStateAutoPrice, SetViewportTakesThePriceFromTheFitInOneChange) {
    AutoView v;
    Counter changed(v.state);
    v.state.setViewport(60 * kMinute, 600 * kMinute, 1, 2);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 3.6);
    EXPECT_DOUBLE_EQ(v.state.getMaxPrice(), 103.6);
    v.state.setViewport(60 * kMinute, 600 * kMinute, 7, 8);
    EXPECT_EQ(changed.count(), 1) << "the fit unchanged: no change";
    v.haveFit = false; // nothing to fit: the given price stays
    v.state.setViewport(60 * kMinute, 600 * kMinute, 7, 8);
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 7);
    v.state.setAutoPriceScale(false);
    v.haveFit = true;
    const int fits = v.fits;
    v.state.setViewport(0, 600 * kMinute, 9, 10);
    EXPECT_EQ(v.fits, fits) << "off: no fit";
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 9);
}

TEST(GridViewStateAutoPrice, DragsAndKeyboardPansMoveTimeOnly) {
    AutoView v;
    v.state.setViewport(0, 600 * kMinute, 0, 1); // fitted: 0..100
    int priceSignals = 0;
    QObject::connect(&v.state, &GridViewState::priceInteracted, &v.state, [&] { ++priceSignals; });
    v.state.handlePanStart(QPointF(500, 250));
    v.state.handlePanMove(QPointF(400, 400));
    EXPECT_EQ(v.state.getPanVisualOffset(), QPointF(-100, 0)) << "no vertical offset";
    v.state.handlePanEnd(true);
    EXPECT_EQ(v.state.getVisibleTimeStart(), 60 * kMinute);
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 3.6) << "the fit for the new time range";
    const uint64_t version = v.state.getViewportVersion();
    v.state.panUp();
    v.state.panDown();
    EXPECT_EQ(v.state.getViewportVersion(), version) << "no keyboard vertical pan";
    EXPECT_EQ(priceSignals, 0);
    EXPECT_TRUE(v.state.autoPriceScale());
}

TEST(GridViewStateAutoPrice, OffDragsPanPriceAgain) {
    AutoView v;
    v.state.setViewport(0, 600 * kMinute, 1, 2); // fitted: 0..100
    int offSignals = 0;
    QObject::connect(&v.state, &GridViewState::autoPriceScaleChanged, &v.state, [&] { ++offSignals; });
    v.state.setAutoPriceScale(false);
    EXPECT_EQ(offSignals, 1);
    v.state.handlePanStart(QPointF(500, 250));
    v.state.handlePanMove(QPointF(500, 300));
    EXPECT_EQ(v.state.getPanVisualOffset(), QPointF(0, 50));
    v.state.handlePanEnd(true);
}
TEST(GridViewStateAutoPrice, TheFitFollowsTheDisplayedWindowDuringADrag) {
    AutoView v;
    v.state.setViewport(0, 600 * kMinute, 0, 1); // fitted: 0..100
    Counter changed(v.state);
    v.state.handlePanStart(QPointF(500, 250));
    v.state.handlePanMove(QPointF(400, 250)); // 100 px of 1000: 60 minutes later on screen
    EXPECT_EQ(v.state.displayedTimeWindow(), std::make_pair(qint64(60 * kMinute), qint64(660 * kMinute)));
    EXPECT_EQ(v.state.getVisibleTimeStart(), 0) << "committed only at release";
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 3.6) << "fitted to the window on screen, before release";
    EXPECT_EQ(changed.count(), 1);
    v.state.handlePanMove(QPointF(400, 300)); // vertical only: no refit
    EXPECT_EQ(changed.count(), 1);
    v.state.handlePanEnd(false); // cancelled: the committed window's fit again
    EXPECT_DOUBLE_EQ(v.state.getMinPrice(), 0);
    EXPECT_EQ(v.state.displayedTimeWindow(), std::make_pair(qint64(0), qint64(600 * kMinute)));
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
