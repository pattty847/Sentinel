#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include <gtest/gtest.h>
#include <sstream>

namespace {
struct Environment final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    chart::SurfaceFacts size{800, 400, 1};
    bool present = true, shown = true;
    bool visible() const override { return shown; }
    double mid = 100;
    int64_t live = 100000;
    chart::SpanLimits spanLimits{};
    std::function<void(const char*)> effect = [](const char*) {};
    bool attached() const override { return present; }
    chart::SurfaceFacts surface() const override { return size; }
    double drawnTick() const override { return 1; }
    double priceScale() const override { return 1; }
    bool manualTick() const override { return true; }
    bool hasCurrentResolution() const override { return true; }
    bool priceScaleCurrent() const override { return true; }
    chart::SpanLimits limits() const override { return spanLimits; }
    int64_t liveAnchorMs() const override { return live; }
    int64_t liveOpenEndMs() const override { return live; }
    int64_t oldestAvailableMs() const override { return 10000; }
    double recentMidPrice() const override { return mid; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 1; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return true; }
    int64_t autoTickState() const override { return 1; }
    heatmap::AutoTickParams autoTickParams() const override { return {1, .1}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { effect("autoTick"); return true; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override { effect("sink"); }
};
struct CommandsTest : testing::Test {
    Environment env;
    chart::PublishedFrame published;
    chart::ChartViewController c{env, env, env, published};
    GridViewState& v = *c.viewPtr();
    chart::ViewCommands& cmd = c.commands();
    using Request = chart::ViewportRequest;
    std::ostringstream trace;
    std::string events;
    void SetUp() override {
        c.session().timeframeMs = 1000;
        v.setViewportSize(800, 400);
        v.setAutoPriceScale(false);
        v.setViewport(0, 100000, 50, 150);
        v.enableAutoScroll(false);
        v.setRasterAnchor(.25, .75);
        c.zoomGlide().setClock([] { return 0; });
        auto effect = [this](const char* name) { events += std::string(name) + ','; };
        QObject::connect(&cmd, &chart::ViewCommands::updateRequested, &c, [effect] { effect("update"); });
        QObject::connect(&cmd, &chart::ViewCommands::liveRenderTick, &c, [effect] { effect("live"); });
        QObject::connect(&cmd, &chart::ViewCommands::heatmapTickSizeChanged, &c, [effect] { effect("tick"); });
        QObject::connect(&cmd, &chart::ViewCommands::viewportChanged, &c, [effect] { effect("viewport"); });
        env.effect = effect;
        c.priceFitter().setRequestUpdate([effect] { effect("update"); });
        QObject::connect(&v, &GridViewState::viewportChanged, &c, [this, effect] { effect("viewport"); cmd.onViewportChanged(); });
        QObject::connect(&cmd, &chart::ViewCommands::autoPriceScaleChanged, &c, [effect] { effect("auto"); });
        QObject::connect(&v, &GridViewState::autoPriceScaleChanged, &cmd, &chart::ViewCommands::onAutoPriceScaleChanged);
        QObject::connect(&v, &GridViewState::autoPriceScaleChanged, &cmd, &chart::ViewCommands::clearAutoPriceTick);
        QObject::connect(&v, &GridViewState::autoScrollEnabledChanged, &c, [effect] { effect("follow"); });
        QObject::connect(&v, &GridViewState::panVisualOffsetChanged, &c, [effect] { effect("pan"); });
        QObject::connect(&v, &GridViewState::priceInteracted, &c, [this, effect] { effect("price"); c.priceCarry().priceInteracted(); });
        QObject::connect(&c.livePlacement(), &chart::LivePlacement::liveRenderTick, &c, [effect] { effect("live"); });
        QObject::connect(&c.livePlacement(), &chart::LivePlacement::updateRequested, &c, [effect] { effect("update"); });
        QObject::connect(&c.zoomGlide(), &chart::ZoomGlide::updateRequested, &c, [effect] { effect("glide-update"); });
        v.setPriceFit([this](qint64 start, qint64 end, double& lo, double& hi) { return c.priceFitter().autoPriceFit(start,end,lo,hi); });
        v.setPanShift([this](QPointF p, qint64& time, double& price) { return c.camera().panShift(p,time,price); });
    }
    void snapshot() { cmd.onHeatmapSnapshot(); c.priceFitter().refitAutoPrice(); }
    void live() {
        cmd.onHeatmapLive();
        cmd.refitAfterHeatmapLive();
    }
    // Fixed old-shell checkpoints: time/price bounds (hex floats), anchors, version,
    // auto/follow/placement/carry/gesture flags, drag offset, then ordered effects.
    void snap() {
        trace << v.getVisibleTimeStart() << ':' << v.getVisibleTimeEnd() << ':' << std::hexfloat
              << v.getMinPrice() << ':' << v.getMaxPrice() << ':' << v.rasterAnchor().fracX << ':' << v.rasterAnchor().fracY
              << ':' << v.getViewportVersion() << ':' << v.autoPriceScale() << v.isAutoScrollEnabled()
              << c.flags().priceKnown << c.flags().reseedPrice << c.flags().pristine << c.flags().selfViewport
              << bool(c.priceFitter().priceCarry()) << c.glide().gesture << v.isDragging()
              << ':' << v.getPanVisualOffset().x() << ':' << v.getPanVisualOffset().y() << ':' << events << '|';
        events.clear();
    }
    void pin(const char* expected) {
        EXPECT_EQ(trace.str(), expected);
    }
};
TEST_F(CommandsTest, SetViewport) {
    c.glide().gesture = true;
    c.flags().reseedPrice = true;
    c.priceFitter().priceCarry() = {2, .5};
    v.setAutoPriceScale(true); v.enableAutoScroll(true); events.clear();
    cmd.setViewport(1000, 101000, 60, 160); snap();
    cmd.setViewport(1000, 101000, 60, 160); snap();
    v.setMaxSpans(50000, 50); events.clear();
    cmd.setViewport(-10000, 190000, 10, 210); snap();
    cmd.setViewport(10, 5, 200, 100); snap(); // preserve the existing invalid-bound behavior
    pin(
        "1000:101000:0x1.ep+5:0x1.4p+7:0x1p-1:0x1p-1:4:001000000:0x0p+0:0x0p+0:auto,autoTick,update,follow,viewport,update,sink,update,sink,|"
        "1000:101000:0x1.ep+5:0x1.4p+7:0x1p-1:0x1p-1:4:001000000:0x0p+0:0x0p+0:update,sink,|"
        "65000:115000:0x1.54p+6:0x1.0ep+7:0x1p-1:0x1p-1:6:001000000:0x0p+0:0x0p+0:viewport,update,sink,update,sink,|"
        "10:5:0x1.9p+7:0x1.9p+6:0x1p-1:0x1p-1:7:001000000:0x0p+0:0x0p+0:viewport,update,sink,update,sink,|"
    );
}
TEST_F(CommandsTest, RequestFlags) {
    for (int bits = 0; bits < 16; ++bits) {
        Request r; r.autoScale = bool(bits & 1); r.followLive = bool(bits & 2);
        if (bits & 4) { r.priceMin = 50; r.priceMax = 150; }
        if (bits & 8) { r.startMs = 0; r.endMs = 100000; }
        EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    }
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:3:000000000:0x0p+0:0x0p+0:sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:3:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:011000000:0x0p+0:0x0p+0:auto,autoTick,update,follow,viewport,update,sink,sink,live,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:111000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,live,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:001000000:0x0p+0:0x0p+0:auto,autoTick,update,follow,sink,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:001000000:0x0p+0:0x0p+0:sink,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:001000000:0x0p+0:0x0p+0:sink,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:001000000:0x0p+0:0x0p+0:sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:5:001000000:0x0p+0:0x0p+0:sink,update,|"
    );
}
// Lock to mid and the auto price scale are exclusive through the request flags.
TEST_F(CommandsTest, LockMidRequestAndAutoScaleExcludeEachOther) {
    v.setAutoPriceScale(true);
    Request lockOn;
    lockOn.lockMid = true;
    EXPECT_TRUE(c.applyViewportRequest(lockOn).isEmpty());
    EXPECT_TRUE(c.midLock().enabled());
    EXPECT_FALSE(v.autoPriceScale());
    EXPECT_TRUE(c.snapshot().lockMid);
    Request autoOn;
    autoOn.autoScale = true;
    EXPECT_TRUE(c.applyViewportRequest(autoOn).isEmpty());
    EXPECT_TRUE(v.autoPriceScale());
    EXPECT_FALSE(c.midLock().enabled());
    EXPECT_FALSE(c.snapshot().lockMid);
    c.applyViewportRequest(lockOn);
    Request lockOff;
    lockOff.lockMid = false;
    c.applyViewportRequest(lockOff);
    EXPECT_FALSE(c.midLock().enabled());
    EXPECT_FALSE(v.autoPriceScale()) << "turning the lock off leaves the auto price scale off";
}

TEST_F(CommandsTest, RequestWindows) {
    Request r; r.startMs = 1000; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    r = {}; r.endMs = 95000; r.priceMin = 75; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    r = {}; r.priceMax = 200; r.autoScale = true; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    r = {}; r.followLive = true; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    env.live = 0; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    env.mid = 0; r.autoScale = true; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    pin(
        "1000:100000:0x1.9p+5:0x1.2cp+7:0x1p-1:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,|"
        "1000:95000:0x1.2cp+6:0x1.2cp+7:0x1p-1:0x1p-1:5:000000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,|"
        "1000:95000:0x1.ep+5:0x1.18p+7:0x1p-1:0x1p-1:6:101000000:0x0p+0:0x0p+0:auto,autoTick,update,viewport,update,sink,sink,update,|"
        "13520:107520:0x1.ep+5:0x1.18p+7:0x1p+0:0x1p-1:7:111000000:0x0p+0:0x0p+0:follow,autoTick,update,viewport,update,sink,sink,live,update,|"
        "13520:107520:0x1.ep+5:0x1.18p+7:0x1p+0:0x1p-1:7:111000000:0x0p+0:0x0p+0:autoTick,update,sink,live,update,|"
        "13520:107520:0x1.ep+5:0x1.18p+7:0x1p+0:0x1p-1:7:111000000:0x0p+0:0x0p+0:autoTick,update,sink,live,update,|"
    );
}
TEST_F(CommandsTest, RequestFits) {
    for (const char* fit : {"time", "price", "both", "default", "unknown"}) {
        Request r; r.fit = fit; EXPECT_TRUE(cmd.applyViewportRequest(r).isEmpty()); snap();
    }
    env.live = 0; Request r; r.fit = "time";
    EXPECT_EQ(cmd.applyViewportRequest(r), "fit_unavailable"); snap();
    pin(
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:autoTick,update,sink,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:5:111010000:0x0p+0:0x0p+0:follow,autoTick,update,viewport,update,sink,sink,live,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:6:111000000:0x0p+0:0x0p+0:autoTick,update,viewport,update,sink,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:6:111000000:0x0p+0:0x0p+0:|"
    );
}
TEST_F(CommandsTest, FitView) {
    EXPECT_FALSE(cmd.fitView(false,false)); snap();
    EXPECT_TRUE(cmd.fitView(true,false)); snap();
    EXPECT_TRUE(cmd.fitView(false,true)); snap();
    EXPECT_TRUE(cmd.fitView(true,true)); snap();
    env.live = 0; EXPECT_FALSE(cmd.fitView(true,true)); snap();
    env.live = 100000; env.mid = 0; EXPECT_FALSE(cmd.fitView(true,true)); snap();
    v.handlePanStart({0,0}); EXPECT_FALSE(cmd.fitView(false,true)); snap();
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:3:000000000:0x0p+0:0x0p+0:|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:autoTick,update,sink,update,|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000000:0x0p+0:0x0p+0:|"
        "9999:107826:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:101000001:0x0p+0:0x0p+0:|"
    );
}
TEST_F(CommandsTest, ResetView) {
    env.live = 0; EXPECT_FALSE(cmd.resetView()); snap();
    env.live = 100000; EXPECT_TRUE(cmd.resetView()); snap();
    v.setMaxSpans(40000, 0); events.clear(); EXPECT_TRUE(cmd.resetView()); snap();
    env.mid = 0; EXPECT_TRUE(cmd.resetView()); snap();
    v.handlePanStart({0,0}); EXPECT_FALSE(cmd.resetView()); snap();
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:3:000000000:0x0p+0:0x0p+0:|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:4:111010000:0x0p+0:0x0p+0:auto,follow,autoTick,update,viewport,update,sink,sink,live,update,|"
        "63200:103200:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:6:111010000:0x0p+0:0x0p+0:autoTick,update,viewport,update,sink,sink,live,update,|"
        "63200:103200:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:6:111010000:0x0p+0:0x0p+0:autoTick,update,sink,live,update,|"
        "63200:103200:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1p-1:6:111010001:0x0p+0:0x0p+0:|"
    );
}
TEST_F(CommandsTest, AutoPriceScale) {
    cmd.setAutoPriceScale(true); snap(); cmd.setAutoPriceScale(false); snap();
    env.mid = 0; cmd.setAutoPriceScale(true); snap();
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1p-1:3:101000000:0x0p+0:0x0p+0:auto,autoTick,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1p-1:3:001000000:0x0p+0:0x0p+0:auto,autoTick,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1p-1:3:101000000:0x0p+0:0x0p+0:auto,|"
    );
}
TEST_F(CommandsTest, PanDirections) {
    cmd.panLeft(); snap(); cmd.panRight(); snap(); cmd.panUp(); snap(); cmd.panDown(); snap();
    v.setAutoPriceScale(true); events.clear(); cmd.panUp(); snap(); cmd.panDown(); snap();
    cmd.resetZoom(); snap(); pin(
        "-10000:90000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:5:000000000:0x0p+0:0x0p+0:viewport,update,sink,update,|"
        "0:100000:0x1.ep+5:0x1.4p+7:0x1p-2:0x1.8p-1:6:000000000:0x0p+0:0x0p+0:viewport,update,sink,price,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:7:000000000:0x0p+0:0x0p+0:viewport,update,sink,price,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:7:100000000:0x0p+0:0x0p+0:update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:7:100000000:0x0p+0:0x0p+0:update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:7:100000000:0x0p+0:0x0p+0:viewport,update,sink,pan,update,|"
    );
}
TEST_F(CommandsTest, PanGesture) {
    v.enableAutoScroll(true); events.clear();
    env.shown = false; cmd.beginPanAt(0,0); cmd.updatePanAt(20,10); snap();
    env.shown = true; cmd.beginPanAt(100,100); snap(); cmd.updatePanAt(120,110); snap(); cmd.endPanAt(); snap();
    cmd.endPanAt(); snap();
    env.size.dpr = 2; v.setAutoPriceScale(true); events.clear();
    cmd.beginPanAt(0,0); cmd.updatePanAt(2.4,3.6); cmd.endPanAt(); snap();
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:3:010000000:0x0p+0:0x0p+0:|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:3:000000001:0x0p+0:0x0p+0:follow,update,|"
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:3:000000001:0x1.4p+4:0x1.4p+3:pan,update,|"
        "-2500:97500:0x1.a4p+5:0x1.31p+7:0x1p+0:0x1.8p-1:4:000000000:0x1.4p+4:0x1.4p+3:viewport,update,sink,price,update,|"
        "-2500:97500:0x1.a4p+5:0x1.31p+7:0x1p+0:0x1.8p-1:4:000000000:0x1.4p+4:0x1.4p+3:update,|"
        "-2813:97187:0x1.a4p+5:0x1.31p+7:0x1p+0:0x1.8p-1:5:100000000:0x1.3333333333333p+1:0x0p+0:update,pan,autoTick,update,update,autoTick,update,viewport,update,sink,update,|"
    );
}
TEST_F(CommandsTest, HeatmapSnapshot) {
    c.flags().priceKnown = false; snapshot(); snap();
    snapshot(); snap();
    c.flags().reseedPrice = true; env.mid = 120; snapshot(); snap();
    env.mid = 0; snapshot(); snap(); pin(
        "0:100000:0x1.8666666666666p+5:0x1.2e66666666666p+7:0x1p-2:0x1p-1:4:001000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,update,|"
        "0:100000:0x1.8666666666666p+5:0x1.2e66666666666p+7:0x1p-2:0x1p-1:4:001000000:0x0p+0:0x0p+0:update,|"
        "0:100000:0x1.1333333333333p+6:0x1.5666666666666p+7:0x1p-2:0x1p-1:5:001000000:0x0p+0:0x0p+0:viewport,update,sink,sink,update,update,|"
        "0:100000:0x1.1333333333333p+6:0x1.5666666666666p+7:0x1p-2:0x1p-1:5:001000000:0x0p+0:0x0p+0:update,|"
    );
}
TEST_F(CommandsTest, HeatmapLive) {
    live(); snap();
    v.enableAutoScroll(true); events.clear(); live(); snap();
    env.live = 150000; v.setAutoPriceScale(true); events.clear(); live(); snap();
    v.handlePanStart({0,0}); live(); snap();
    pin(
        "0:100000:0x1.9p+5:0x1.2cp+7:0x1p-2:0x1.8p-1:3:000000000:0x0p+0:0x0p+0:live,update,|"
        "8000:108000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:4:010000000:0x0p+0:0x0p+0:viewport,update,sink,live,live,update,|"
        "58000:158000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:5:110000000:0x0p+0:0x0p+0:autoTick,update,viewport,update,sink,live,live,update,|"
        "58000:158000:0x1.9p+5:0x1.2cp+7:0x1p+0:0x1.8p-1:5:110000001:0x0p+0:0x0p+0:live,update,autoTick,update,|");
}
TEST_F(CommandsTest, HeatmapTick) {
    env.spanLimits = {1000, 50000, 10, 50}; cmd.onHeatmapTick(); snap();
    v.setAutoPriceScale(true); events.clear(); env.mid = 120; cmd.onHeatmapTick(); snap();
    c.flags().limitsDeferred = true; env.spanLimits = {1000, 25000, 10, 25}; cmd.onHeatmapTick(); snap(); pin(
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,sink,tick,viewport,update,|"
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:100000000:0x0p+0:0x0p+0:autoTick,update,sink,tick,viewport,update,|"
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:100000000:0x0p+0:0x0p+0:sink,tick,viewport,update,|"
    );
}
TEST_F(CommandsTest, HeatmapLimits) {
    env.spanLimits = {1000, 50000, 10, 50}; cmd.onHeatmapLimits(); snap();
    c.priceFitter().priceCarry() = {.5, 1}; c.flags().reseedPrice = true;
    cmd.onHeatmapLimits(); snap(); v.setAutoPriceScale(true); events.clear(); env.mid = 120;
    cmd.onHeatmapLimits(); snap(); pin(
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:000000000:0x0p+0:0x0p+0:viewport,update,sink,|"
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:000100100:0x0p+0:0x0p+0:|"
        "25000:75000:0x1.2cp+6:0x1.f4p+6:0x1p-2:0x1.8p-1:4:100100000:0x0p+0:0x0p+0:autoTick,update,|"
    );
}
}

