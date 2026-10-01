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
    // Zooming back in still works at once (the zoom factor was not used up).
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
