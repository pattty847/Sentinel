// S6b: GridViewState clamps (interaction spec rules 1, 2 and 9). The GPU heatmap
// sets maximum spans; wheel (handleZoomWithSensitivity), axis drags/wheels
// (handleTime/PriceZoomWithSensitivity, what the QML axis MouseAreas call) and the
// Agent API / direct viewport (setViewport) must all obey them. Without spans
// (legacy renderer) nothing is clamped.
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

TEST(GridViewStateClamps, WheelZoomOutStopsAtTheMaxSpansAboutTheAnchor) {
    View v;
    v.state.setMaxSpans(1000 * kMinute, 5000); // one column per pixel at 1m; Manual $10 over 500 px
    const QPointF anchor(250, 100);            // a quarter across, a fifth down
    const double anchorTime = v.state.getVisibleTimeStart() + 0.25 * v.time();
    const double anchorPrice = v.state.getMinPrice() + 0.8 * v.price();
    for (int i = 0; i < 80; ++i) v.state.handleZoomWithSensitivity(-120, anchor, QSizeF(1000, 500));
    EXPECT_EQ(v.time(), 1000.0 * kMinute);
    EXPECT_DOUBLE_EQ(v.price(), 5000);
    // The point under the cursor stays put (integer ms rounding drifts a few ms; a pixel is 60 s).
    EXPECT_NEAR(v.state.getVisibleTimeStart() + 0.25 * v.time(), anchorTime, 100.0);
    EXPECT_NEAR(v.state.getMinPrice() + 0.8 * v.price(), anchorPrice, 0.01);
    v.state.handleZoomWithSensitivity(-120, anchor, QSizeF(1000, 500));
    EXPECT_EQ(v.time(), 1000.0 * kMinute) << "a wheel at the clamp keeps the spans";
    EXPECT_NEAR(v.price(), 5000, 1e-9);
    // Zooming back in still works at once.
    v.state.handleZoomWithSensitivity(120, anchor, QSizeF(1000, 500));
    EXPECT_LT(v.time(), 1000.0 * kMinute);
    EXPECT_LT(v.price(), 5000);
}