TEST(ProductComparison, MatchesAllocatingUtf8ForEveryScalarAndMalformedInputs) {
    auto check = [](const std::string& bytes) {
        const QString decoded = QString::fromStdString(bytes);
        const QString latin = QString::fromLatin1(bytes.data(), qsizetype(bytes.size()));
        for (const QString& symbol : {decoded, decoded + QChar('!'), latin, QString(), QString(QChar(0xd800))}) {
            ASSERT_EQ(chart::LivePlacement::matchesUtf8Product(symbol, bytes), decoded == symbol)
                << "input=" << QByteArray(bytes.data(), qsizetype(bytes.size())).toHex().toStdString();
        }
    };
    check({});
    for (int a = 0; a < 256; ++a) {
        check(std::string(1, char(a)));
        for (int b = 0; b < 256; ++b) check(std::string{char(a), char(b)});
    }
    for (char32_t scalar = 0; scalar <= 0x10ffff; ++scalar) {
        if (scalar >= 0xd800 && scalar <= 0xdfff) continue;
        check(QString::fromUcs4(&scalar, 1).toStdString());
    }
    const std::string bom = "\xef\xbb\xbf";
    for (const std::string& bytes : {bom, bom + bom, bom + "BTC-USD", std::string("A\0B", 3),
                                    std::string("\xed\xa0\x80"), std::string("\xf0\x9f\x98"),
                                    std::string("\xf4\x90\x80\x80"), std::string("\xf0\x80\x80\x80")}) check(bytes);
    uint32_t seed = 17;
    for (int n = 0; n < 4096; ++n) {
        std::string bytes;
        for (int j = 0; j < 32; ++j) { seed = seed * 1664525u + 1013904223u; bytes += char(seed >> 24); }
        check(bytes); check(bom + bytes);
    }
}