TEST(GridViewStateClamps, AxisDragsObeyTheSameClamps) {
    View v;
    v.state.setMaxSpans(1000 * kMinute, 5000);
    // Time axis drag (zoomTimeAt): many drag steps outwards.
    for (int i = 0; i < 200; ++i) v.state.handleTimeZoomWithSensitivity(-24 * 10, 500, 1000);
    EXPECT_EQ(v.time(), 1000.0 * kMinute);
    // Price axis drag/wheel (zoomPriceAt).
    for (int i = 0; i < 200; ++i) v.state.handlePriceZoomWithSensitivity(-24 * 10, 250, 500);
    EXPECT_DOUBLE_EQ(v.price(), 5000);
}

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
    View v; // the legacy renderer sets no limits
    for (int i = 0; i < 20; ++i) v.state.handleZoomWithSensitivity(-120, QPointF(500, 250), QSizeF(1000, 500));
    EXPECT_GT(v.time(), 1000.0 * kMinute);
    v.state.setViewport(0, 9000 * kMinute, 1, 900'000);
    EXPECT_EQ(v.time(), 9000.0 * kMinute);
    EXPECT_DOUBLE_EQ(v.price(), 899'999);
}

// Owner bug (2026-10-02): the wheel stopped zooming out at a relative zoom factor
// floor (10x the start span), far below the 1 column/px limit. The spans are zoomed
// and clamped directly: from any start the wheel reaches the max spans exactly.
TEST(GridViewStateZoom, WheelZoomOutReachesTheMaxSpansFromAnyStart) {
    for (const qint64 startMinutes : {2, 10, 60, 600}) {
        GridViewState state;
        state.setViewportSize(1000, 500);
        state.setMaxSpans(1000 * kMinute, 5000);
        state.setMinSpans(4 * kMinute, 2);
        state.setViewport(0, startMinutes * kMinute, 100'000, 100'000 + startMinutes); // $1 per minute
        for (int i = 0; i < 200; ++i) state.handleZoomWithSensitivity(-120, QPointF(500, 250), QSizeF(1000, 500));
        EXPECT_EQ(state.getVisibleTimeEnd() - state.getVisibleTimeStart(), 1000 * kMinute) << startMinutes << " min";
        EXPECT_DOUBLE_EQ(state.getMaxPrice() - state.getMinPrice(), 5000) << startMinutes << " min";
    }
}

// Owner bug: an axis zoom-out widened the view past the wheel's factor floor and the
// wider range became the new floor. After any axis zoom the wheel still zooms out
// up to the max spans.
TEST(GridViewStateZoom, AfterAnAxisZoomTheWheelStillZoomsOutToTheMaxSpans) {
    GridViewState state;
    state.setViewportSize(1000, 500);
    state.setMaxSpans(1000 * kMinute, 5000);
    state.setViewport(0, 10 * kMinute, 100'000, 100'010);
    const QSizeF size(1000, 500);
    auto time = [&] { return state.getVisibleTimeEnd() - state.getVisibleTimeStart(); };
    auto price = [&] { return state.getMaxPrice() - state.getMinPrice(); };
    for (int i = 0; i < 120; ++i) state.handleZoomWithSensitivity(-120, QPointF(500, 250), size);
    ASSERT_EQ(time(), 1000 * kMinute);
    // Back in, then the axes widen the view; the wheel keeps going out from there.
    for (int i = 0; i < 40; ++i) state.handleZoomWithSensitivity(120, QPointF(500, 250), size);
    ASSERT_LT(time(), 100 * kMinute);
    for (int i = 0; i < 10; ++i) state.handleTimeZoomWithSensitivity(-240, 500, 1000);
    for (int i = 0; i < 10; ++i) state.handlePriceZoomWithSensitivity(-240, 250, 500);
    const qint64 afterAxisTime = time();
    const double afterAxisPrice = price();
    state.handleZoomWithSensitivity(-120, QPointF(500, 250), size);
    EXPECT_GT(time(), afterAxisTime) << "the wheel zooms out after an axis zoom";
    EXPECT_GT(price(), afterAxisPrice);
    for (int i = 0; i < 120; ++i) state.handleZoomWithSensitivity(-120, QPointF(500, 250), size);
    EXPECT_EQ(time(), 1000 * kMinute);
    EXPECT_DOUBLE_EQ(price(), 5000);
}

// Zoom-in stays bounded: wheel and axes stop at the minimum spans (gpu: a few
// columns and rows); a view already below them is not widened by a zoom-in.
TEST(GridViewStateZoom, ZoomInStopsAtTheMinimumSpans) {
    View v;
    v.state.setMaxSpans(1000 * kMinute, 5000);
    v.state.setMinSpans(4 * kMinute, 2.0);
    for (int i = 0; i < 300; ++i) v.state.handleZoomWithSensitivity(120, QPointF(300, 100), QSizeF(1000, 500));
    EXPECT_EQ(v.time(), 4.0 * kMinute);
    EXPECT_DOUBLE_EQ(v.price(), 2.0);
    v.state.setViewport(0, 600 * kMinute, 100'000, 100'500);
    for (int i = 0; i < 300; ++i) v.state.handleTimeZoomWithSensitivity(240, 500, 1000);
    for (int i = 0; i < 300; ++i) v.state.handlePriceZoomWithSensitivity(240, 250, 500);
    EXPECT_EQ(v.time(), 4.0 * kMinute);
    EXPECT_DOUBLE_EQ(v.price(), 2.0);
    v.state.setViewport(0, kMinute, 100'000, 100'001); // e.g. the Agent API: below the floors
    Counter changed(v.state);
    v.state.handleZoomWithSensitivity(120, QPointF(500, 250), QSizeF(1000, 500));
    EXPECT_EQ(changed.count(), 0) << "a zoom-in never widens";
    EXPECT_EQ(v.time(), 1.0 * kMinute);
    v.state.handleZoomWithSensitivity(-120, QPointF(500, 250), QSizeF(1000, 500));
    EXPECT_GT(v.time(), 1.0 * kMinute) << "zoom-out works from there";
}

TEST(GridViewStateClamps, OneViewportChangePerWheel) {
    View v;
    v.state.setMaxSpans(1000 * kMinute, 0);
    Counter changed(v.state);
    v.state.handleZoomWithSensitivity(120, QPointF(500, 250), QSizeF(1000, 500));
    v.state.handleTimeZoomWithSensitivity(120, 500, 1000);
    v.state.handlePriceZoomWithSensitivity(120, 250, 500);
    EXPECT_EQ(changed.count(), 3);
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
