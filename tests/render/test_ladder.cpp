// The chart-edge book ladder: the GUI-thread feed (native-grid binning, completeness,
// invalidation, withdrawal, generation, suspension) and the render-thread layer
// (validation against the drawn frame, retained geometry, no per-frame allocation).
#include "chart/feeds/BookLadderFeed.hpp"
#include "chart/feeds/LadderTracker.hpp"
#include "chart/layers/ChartLayers.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/shell/LadderBinding.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "marketdata/model/TradeData.h"
#include "render/ChartTextRenderer.hpp"
#include "render/GridViewState.hpp"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QTest>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <tuple>
#include <vector>

// Allocation counter for the render-path assertions (test binary only).
namespace {
std::atomic<bool> gCounting{false};
std::atomic<size_t> gAllocations{0};
void* counted(std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
} // namespace
void* operator new(std::size_t n) { return counted(n); }
void* operator new[](std::size_t n) { return counted(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return gCounting ? (gAllocations++, std::malloc(n ? n : 1)) : std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using chart::LadderSnapshot;
constexpr int64_t kTf = 60'000;
QStringList gProbeLines;
int wallRadarWarnings = 0;

chart_raster::RasterCamera camera(double lo, double hi, double tick, double dpr = 1.0, double height = 320) {
    chart_raster::RasterInputs in;
    in.timeStart = 0;
    in.timeEnd = 60 * kTf;
    in.minPrice = lo;
    in.maxPrice = hi;
    in.tfMs = double(kTf);
    in.tick = tick;
    in.itemWidthLogical = 640;
    in.itemHeightLogical = height;
    in.dpr = dpr;
    return chart_raster::computeRaster(in, {});
}

std::unique_ptr<LiveOrderBook> book(double lo, double hi, double tick, const std::vector<BookLevelUpdate>& levels) {
    auto b = std::make_unique<LiveOrderBook>("TEST");
    b->initialize(lo, hi, tick);
    b->applyUpdates(levels, std::chrono::system_clock::time_point(std::chrono::milliseconds(1)), nullptr);
    return b;
}

float bidAt(const LadderSnapshot& s, int64_t k) {
    const int64_t i = k - s.binLo;
    return i >= 0 && i < s.rowCount ? s.rows[size_t(i)].bid : -1.0f;
}
float askAt(const LadderSnapshot& s, int64_t k) {
    const int64_t i = k - s.binLo;
    return i >= 0 && i < s.rowCount ? s.rows[size_t(i)].ask : -1.0f;
}

struct Rig {
    chart::LadderLayer layer;
    chart::BookLadderFeed feed{layer.mailbox()};
    MappingFrameContext frame;
    std::vector<std::pair<double, double>> tops;
    int updates = 0, requests = 0;
    int64_t now = 1000, mono = 0;
    Rig() {
        feed.setPorts({[this] { return frame; }, [this](double b, double a) { tops.emplace_back(b, a); },
                       [this] { ++updates; }, [this] { return now; }, [this] { return mono; }});
        layer.setRepublishRequest([this] { ++requests; });
    }
    void view(const chart_raster::RasterCamera& cam) { frame.raster = cam; }
    chart::ChartFrame chartFrame(double gutter = 72) const {
        chart::ChartFrame f;
        f.context.raster = frame.raster;
        f.context.rightGutterPx = gutter;
        f.context.itemBounds = QRectF(0, 0, 640, frame.raster.heightDev / frame.raster.dpr);
        f.context.surfaceBounds = f.context.itemBounds;
        f.context.surfaceDpr = frame.raster.dpr;
        return f;
    }
    const LadderSnapshot& sync(double gutter = 72) {
        layer.sync(chartFrame(gutter));
        return layer.current();
    }
    // A delta, then the timer's work, then the render sync.
    const LadderSnapshot& rows(const LiveOrderBook& b) {
        feed.publish(b);
        feed.publishRows();
        return sync();
    }
};

// ------------------------------------------------------------------ (2) binning
TEST(LadderFeed, BinsOnTheNativeServerGridLowerInclusiveUpperExclusive) {
    for (const double serverTick : {0.001, 0.01}) {
        SCOPED_TRACE(serverTick);
        Rig rig;
        rig.feed.setEnabled(true);
        const double a = 100.0, b = serverTick == 0.001 ? 100.049 : 100.04, c = 100.05;
        auto replica = book(99, 101, serverTick, {{true, a, 1}, {true, b, 2}, {false, c, 4}});
        rig.view(camera(99.9, 100.2, 0.05));
        const auto& s = rig.rows(*replica);
        ASSERT_TRUE(s.valid);
        ASSERT_TRUE(s.complete) << rig.feed.lastReason();
        EXPECT_EQ(s.group, llround(0.05 / serverTick));
        EXPECT_FLOAT_EQ(bidAt(s, 2000), 3.0f) << "100.000 and the level just under 100.05 share bin 2000";
        EXPECT_FLOAT_EQ(askAt(s, 2000), 0.0f);
        EXPECT_FLOAT_EQ(askAt(s, 2001), 4.0f) << "100.05 opens bin 2001";
        EXPECT_FLOAT_EQ(bidAt(s, 2001), 0.0f);
        // A one-pixel pan (fractional drawn edges): the same absolute bins.
        rig.view(chart_raster::shiftedRaster(rig.frame.raster, 0, 1));
        const auto& panned = rig.rows(*replica);
        ASSERT_TRUE(panned.valid && panned.complete);
        EXPECT_FLOAT_EQ(bidAt(panned, 2000), 3.0f);
        EXPECT_FLOAT_EQ(askAt(panned, 2001), 4.0f);
    }
}

TEST(LadderFeed, ADisplayTickThatIsNoWholeMultipleIsAnUnsupportedGrid) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(99, 101, 0.02, {{true, 100.0, 1}, {false, 100.02, 1}});
    rig.view(camera(99.5, 100.5, 0.03));
    const auto& s = rig.rows(*replica);
    EXPECT_FALSE(s.valid);
    EXPECT_STREQ(rig.feed.lastReason(), "grid");
}

TEST(LadderFeed, AScaleChangeWithdrawsBeforeTheNewRows) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(99, 101, 0.01, {{true, 100.0, 1}, {false, 100.05, 1}});
    rig.view(camera(99.5, 100.5, 0.05));
    ASSERT_TRUE(rig.rows(*replica).valid);
    replica->initialize(99, 101, 0.001); // a re-snapshot at another server tick
    const std::vector<BookLevelUpdate> levels{{true, 100.001, 2}, {false, 100.051, 3}};
    replica->applyUpdates(levels, {}, nullptr);
    rig.feed.publish(*replica);
    EXPECT_FALSE(rig.sync().valid) << "the old grid's rows are withdrawn at once";
    EXPECT_STREQ(rig.feed.lastReason(), "tick");
    EXPECT_TRUE(rig.feed.pending());
    rig.feed.publishRows();
    const auto& s = rig.sync();
    ASSERT_TRUE(s.valid);
    EXPECT_DOUBLE_EQ(s.serverTick, 0.001);
    EXPECT_FLOAT_EQ(bidAt(s, 2000), 2.0f);
    EXPECT_FLOAT_EQ(askAt(s, 2001), 3.0f);
}

// ------------------------------------------------------------- (3) invalidation
TEST(LadderLayer, AnotherTickHidesTheRowsAndAsksOnceForARepublish) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(99, 101, 0.01, {{true, 100.0, 1}, {false, 100.05, 1}});
    rig.view(camera(99.5, 100.5, 0.05));
    rig.rows(*replica);
    EXPECT_TRUE(rig.layer.drawing());
    rig.layer.afterRendering();
    EXPECT_EQ(rig.requests, 0);
    rig.view(camera(99.5, 100.5, 0.1)); // no delta
    QSGNode root;
    root.appendChildNode(rig.layer.createContainer());
    ChartTextRenderer text;
    ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    const auto frame = rig.chartFrame();
    rig.layer.sync(frame);
    rig.layer.updateNodes(frame, context);
    EXPECT_FALSE(rig.layer.drawing());
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.firstChild())->opacity(), 0.0);
    rig.layer.afterRendering();
    EXPECT_EQ(rig.requests, 1);
    rig.layer.sync(frame);
    rig.layer.afterRendering();
    EXPECT_EQ(rig.requests, 1) << "one request per snapshot";
}

// A2: the published raster already holds the drag; the feed adds nothing to it.
TEST(LadderFeed, AHeldDragIsCountedOnceAndNeverAccumulates) {
    Rig rig;
    rig.feed.setEnabled(true);
    std::vector<BookLevelUpdate> levels;
    for (int i = 0; i < 400; ++i) levels.push_back({i < 200, 98.0 + i * 0.01, 1.0 + i});
    auto replica = book(90, 110, 0.01, levels);
    const auto rest = camera(98, 102, 0.05); // 80 rows of 4 px
    rig.view(rest);
    const auto& first = rig.rows(*replica);
    ASSERT_TRUE(first.valid && first.complete) << rig.feed.lastReason();
    const int64_t restLo = first.binLo, restHi = first.binLo + first.rowCount - 1;
    // A held drag of 200 px (50 rows; beyond the margin) as the published frame carries it.
    const auto dragged = chart_raster::shiftedRaster(rest, 0, 200);
    rig.frame.raster = dragged;
    rig.frame.viewportDragging = true;
    rig.frame.viewportPanVisualOffset = QPointF(0, 200);
    rig.sync();
    EXPECT_FALSE(rig.layer.drawing()) << "the rest rows do not cover the dragged window";
    const auto window = chart::ladderWindow(dragged.drawnMinPrice, dragged.drawnMaxPrice, 0.05);
    ASSERT_TRUE(window.lo < restLo || window.hi > restHi) << "the drag moved past the old rows and margin";
    for (int publication = 0; publication < 3; ++publication) {
        rig.feed.publishRows();
        const auto& s = rig.sync();
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_EQ(s.evalLo, window.lo) << publication;
        EXPECT_EQ(s.evalHi, window.hi) << publication;
        EXPECT_LE(s.binLo, window.lo);
        EXPECT_GE(s.binLo + s.rowCount - 1, window.hi);
        EXPECT_TRUE(rig.layer.drawing()) << publication;
    }
}

// --------------------------------------------------------------- (4) withdrawal
TEST(LadderFeed, AnEmptyUpdateWithdrawsAndKeepsTheReceiveTime) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(99, 101, 0.01, {{true, 100.0, 1}, {false, 100.05, 1}});
    rig.view(camera(99.5, 100.5, 0.05));
    ASSERT_TRUE(rig.rows(*replica).valid);
    EXPECT_EQ(rig.layer.current().receivedAtMs, 1000);
    rig.now = 5000;
    replica->clear();
    rig.feed.publish(*replica);
    EXPECT_FALSE(rig.feed.pending());
    ASSERT_FALSE(rig.tops.empty());
    EXPECT_EQ(rig.tops.back(), (std::pair<double, double>{0.0, 0.0})) << "the top is withdrawn too";
    const auto& s = rig.sync();
    EXPECT_FALSE(s.valid);
    EXPECT_EQ(s.rowCount, 0);
    EXPECT_EQ(s.receivedAtMs, 1000) << "INV-130: a withdrawal is no receive";
    EXPECT_FALSE(rig.layer.drawing());
}

// --------------------------------------------------------------- (5) generation
TEST(LadderFeed, ASymbolGenerationDropsTheOldRowsAndTheirPendingPublish) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(99, 101, 0.01, {{true, 100.0, 1}, {false, 100.05, 1}});
    rig.view(camera(99.5, 100.5, 0.05));
    rig.rows(*replica);
    ASSERT_TRUE(rig.layer.drawing());
    rig.feed.publish(*replica); // an A publication pending
    ASSERT_TRUE(rig.feed.pending());
    const uint64_t published = rig.feed.rowPublications();
    rig.feed.beginGeneration(); // no B book ever arrives
    EXPECT_FALSE(rig.feed.pending());
    const auto& s = rig.sync();
    EXPECT_EQ(s.generation, 1u);
    EXPECT_FALSE(s.valid);
    EXPECT_EQ(s.rowCount, 0);
    EXPECT_FALSE(rig.layer.drawing());
    QTest::qWait(2 * chart::BookLadderFeed::kIntervalMs);
    EXPECT_EQ(rig.feed.rowPublications(), published) << "the A publication never ran";
    // A late snapshot of the old generation is rejected by the layer.
    auto& late = rig.layer.mailbox().back();
    late.generation = 0;
    late.valid = late.complete = true;
    late.serverTick = 0.01;
    late.group = 5;
    late.binLo = 1990;
    late.rowCount = 20;
    rig.layer.mailbox().post();
    EXPECT_EQ(rig.sync().generation, 1u);
    EXPECT_FALSE(rig.layer.drawing());
}

// --------------------------------------------------------------- (6) global top
TEST(LadderFeed, TheGlobalTopIsForwardedOnEveryUpdate) {
    Rig rig; // ladder off, no raster yet
    auto replica = book(99, 101, 0.01, {{true, 100.01, 1}, {true, 99.5, 1}, {false, 100.02, 1}, {false, 100.9, 1}});
    rig.feed.publish(*replica);
    ASSERT_EQ(rig.tops.size(), 1u);
    EXPECT_NEAR(rig.tops[0].first, 100.01, 1e-9);
    EXPECT_NEAR(rig.tops[0].second, 100.02, 1e-9);
    EXPECT_FALSE(rig.feed.pending());
    rig.feed.setEnabled(true);
    rig.view(camera(200, 210, 0.05)); // far from the market, outside the band
    const auto& s = rig.rows(*replica);
    EXPECT_TRUE(s.valid);
    EXPECT_FALSE(s.complete);
    EXPECT_STREQ(rig.feed.lastReason(), "band");
    ASSERT_EQ(rig.tops.size(), 2u);
    EXPECT_NEAR(rig.tops[1].first, 100.01, 1e-9);
    EXPECT_NEAR(rig.tops[1].second, 100.02, 1e-9);
    EXPECT_NEAR(s.bestBid, 100.01, 1e-9);
}

// ------------------------------------------------------------- (7) completeness
std::vector<BookLevelUpdate> bidsFrom(int lo, int count) {
    std::vector<BookLevelUpdate> levels;
    for (int i = 0; i < count; ++i) levels.push_back({true, double(lo + i), 1.0});
    levels.push_back({false, 20'000.0, 1.0});
    return levels;
}

TEST(LadderFeed, ExactlyTheLevelCapIsComplete) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(0, 40'000, 1, bidsFrom(100, int(chart::BookLadderFeed::kMaxLevels)));
    rig.view(camera(50, 350, 1));
    const auto& s = rig.rows(*replica);
    EXPECT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
}

TEST(LadderFeed, OverTheLevelCapIsIncompleteBeyondTheLastCapturedLevel) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(0, 40'000, 1, bidsFrom(100, int(chart::BookLadderFeed::kMaxLevels) + 1));
    rig.view(camera(50, 350, 1)); // reaches the uncaptured level 100
    const auto& low = rig.rows(*replica);
    EXPECT_TRUE(low.valid);
    EXPECT_FALSE(low.complete);
    EXPECT_STREQ(rig.feed.lastReason(), "levels");
    EXPECT_FALSE(rig.layer.drawing()) << "nothing drawn for an incomplete window";
    rig.view(camera(16'000, 16'300, 1)); // above the last captured level
    EXPECT_TRUE(rig.rows(*replica).complete) << rig.feed.lastReason();
    EXPECT_TRUE(rig.layer.drawing());
}

TEST(LadderFeed, AWordBudgetStopIsIncompleteBeyondTheScannedRange) {
    Rig rig;
    rig.feed.setEnabled(true);
    std::vector<BookLevelUpdate> levels;
    for (int i = 10; i <= 20; ++i) levels.push_back({true, double(i), 1.0});
    for (int i = 30; i <= 40; ++i) levels.push_back({false, double(i), 1.0});
    // 1.1 M slots: the bid scan from the band top runs out of words before the bids.
    auto replica = book(0, 1'100'000, 1, levels);
    rig.view(camera(5, 105, 1));
    EXPECT_FALSE(rig.rows(*replica).complete);
    EXPECT_STREQ(rig.feed.lastReason(), "scan");
    rig.view(camera(60'000, 60'100, 1)); // inside what the bid scan saw; asks fully scanned
    EXPECT_TRUE(rig.rows(*replica).complete) << rig.feed.lastReason();
}

TEST(LadderFeed, ASparseFullyScannedBookIsCompleteOverTheSpreadAndEmptyRegions) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(0, 1000, 1, {{true, 400, 1}, {true, 410, 2}, {false, 500, 3}, {false, 510, 4}});
    rig.view(camera(300, 700, 1)); // beyond both occupied extrema
    const auto& wide = rig.rows(*replica);
    EXPECT_TRUE(wide.valid && wide.complete) << rig.feed.lastReason();
    EXPECT_FLOAT_EQ(bidAt(wide, 410), 2.0f);
    EXPECT_FLOAT_EQ(askAt(wide, 500), 3.0f);
    rig.view(camera(450, 460, 1)); // only the spread
    EXPECT_TRUE(rig.rows(*replica).complete) << rig.feed.lastReason();
}

TEST(LadderFeed, AWindowOfMoreBinsThanRowsIsTooWide) {
    Rig rig;
    rig.feed.setEnabled(true);
    auto replica = book(0, 10'000, 1, {{true, 400, 1}, {false, 500, 3}});
    rig.view(camera(0, 5000, 1, 1.0, 5000)); // 5000 one-pixel rows
    const auto& s = rig.rows(*replica);
    EXPECT_FALSE(s.valid);
    EXPECT_STREQ(rig.feed.lastReason(), "wide");
}

// ------------------------------------------------- (8) allocation and geometry
TEST(LadderLayer, WarmedFramesAllocateNothingAndRewriteOnlyOnChange) {
    gAllocations = 0;
    gCounting = true;
    { chart::LadderMailbox probe; }
    gCounting = false;
    ASSERT_GE(gAllocations.load(), 2u) << "the counter sees library allocations";

    Rig rig;
    rig.feed.setEnabled(true);
    rig.feed.setRadar(true, 4); // the tracker runs inside publishRows
    std::vector<BookLevelUpdate> levels;
    for (int i = 0; i < 400; ++i) levels.push_back({i < 200, 98.0 + i * 0.01, 1.0 + i});
    auto a = book(97, 103, 0.01, levels);
    for (auto& level : levels) level.quantity *= 2;
    auto b = book(97, 103, 0.01, levels);
    const auto rest = camera(98, 102, 0.05);
    QSGNode root;
    root.appendChildNode(rig.layer.createContainer());
    ChartTextRenderer text;
    ChartTextAtlas atlas;
    chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
    auto draw = [&](const chart::ChartFrame& frame) {
        rig.layer.sync(frame);
        rig.layer.updateNodes(frame, context);
        rig.layer.submitLabels(frame, context, false); // the text stage lays out the badges
    };
    rig.view(rest);
    rig.feed.publish(*a);
    rig.feed.publishRows();
    draw(rig.chartFrame());
    ASSERT_TRUE(rig.layer.drawing());
    const auto rebuilds = rig.layer.rebuildCount(), marks = rig.layer.dirtyMarks();
    EXPECT_GT(rig.layer.vertexCount(), 0);
    draw(rig.chartFrame());
    EXPECT_EQ(rig.layer.rebuildCount(), rebuilds) << "identical snapshot and camera";
    EXPECT_EQ(rig.layer.dirtyMarks(), marks);
    rig.view(chart_raster::shiftedRaster(rest, 0, 1));
    draw(rig.chartFrame());
    EXPECT_EQ(rig.layer.rebuildCount(), rebuilds + 1) << "a camera change";
    EXPECT_EQ(rig.layer.dirtyMarks(), marks + 2) << "one per node: bars and badges";
    rig.feed.publish(*b);
    rig.feed.publishRows();
    draw(rig.chartFrame());
    EXPECT_EQ(rig.layer.rebuildCount(), rebuilds + 2) << "a snapshot change";
    draw(rig.chartFrame(0)); // ladder off
    EXPECT_EQ(rig.layer.rebuildCount(), rebuilds + 2);
    EXPECT_EQ(static_cast<QSGOpacityNode*>(root.firstChild())->opacity(), 0.0);
    // 100 warmed frames with changing rows and cameras.
    size_t allocations = 0;
    for (int i = 0; i < 100; ++i) {
        rig.view(chart_raster::shiftedRaster(rest, 0, i % 7));
        if (i % 3 == 0) {
            rig.feed.publish(i % 2 ? *a : *b);
            rig.feed.publishRows();
        }
        const auto frame = rig.chartFrame();
        gAllocations = 0;
        gCounting = true;
        draw(frame);
        rig.layer.afterRendering();
        gCounting = false;
        allocations += gAllocations.load();
        ASSERT_TRUE(rig.layer.drawing());
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_GT(rig.layer.rebuildCount(), rebuilds + 50);
    // A root rebuild recreates the nodes once.
    const auto creations = rig.layer.nodeCreations();
    QSGNode root2;
    rig.layer.onRootRebuilt();
    root2.appendChildNode(rig.layer.createContainer());
    EXPECT_EQ(rig.layer.nodeCreations(), creations + 1);
}

// Bars are inside the strip and on the drawn rows at DPR 1 and 2.
TEST(LadderLayer, BarsSitInTheStripOnTheDrawnRowsAtEveryDeviceRatio) {
    for (const double dpr : {1.0, 2.0}) {
        SCOPED_TRACE(dpr);
        Rig rig;
        rig.feed.setEnabled(true);
        auto replica = book(97, 103, 0.01, {{true, 99.98, 2}, {true, 100.0, 1}, {false, 100.05, 4}});
        rig.view(camera(99.5, 100.5, 0.05, dpr));
        QSGNode root;
        root.appendChildNode(rig.layer.createContainer());
        ChartTextRenderer text;
        ChartTextAtlas atlas;
        chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
        rig.rows(*replica);
        rig.layer.updateNodes(rig.chartFrame(), context);
        ASSERT_TRUE(rig.layer.drawing());
        auto* gate = static_cast<QSGOpacityNode*>(root.firstChild());
        auto* clip = static_cast<QSGClipNode*>(gate->firstChild());
        auto* bars = static_cast<QSGGeometryNode*>(clip->firstChild());
        EXPECT_EQ(clip->clipRect(), QRectF(640, 0, 72, 320));
        const auto* v = bars->geometry()->vertexDataAsColoredPoint2D();
        const int n = rig.layer.vertexCount();
        ASSERT_EQ(n, 3 * 6) << "bins 1999 (bid), 2000 (bid), 2001 (ask)";
        const auto& cam = rig.frame.raster;
        for (int i = 0; i < n; ++i) {
            EXPECT_GE(v[i].x, 640.0f + chart::LadderLayer::kInsetPx);
            EXPECT_LE(v[i].x, 712.0f - chart::LadderLayer::kInsetPx);
        }
        for (int q = 0; q < 3; ++q) {
            const int64_t k = 1999 + q;
            float top = 1e9f, bottom = -1e9f;
            for (int i = 0; i < 6; ++i) top = std::min(top, v[q * 6 + i].y), bottom = std::max(bottom, v[q * 6 + i].y);
            EXPECT_NEAR(top * dpr, cam.yDev((k + 1) * 0.05), 1e-3) << "bin " << k << " top on its heatmap row edge";
            EXPECT_NEAR(bottom * dpr, cam.yDev(k * 0.05), 1e-3) << "bin " << k;
            EXPECT_NEAR(top * dpr, std::round(top * dpr), 1e-3) << "row edges on device pixels";
        }
    }
}

// ------------------------------------------------- (9) suspension and binding
int probeLines(const char* name) {
    int n = 0;
    for (const auto& line : gProbeLines) n += line.contains(QLatin1String(name));
    return n;
}

TEST(LadderFeed, OffOrHiddenDoesNoRowWorkAndReactivationPublishesAtOnce) {
    Rig rig;
    auto replica = book(99, 101, 0.01, {{true, 100.0, 1}, {false, 100.05, 1}});
    rig.view(camera(99.5, 100.5, 0.05));
    gProbeLines.clear();
    rig.feed.publish(*replica);
    EXPECT_FALSE(rig.feed.pending()) << "off";
    rig.feed.setEnabled(true);
    EXPECT_TRUE(rig.feed.pending());
    ASSERT_TRUE(QTest::qWaitFor([&] { return rig.feed.rowPublications() == 1; }, chart::BookLadderFeed::kIntervalMs));
    EXPECT_EQ(probeLines("[ladder.publish]"), 1);
    rig.feed.setExposed(false);
    EXPECT_FALSE(rig.sync().valid) << "a suspension withdraws the rows";
    for (int i = 0; i < 5; ++i) rig.feed.publish(*replica);
    rig.feed.onFrameChanged();
    EXPECT_FALSE(rig.feed.pending());
    QTest::qWait(2 * chart::BookLadderFeed::kIntervalMs);
    EXPECT_EQ(rig.feed.rowPublications(), 1u);
    EXPECT_EQ(probeLines("[ladder.publish]"), 1);
    rig.feed.setExposed(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return rig.feed.rowPublications() == 2; }, chart::BookLadderFeed::kIntervalMs));
    EXPECT_TRUE(rig.sync().valid);
}

// ------------------------------------------------------------- (10) wall radar
const chart::LadderRow& rowAt(const LadderSnapshot& s, int64_t k) {
    static const chart::LadderRow none{};
    const int64_t i = k - s.binLo;
    return i >= 0 && i < s.rowCount ? s.rows[size_t(i)] : none;
}
bool bidLarge(const LadderSnapshot& s, int64_t k) { return rowAt(s, k).flags & chart::LadderRow::kBidLarge; }
bool askLarge(const LadderSnapshot& s, int64_t k) { return rowAt(s, k).flags & chart::LadderRow::kAskLarge; }
bool bidHot(const LadderSnapshot& s, int64_t k) { return rowAt(s, k).flags & chart::LadderRow::kBidHot; }

// Bids 91..97 (1 each, 93 = 5, 96 = 3), asks 101..107 (10 each): bid median 1, ask median 10.
std::unique_ptr<LiveOrderBook> wallBook() {
    std::vector<BookLevelUpdate> levels;
    for (int p = 91; p <= 97; ++p) levels.push_back({true, double(p), p == 93 ? 5.0 : p == 96 ? 3.0 : 1.0});
    for (int p = 101; p <= 107; ++p) levels.push_back({false, double(p), 10.0});
    return book(0, 1000, 1, levels);
}

struct RadarRig : Rig {
    RadarRig(double multiple = 4) {
        feed.setEnabled(true);
        feed.setRadar(true, multiple);
        view(camera(90, 110, 1)); // 20 rows of 16 px, bins = prices
    }
    const LadderSnapshot& tick(const LiveOrderBook& b, int64_t monotonicMs) {
        mono = monotonicMs;
        return rows(b);
    }
};

TEST(WallRadar, FlagsMarkOnlyBinsAtTheMultipleOfTheirOwnSideMedian) {
    RadarRig rig;
    auto replica = wallBook();
    const auto& s = rig.tick(*replica, 1000);
    ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
    EXPECT_TRUE(s.radar);
    EXPECT_DOUBLE_EQ(s.wallMultiple, 4);
    EXPECT_FLOAT_EQ(s.bidMedian, 1.0f);
    EXPECT_FLOAT_EQ(s.askMedian, 10.0f);
    EXPECT_STREQ(s.asset, "TEST");
    for (int64_t k = 85; k <= 115; ++k) {
        EXPECT_EQ(bidLarge(s, k), k == 93) << "bid bin " << k << " (5x yes, 3x no; the ask median never marks bids)";
        EXPECT_FALSE(askLarge(s, k)) << "ask bin " << k;
    }
    rig.feed.setRadar(true, 6);
    const auto& six = rig.tick(*replica, 1067);
    EXPECT_DOUBLE_EQ(six.wallMultiple, 6);
    for (int64_t k = 85; k <= 115; ++k) EXPECT_FALSE(bidLarge(six, k)) << k;
    rig.feed.setRadar(false, 6);
    const auto& off = rig.tick(*replica, 1134);
    EXPECT_FALSE(off.radar);
    EXPECT_FALSE(bidLarge(off, 93));
    EXPECT_FLOAT_EQ(off.bidMedian, 0.0f);
}

TEST(WallRadar, AgesAndFlickersFollowTheTrackerAndSurvivePansButNotATickChange) {
    RadarRig rig;
    auto replica = wallBook();
    EXPECT_EQ(rowAt(rig.tick(*replica, 1000), 93).bidWallMs, 0u);
    EXPECT_EQ(rowAt(rig.tick(*replica, 2000), 93).bidWallMs, 1000u);
    const std::vector<BookLevelUpdate> gone{{true, 93.0, 0}}, back{{true, 93.0, 5}};
    replica->applyUpdates(gone, {}, nullptr);
    EXPECT_EQ(rowAt(rig.tick(*replica, 2200), 93).bidWallMs, 0u);
    replica->applyUpdates(back, {}, nullptr);
    const auto& returned = rig.tick(*replica, 2500);
    EXPECT_EQ(rowAt(returned, 93).bidFlickers, 1);
    EXPECT_TRUE(bidHot(returned, 93));
    EXPECT_EQ(rowAt(returned, 93).bidWallMs, 1500u) << "a return within the flicker window keeps the wall age";
    EXPECT_EQ(rowAt(rig.tick(*replica, 3500), 93).bidWallMs, 2500u);
    const auto& later = rig.tick(*replica, 2500 + chart::kFlickerHotMs + 1);
    EXPECT_EQ(rowAt(later, 93).bidFlickers, 1) << "cumulative";
    EXPECT_FALSE(bidHot(later, 93)) << "hot expires";
    const uint32_t seen = rowAt(later, 93).bidWallMs;
    rig.view(chart_raster::shiftedRaster(rig.frame.raster, 0, 16)); // one bin
    const auto& panned = rig.tick(*replica, 2500 + chart::kFlickerHotMs + 101);
    EXPECT_EQ(rowAt(panned, 93).bidWallMs, seen + 100) << "a pan keeps the overlapping ages";
    EXPECT_EQ(rowAt(panned, 93).bidFlickers, 1);
    rig.view(camera(90, 110, 2)); // display tick 2: every age restarts
    const auto& coarse = rig.tick(*replica, 2500 + chart::kFlickerHotMs + 201);
    ASSERT_TRUE(coarse.valid && coarse.complete);
    for (int i = 0; i < coarse.rowCount; ++i) {
        EXPECT_EQ(coarse.rows[size_t(i)].bidWallMs, 0u) << i;
        EXPECT_EQ(coarse.rows[size_t(i)].bidFlickers, 0) << i;
    }
}

TEST(WallRadar, SeenForAndFlickerCountsSaturate) {
    RadarRig rig;
    auto replica = wallBook();
    rig.tick(*replica, 0);
    EXPECT_EQ(rowAt(rig.tick(*replica, (int64_t(1) << 32) + 5), 93).bidWallMs, 0xffffffffu);
    // 70 000 flickers of one row, straight into the tracker.
    auto s = std::make_unique<LadderSnapshot>();
    s->valid = s->complete = true;
    s->serverTick = 1;
    s->group = 1;
    s->binLo = 100;
    s->rowCount = 2;
    s->rows[1].bid = 1;
    chart::LadderTracker tracker;
    chart::WallCandidates none;
    int64_t t = 0;
    for (int i = 0; i <= 70'000; ++i) {
        s->rows[0].bid = 1;
        tracker.publish(*s, 7, ++t, none);
        s->rows[0].bid = 0;
        tracker.publish(*s, 7, ++t, none);
    }
    s->rows[0].bid = 1;
    tracker.publish(*s, 7, ++t, none);
    EXPECT_EQ(s->rows[0].bidFlickers, 65535);
    EXPECT_EQ(tracker.rejectedUpdates(), 0u);
}

// r1 F1: an incomplete capture (zero-filled unknown rows) is no observation.
TEST(WallRadar, AnIncompleteCaptureIsNoObservation) {
    std::vector<BookLevelUpdate> levels;
    for (int i = 0; i < 50; ++i) levels.push_back({true, 99.5 + i * 0.01, 1.0}); // 99.50..99.99
    levels.push_back({true, 100.0, 25.0});                                      // 5x the bin median 5
    for (int i = 1; i <= 50; ++i) levels.push_back({false, 100.0 + i * 0.01, 1.0});
    auto replica = book(99, 101, 0.01, levels);
    RadarRig rig;
    rig.view(camera(99.5, 100.5, 0.05));
    const auto& t0 = rig.tick(*replica, 1000);
    ASSERT_TRUE(t0.valid && t0.complete) << rig.feed.lastReason();
    ASSERT_TRUE(bidLarge(t0, 2000));
    const float median = t0.bidMedian;
    EXPECT_FLOAT_EQ(median, 5.0f);
    std::vector<sentinel::book::LevelSizes> before;
    for (int i = 0; i < t0.rowCount; ++i) before.push_back({t0.rows[size_t(i)].bid, t0.rows[size_t(i)].ask});
    const int64_t binLo = t0.binLo;

    rig.feed.setScanBudget(1, chart::BookLadderFeed::kMaxLevels);
    const auto& t1 = rig.tick(*replica, 1067);
    EXPECT_TRUE(t1.valid);
    EXPECT_FALSE(t1.complete);
    EXPECT_STREQ(rig.feed.lastReason(), "scan");
    EXPECT_FLOAT_EQ(bidAt(t1, 2000), 0.0f) << "an unknown row inside the window reads zero";
    EXPECT_TRUE(t1.radar);
    EXPECT_DOUBLE_EQ(t1.wallMultiple, 4);
    EXPECT_FLOAT_EQ(t1.bidMedian, median) << "the last accepted median";
    EXPECT_FALSE(rig.layer.drawing());
    // Negative control: the same zeros as an observation are a disappearance.
    std::vector<sentinel::book::LevelSizes> after;
    for (int i = 0; i < t1.rowCount; ++i) after.push_back({t1.rows[size_t(i)].bid, t1.rows[size_t(i)].ask});
    sentinel::book::LevelTracker naive({.maxRows = 4096});
    using sentinel::book::UpdateStatus;
    ASSERT_EQ(naive.update({1, 0.05, double(binLo) * 0.05, before, 1000}), UpdateStatus::Accepted);
    ASSERT_TRUE(naive.level(size_t(2000 - binLo), sentinel::book::Side::Bid).present);
    ASSERT_EQ(naive.update({1, 0.05, double(t1.binLo) * 0.05, after, 1067}), UpdateStatus::Accepted);
    EXPECT_FALSE(naive.level(size_t(2000 - t1.binLo), sentinel::book::Side::Bid).present);

    rig.feed.setScanBudget(chart::BookLadderFeed::kMaxScanWords, chart::BookLadderFeed::kMaxLevels);
    const auto& t2 = rig.tick(*replica, 1134);
    ASSERT_TRUE(t2.complete);
    EXPECT_EQ(rowAt(t2, 2000).bidWallMs, 134u);
    EXPECT_EQ(rowAt(t2, 2000).bidFlickers, 0);
    EXPECT_FLOAT_EQ(t2.bidMedian, median);
}

TEST(WallRadar, TheTrackerClockIsMonotonicAndStrictlyIncreasing) {
    RadarRig rig;
    auto replica = wallBook();
    wallRadarWarnings = 0;
    rig.now = 10'000;
    rig.tick(*replica, 1000);
    rig.now = 5000; // the wall clock steps back 5 s
    const auto& s = rig.tick(*replica, 1500);
    EXPECT_EQ(rowAt(s, 93).bidWallMs, 500u) << "ages follow the monotonic port";
    EXPECT_EQ(rig.layer.current().receivedAtMs, 5000) << "the receive time stays wall time";
    const auto accepted = rig.feed.tracker().acceptedUpdates();
    rig.feed.publishRows(); // the same monotonic millisecond, twice
    rig.feed.publishRows();
    EXPECT_EQ(rig.feed.tracker().acceptedUpdates(), accepted + 2);
    EXPECT_EQ(rig.feed.tracker().rejectedUpdates(), 0u);
    EXPECT_EQ(wallRadarWarnings, 0);
}

// r1 F2: every way a series can end restarts the ages.
TEST(WallRadar, EverySeriesEndRestartsTheAges) {
    const auto aged = [](RadarRig& rig, LiveOrderBook& b, int64_t& t) {
        rig.tick(b, t += 1000);
        return rowAt(rig.tick(b, t += 1000), 93).bidWallMs;
    };
    const auto fresh = [](RadarRig& rig, LiveOrderBook& b, int64_t& t) {
        const auto& s = rig.tick(b, t += 1000);
        EXPECT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        return rowAt(s, 93).bidWallMs;
    };
    { // (a) a resnapshot of the same replica; a plain delta keeps the ages
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        const std::vector<BookLevelUpdate> delta{{true, 91.0, 2}};
        replica->applyUpdates(delta, {}, nullptr);
        EXPECT_EQ(fresh(rig, *replica, t), 2000u) << "a delta is no new series";
        const uint64_t epoch = replica->epoch();
        replica->initialize(0, 1000, 1);
        EXPECT_EQ(replica->epoch(), epoch + 1);
        std::vector<BookLevelUpdate> levels;
        for (int p = 91; p <= 97; ++p) levels.push_back({true, double(p), p == 93 ? 5.0 : 1.0});
        replica->applyUpdates(levels, {}, nullptr);
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "resnapshot";
    }
    { // (b) a symbol generation
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        rig.feed.beginGeneration();
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "generation";
    }
    { // (c) an empty update
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        const int64_t received = rig.layer.current().receivedAtMs;
        rig.now += 1000;
        replica->clear();
        rig.feed.publish(*replica);
        EXPECT_EQ(rig.sync().receivedAtMs, received) << "INV-130";
        auto rebuilt = wallBook();
        EXPECT_EQ(fresh(rig, *rebuilt, t), 0u) << "empty update";
    }
    { // (d) a display tick change
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        rig.view(camera(90, 110, 2));
        rig.tick(*replica, t += 1000);
        rig.view(camera(90, 110, 1));
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "tick";
    }
    { // (e) a suspension
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        const int64_t received = rig.layer.current().receivedAtMs;
        rig.feed.setExposed(false);
        EXPECT_EQ(rig.sync().receivedAtMs, received);
        rig.feed.setExposed(true);
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "suspended";
    }
    { // (f) an invalid fill
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        const auto rest = rig.frame.raster;
        rig.view(camera(0, 5000, 1, 1.0, 5000));
        EXPECT_FALSE(rig.tick(*replica, t += 1000).valid);
        EXPECT_STREQ(rig.feed.lastReason(), "wide");
        rig.view(rest);
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "wide";
    }
    { // (g) radar off and on
        RadarRig rig;
        auto replica = wallBook();
        int64_t t = 0;
        ASSERT_EQ(aged(rig, *replica, t), 1000u);
        rig.feed.setRadar(false, 4);
        rig.tick(*replica, t += 1000);
        rig.feed.setRadar(true, 4);
        EXPECT_EQ(fresh(rig, *replica, t), 0u) << "radar off";
    }
}

TEST(WallRadar, TheRadarRepublishesAt15HzAndADeltaStillWinsTheEarlierDeadline) {
    auto replica = wallBook();
    {
        RadarRig rig;
        rig.feed.publish(*replica);
        const auto start = rig.feed.rowPublications();
        QTest::qWait(1000);
        // About 15 at 67 ms; the lower bound leaves room for a loaded machine (the landing
        // gate's full ctest), far above the one publication a missing re-arm leaves.
        const auto n = rig.feed.rowPublications() - start;
        EXPECT_GE(n, 8u);
        EXPECT_LE(n, 18u);
    }
    {
        Rig rig; // ladder on, radar off, quiet
        rig.view(camera(90, 110, 1));
        rig.feed.publish(*replica);
        rig.feed.setEnabled(true);
        QTest::qWait(300);
        EXPECT_EQ(rig.feed.rowPublications(), 1u);
    }
    {
        RadarRig rig;
        rig.feed.setRadarIntervalMs(250);
        QElapsedTimer clock;
        clock.start();
        int64_t at = -1;
        QEventLoop loop; // quit by the publication itself (qWaitFor polls in 10 ms sleeps)
        rig.feed.setPorts({[&rig] { return rig.frame; }, [](double, double) {},
                           [&] {
                               at = clock.elapsed();
                               loop.quit();
                           },
                           [] { return int64_t(0); }, [&] { return clock.elapsed(); }});
        const auto waitForPublication = [&] {
            const auto before = rig.feed.rowPublications();
            QTimer::singleShot(1000, &loop, &QEventLoop::quit); // the cap
            loop.exec();
            return rig.feed.rowPublications() > before;
        };
        rig.feed.publish(*replica);
        ASSERT_TRUE(waitForPublication()) << "the first radar publication";
        QTest::qWait(10);
        const int64_t deltaAt = clock.elapsed();
        rig.feed.publish(*replica);
        ASSERT_TRUE(waitForPublication());
        // kIntervalMs (67) plus scheduling slack under load; the radar wait would be ~240 ms.
        EXPECT_LE(at - deltaAt, 150) << "the delta's earlier deadline, not the 250 ms radar wait";
    }
}

TEST(WallRadar, CandidatesTakeTheNearestWallPerSideBeyondEachDrawnEdge) {
    chart::WallCandidates walls;
    walls.begin(100.0, 110.0, {{1.0, 1.0}, 2}); // a wall: at least 2
    walls.consider(LadderSnapshot::kAsk, 112.0, 9);
    walls.consider(LadderSnapshot::kAsk, 110.0, 3); // exactly the top edge: above, and nearer
    walls.consider(LadderSnapshot::kAsk, 111.0, 1.5); // nearer but no wall
    walls.consider(LadderSnapshot::kAsk, 109.99, 50); // on screen
    walls.consider(LadderSnapshot::kAsk, 99.0, 4);
    walls.consider(LadderSnapshot::kBid, 100.0, 50); // exactly the bottom edge: on screen
    walls.consider(LadderSnapshot::kBid, 99.99, 1);  // no wall
    walls.consider(LadderSnapshot::kBid, 99.5, 2);   // exactly the multiple
    walls.consider(LadderSnapshot::kBid, 90.0, 80);
    walls.consider(LadderSnapshot::kBid, 120.0, 7);
    walls.setCapped(false, true);
    const auto& c = walls.cells;
    const auto& askAbove = c[LadderSnapshot::kAsk][LadderSnapshot::kAbove];
    EXPECT_TRUE(askAbove.found);
    EXPECT_DOUBLE_EQ(askAbove.price, 110.0) << "the nearest wall, not the largest (112 x 9)";
    EXPECT_FLOAT_EQ(askAbove.size, 3);
    EXPECT_DOUBLE_EQ(c[LadderSnapshot::kAsk][LadderSnapshot::kBelow].price, 99.0);
    EXPECT_DOUBLE_EQ(c[LadderSnapshot::kBid][LadderSnapshot::kBelow].price, 99.5);
    EXPECT_FLOAT_EQ(c[LadderSnapshot::kBid][LadderSnapshot::kBelow].size, 2);
    EXPECT_DOUBLE_EQ(c[LadderSnapshot::kBid][LadderSnapshot::kAbove].price, 120.0);
    EXPECT_TRUE(askAbove.capped);
    EXPECT_FALSE(c[LadderSnapshot::kBid][LadderSnapshot::kAbove].capped);
    // A higher multiple: the nearer walls fail and the farther ones show.
    walls.begin(100.0, 110.0, {{1.0, 1.0}, 4});
    walls.consider(LadderSnapshot::kAsk, 112.0, 9);
    walls.consider(LadderSnapshot::kAsk, 110.0, 3);
    EXPECT_DOUBLE_EQ(c[LadderSnapshot::kAsk][LadderSnapshot::kAbove].price, 112.0);
    // None passes, or no positive median: no wall.
    walls.begin(100.0, 110.0, {{1.0, 1.0}, 10});
    walls.consider(LadderSnapshot::kAsk, 112.0, 9);
    EXPECT_FALSE(c[LadderSnapshot::kAsk][LadderSnapshot::kAbove].found);
    walls.begin(100.0, 110.0, {{0.0, 0.0}, 2});
    walls.consider(LadderSnapshot::kAsk, 112.0, 9);
    EXPECT_FALSE(c[LadderSnapshot::kAsk][LadderSnapshot::kAbove].found);
}

const chart::WallCandidate& wall(const LadderSnapshot& s, int side, int direction) {
    return s.walls[size_t(side)][size_t(direction)];
}

// Bids 80..99 and asks 100..119 (1 each) plus extra levels.
std::unique_ptr<LiveOrderBook> sparseBook(const std::vector<BookLevelUpdate>& extra) {
    std::vector<BookLevelUpdate> levels;
    for (int p = 80; p <= 99; ++p) levels.push_back({true, double(p), 1});
    for (int p = 100; p <= 119; ++p) levels.push_back({false, double(p), 1});
    levels.insert(levels.end(), extra.begin(), extra.end()); // a level update sets the size
    return book(0, 1000, 1, levels);
}

TEST(WallRadar, OffScreenWallsAreFoundAgainstAPositiveMedianOnly) {
    using S = LadderSnapshot;
    { // a 10x ask two bins above the window (inside the published margin)
        RadarRig rig;
        rig.view(camera(85, 105, 1));
        auto replica = sparseBook({{false, 107, 10}});
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.complete);
        ASSERT_GT(s.binLo + s.rowCount - 1, 107) << "inside the published rows";
        const auto& w = wall(s, S::kAsk, S::kAbove);
        EXPECT_TRUE(w.found);
        EXPECT_DOUBLE_EQ(w.price, 107);
        EXPECT_FLOAT_EQ(w.size, 10);
        EXPECT_FALSE(w.capped);
        EXPECT_FALSE(wall(s, S::kBid, S::kBelow).found) << "1x bids below";
    }
    { // over the level cap on the ask side: capped
        RadarRig rig;
        rig.view(camera(85, 105, 1));
        auto replica = sparseBook({{false, 107, 10}, {false, 500, 1}});
        rig.feed.setScanBudget(chart::BookLadderFeed::kMaxScanWords, 20);
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.complete) << rig.feed.lastReason();
        EXPECT_TRUE(wall(s, S::kAsk, S::kAbove).found);
        EXPECT_TRUE(wall(s, S::kAsk, S::kAbove).capped);
        EXPECT_FALSE(wall(s, S::kBid, S::kBelow).capped);
    }
    { // a 10x bid above the window (panned below the market); 3x is not a wall
        RadarRig rig;
        rig.view(camera(60, 85, 1));
        auto replica = sparseBook({{true, 90, 9}, {true, 82, 2}});
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.complete);
        EXPECT_TRUE(wall(s, S::kBid, S::kAbove).found);
        EXPECT_DOUBLE_EQ(wall(s, S::kBid, S::kAbove).price, 90);
        auto small = sparseBook({{true, 90, 3}});
        EXPECT_FALSE(wall(rig.tick(*small, 2000), S::kBid, S::kAbove).found) << "3x";
    }
    { // a window over the whole fully scanned book: nothing off screen
        RadarRig rig;
        rig.view(camera(60, 140, 1, 1.0, 320));
        auto replica = sparseBook({{false, 110, 30}});
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.complete);
        for (int side = 0; side < 2; ++side)
            for (int d = 0; d < 2; ++d) EXPECT_FALSE(wall(s, side, d).found) << side << d;
    }
    { // the reference is the book near the touch, not the fed rows: a view above the
      // market still finds the bid wall below; a book without bids has none
        RadarRig rig(1.5);
        auto replica = sparseBook({{true, 70, 30}});
        rig.view(camera(130, 150, 1));
        const auto& above = rig.tick(*replica, 1000);
        ASSERT_TRUE(above.complete);
        EXPECT_FLOAT_EQ(above.bidMedian, 1.0f);
        EXPECT_TRUE(wall(above, S::kBid, S::kBelow).found);
        EXPECT_DOUBLE_EQ(wall(above, S::kBid, S::kBelow).price, 70);
        std::vector<BookLevelUpdate> asksOnly;
        for (int p = 100; p <= 119; ++p) asksOnly.push_back({false, double(p), 1});
        auto noBids = book(0, 1000, 1, asksOnly);
        const auto& empty = rig.tick(*noBids, 2000);
        EXPECT_FLOAT_EQ(empty.bidMedian, 0.0f);
        EXPECT_FALSE(wall(empty, S::kBid, S::kBelow).found);
        EXPECT_FALSE(wall(empty, S::kBid, S::kAbove).found);
    }
}

// The drawn window [90.6, 100.4) at display tick 1, snapped or fractional (free).
chart_raster::RasterCamera partialCamera(bool glide) {
    chart_raster::RasterInputs in;
    in.timeEnd = 60 * kTf;
    in.minPrice = 90.6;
    in.maxPrice = 100.4;
    in.tfMs = double(kTf);
    in.tick = 1;
    in.itemWidthLogical = 640;
    in.itemHeightLogical = 320;
    auto cam = glide ? chart_raster::continuousRaster(in) : chart_raster::computeRaster(in, {});
    cam.drawnMinPrice = 90.6;
    cam.drawnMaxPrice = 100.4;
    return cam;
}

// r2 F4: off-screen by real price, not by bin (the edge bins are partial).
TEST(WallRadar, PartialEdgeBinsClassifyByRealPrice) {
    using S = LadderSnapshot;
    for (const double serverTick : {0.1, 0.01}) {
        for (const bool glide : {false, true}) {
            SCOPED_TRACE(testing::Message() << "serverTick=" << serverTick << " glide=" << glide);
            RadarRig rig;
            std::vector<BookLevelUpdate> levels;
            for (int i = 0; i < 100; ++i) levels.push_back({true, 90.6 + i * 0.05, 1});
            for (int i = 0; i < 100; ++i) levels.push_back({false, 95.6 + i * 0.05, 1});
            levels.push_back({false, 100.8, 1000});
            levels.push_back({true, 90.2, 1000});
            auto replica = book(50, 150, serverTick, levels);
            const auto cam = partialCamera(glide);
            if (glide) ASSERT_TRUE(cam.free);
            rig.view(cam);
            const auto& s = rig.tick(*replica, 1000);
            ASSERT_TRUE(s.valid) << rig.feed.lastReason();
            ASSERT_EQ(chart::ladderWindow(90.6, 100.4, 1).hi, 100) << "100.8 shares the top bin";
            EXPECT_TRUE(wall(s, S::kAsk, S::kAbove).found);
            EXPECT_NEAR(wall(s, S::kAsk, S::kAbove).price, 100.8, 1e-9);
            EXPECT_TRUE(wall(s, S::kBid, S::kBelow).found);
            EXPECT_NEAR(wall(s, S::kBid, S::kBelow).price, 90.2, 1e-9);
        }
    }
}

// Group 1 (server tick = display tick = 1): the edge bins 90 and 100 are partial
// ([90.6, 100.4) drawn); 90 lies below the drawn window though its bin is window.lo.
TEST(WallRadar, PartialEdgeBinsAtGroupOneClassifyByRealPrice) {
    using S = LadderSnapshot;
    for (const bool glide : {false, true}) {
        SCOPED_TRACE(testing::Message() << "glide=" << glide);
        RadarRig rig;
        std::vector<BookLevelUpdate> levels;
        for (int p = 91; p <= 95; ++p) levels.push_back({true, double(p), 1});
        for (int p = 96; p <= 100; ++p) levels.push_back({false, double(p), 1});
        levels.push_back({true, 90, 50});
        levels.push_back({false, 101, 50});
        auto replica = book(50, 150, 1, levels);
        const auto cam = partialCamera(glide);
        if (glide) ASSERT_TRUE(cam.free);
        rig.view(cam);
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_EQ(s.group, 1);
        ASSERT_EQ(chart::ladderWindow(90.6, 100.4, 1).lo, 90) << "90 shares the bottom bin";
        EXPECT_FLOAT_EQ(s.bidMedian, 1.0f);
        EXPECT_FLOAT_EQ(s.askMedian, 1.0f);
        EXPECT_TRUE(wall(s, S::kBid, S::kBelow).found);
        EXPECT_DOUBLE_EQ(wall(s, S::kBid, S::kBelow).price, 90);
        EXPECT_TRUE(wall(s, S::kAsk, S::kAbove).found);
        EXPECT_DOUBLE_EQ(wall(s, S::kAsk, S::kAbove).price, 101);
        EXPECT_FALSE(wall(s, S::kBid, S::kAbove).found);
        EXPECT_FALSE(wall(s, S::kAsk, S::kBelow).found);
    }
}

// Read back one bar's colour: the six vertices of bar `bar`.
std::array<uchar, 4> barColour(const QSGGeometryNode* bars, int bar) {
    const auto* v = bars->geometry()->vertexDataAsColoredPoint2D() + bar * 6;
    for (int i = 1; i < 6; ++i) EXPECT_TRUE(v[i].r == v[0].r && v[i].g == v[0].g && v[i].b == v[0].b && v[i].a == v[0].a);
    return {v[0].r, v[0].g, v[0].b, v[0].a};
}

// The wall reference is the median of the 32 bins from each side's touch: dust bins far
// from the market at a wide zoom do not make every real level a wall.
TEST(WallRadar, TheReferenceIsTheMedianNearTheTouchNotEveryFedBin) {
    std::vector<BookLevelUpdate> levels;
    for (int p = 1; p <= 300; ++p) levels.push_back({true, double(p), 0.1}); // dust, far from the touch
    for (int p = 361; p <= 400; ++p) levels.push_back({true, double(p), p == 395 ? 50.0 : p == 380 ? 30.0 : 10.0});
    for (int p = 401; p <= 440; ++p) levels.push_back({false, double(p), 10.0});
    auto replica = book(0, 1000, 1, levels);
    RadarRig rig;
    rig.view(camera(0, 450, 1, 1.0, 450)); // every level in the window
    const auto& s = rig.tick(*replica, 1000);
    ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
    EXPECT_FLOAT_EQ(s.bidMedian, 10.0f) << "bins 369..400";
    EXPECT_FLOAT_EQ(s.askMedian, 10.0f);
    std::vector<int64_t> walls;
    for (int64_t k = 0; k <= 450; ++k)
        if (bidLarge(s, k)) walls.push_back(k);
    EXPECT_EQ(walls, std::vector<int64_t>{395}) << "5x the near median; 380 is 3x";
    // The old reference (every fed bin) is the dust: every real level is a wall.
    std::vector<sentinel::book::LevelSizes> rows;
    for (int i = 0; i < s.rowCount; ++i) rows.push_back({s.rows[size_t(i)].bid, s.rows[size_t(i)].ask});
    sentinel::book::LevelTracker allBins({.maxRows = 4096});
    ASSERT_EQ(allBins.update({1, 1, double(s.binLo), rows, 1000}), sentinel::book::UpdateStatus::Accepted);
    EXPECT_NEAR(allBins.median(sentinel::book::Side::Bid), 0.1, 1e-6);
    int dustWalls = 0;
    for (int i = 0; i < s.rowCount; ++i) dustWalls += allBins.isLarge(size_t(i), sentinel::book::Side::Bid, 4);
    EXPECT_EQ(dustWalls, 40);
}

// A display bin only partly inside the band (an unaligned band edge) is not wholly known:
// it never enters the reference, and the touch moves to the next wholly known bin.
// Native tick 1, display tick 10. Bids: the band ends at 105 (slots 0..105), so bin 10
// ([100, 110)) is partial. Asks: the band starts at 5, so bin 0 ([0, 10)) is partial.
TEST(WallRadar, APartlyKnownEdgeBinIsNoPartOfTheReference) {
    { // bids: 104 x 100 in the partial bin 10, then bins 9, 8 at 10 and bin 7 at 40
        auto replica = book(0, 105, 1, {{true, 104, 100}, {true, 99, 10}, {true, 89, 10}, {true, 79, 40}});
        RadarRig rig;
        rig.view(camera(20, 90, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.bidMedian, 10.0f) << "bins 9, 8, 7; the partial bin 10 would make it 25";
        EXPECT_TRUE(bidLarge(s, 7)) << "40 is 4x the median";
        EXPECT_FALSE(bidLarge(s, 8));
    }
    { // asks: 5 x 100 in the partial bin 0, then bins 1, 2 at 10 and bin 3 at 40
        auto replica = book(5, 110, 1, {{false, 5, 100}, {false, 12, 10}, {false, 22, 10}, {false, 32, 40}});
        RadarRig rig;
        rig.view(camera(20, 90, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.askMedian, 10.0f) << "bins 1, 2, 3; the partial bin 0 would make it 25";
        EXPECT_TRUE(askLarge(s, 3)) << "40 is 4x the median";
        EXPECT_FALSE(askLarge(s, 2));
    }
}

// The reference window starts at the bin of the real touch, even when that bin is only
// partly known and the next levels lie further out: the window does not slide past a gap.
// Native tick 1, display tick 10; the touch bin is partial, the window holds 10 and 30 and
// the 1s just beyond it stay out (median 20, so neither 10 nor 30 is a wall at 4x).
TEST(WallRadar, ThePartialTouchBinStillAnchorsTheReferenceWindow) {
    { // bids: band ends at 1005, touch 1004 in bin 100 (partial); window bins 100..69
        auto replica = book(0, 1005, 1, {{true, 1004, 100}, {true, 700, 10}, {true, 690, 30},
                                         {true, 680, 1}, {true, 670, 1}, {true, 660, 1}});
        RadarRig rig;
        rig.view(camera(600, 800, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.bidMedian, 20.0f) << "bins 70 and 69; a window re-anchored at bin 70 gives 1";
        for (const int64_t k : {66, 67, 68, 69, 70}) EXPECT_FALSE(bidLarge(s, k)) << k;
    }
    { // asks: band starts at 5, touch 5 in bin 0 (partial); window bins 0..31
        auto replica = book(5, 1010, 1, {{false, 5, 100}, {false, 300, 10}, {false, 310, 30},
                                         {false, 320, 1}, {false, 330, 1}, {false, 340, 1}});
        RadarRig rig;
        rig.view(camera(250, 450, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.askMedian, 20.0f) << "bins 30 and 31; a window re-anchored at bin 30 gives 1";
        for (const int64_t k : {30, 31, 32, 33, 34}) EXPECT_FALSE(askLarge(s, k)) << k;
    }
}

// A capped scan can stop inside a display bin: that bin, at the far end of what the
// capture knows, is only partly known and stays out of the reference (as the touch's).
TEST(WallRadar, APartlyKnownFarBinOfACappedScanIsNoPartOfTheReference) {
    { // bids, three levels captured: known down to 699, so bin 69 ([690, 700)) is partial
        auto replica = book(0, 1005, 1, {{true, 1004, 100}, {true, 700, 10}, {true, 699, 30}, {true, 695, 5}});
        RadarRig rig;
        rig.feed.setScanBudget(chart::BookLadderFeed::kMaxScanWords, 3);
        rig.view(camera(720, 900, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.bidMedian, 10.0f) << "bin 70 only; the partial bin 69 would make it 20";
    }
    { // asks (band from 5), three captured: known up to 300, so bin 30 ([300, 310)) is partial
        auto replica = book(5, 1010, 1, {{false, 5, 100}, {false, 290, 10}, {false, 300, 30}, {false, 305, 5}});
        RadarRig rig;
        rig.feed.setScanBudget(chart::BookLadderFeed::kMaxScanWords, 3);
        rig.view(camera(100, 250, 10));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.valid && s.complete) << rig.feed.lastReason();
        EXPECT_FLOAT_EQ(s.askMedian, 10.0f) << "bin 29 only; the partial bin 30 would make it 20";
    }
}

// The age is how long the level has been a wall, not how long the bin had size.
TEST(WallRadar, TheAgeCountsFromBecomingAWallAndSurvivesViewChangesOnTheSameGrid) {
    RadarRig rig;
    auto replica = wallBook(); // bin 95 holds 1 (median 1)
    const auto set95 = [&](double size) {
        const std::vector<BookLevelUpdate> u{{true, 95.0, size}};
        replica->applyUpdates(u, {}, nullptr);
    };
    rig.tick(*replica, 0);
    EXPECT_FALSE(bidLarge(rig.tick(*replica, 60'000), 95));
    set95(5); // a wall from 60 s
    const auto& wall = rig.tick(*replica, 60'000 + 67);
    ASSERT_TRUE(bidLarge(wall, 95));
    EXPECT_EQ(rowAt(wall, 95).bidWallMs, 0u) << "not the 60 s of presence";
    EXPECT_EQ(rowAt(rig.tick(*replica, 61'067), 95).bidWallMs, 1000u);
    set95(2); // below the multiple, still present
    EXPECT_FALSE(bidLarge(rig.tick(*replica, 62'000), 95));
    set95(5); // back within the flicker window (1 s): the clock keeps
    EXPECT_EQ(rowAt(rig.tick(*replica, 62'500), 95).bidWallMs, 2433u);
    set95(2);
    rig.tick(*replica, 63'000);
    set95(5); // back after more than the flicker window: the clock restarts
    EXPECT_EQ(rowAt(rig.tick(*replica, 64'001), 95).bidWallMs, 0u);
    EXPECT_EQ(rowAt(rig.tick(*replica, 65'001), 95).bidWallMs, 1000u);
    // View changes on the same bin grid keep the age: a pan, a price zoom, a resize.
    rig.view(chart_raster::shiftedRaster(rig.frame.raster, 0, 48));
    EXPECT_EQ(rowAt(rig.tick(*replica, 65'101), 95).bidWallMs, 1100u) << "pan";
    rig.view(camera(80, 120, 1));
    const auto& zoomed = rig.tick(*replica, 65'201);
    ASSERT_DOUBLE_EQ(zoomed.binPrice(), 1.0);
    EXPECT_EQ(rowAt(zoomed, 95).bidWallMs, 1200u) << "price zoom at the same tick";
    rig.view(camera(80, 120, 1, 2.0, 640));
    EXPECT_EQ(rowAt(rig.tick(*replica, 65'301), 95).bidWallMs, 1300u) << "resize and device ratio";
    rig.view(camera(80, 120, 2)); // another bin size: a new series
    rig.tick(*replica, 65'401);
    rig.view(camera(80, 120, 1));
    EXPECT_EQ(rowAt(rig.tick(*replica, 65'501), 95).bidWallMs, 0u);
}

// The edge pill names the nearest wall beyond the edge, so the multiple decides which.
TEST(WallRadar, TheOffScreenWallIsTheNearestThatPassesTheMultiple) {
    using S = LadderSnapshot;
    auto replica = sparseBook({{false, 108, 5}, {false, 112, 20}}); // ask median 1
    const std::tuple<double, bool, double> cases[] = {{4.0, true, 108.0}, {6.0, true, 112.0}, {25.0, false, 0.0}};
    for (const auto& [multiple, found, price] : cases) {
        SCOPED_TRACE(multiple);
        RadarRig rig(multiple);
        rig.view(camera(85, 105, 1));
        const auto& s = rig.tick(*replica, 1000);
        ASSERT_TRUE(s.complete);
        const auto& w = wall(s, S::kAsk, S::kAbove);
        EXPECT_EQ(w.found, found);
        if (found) EXPECT_DOUBLE_EQ(w.price, price);
    }
}

TEST(WallRadar, WallBarsUseThePremultipliedWallColours) {
    using Rgba = std::array<uchar, 4>;
    const Rgba bid{34, 150, 139, 230}, ask{215, 75, 72, 230};
    const Rgba bidWall{0, 220, 220, 220}, askWall{220, 173, 0, 220}, hotWall{230, 188, 69, 230};
    for (const bool radar : {true, false}) {
        SCOPED_TRACE(radar);
        Rig rig;
        rig.view(camera(90, 110, 1));
        QSGNode root;
        root.appendChildNode(rig.layer.createContainer());
        ChartTextRenderer text;
        ChartTextAtlas atlas;
        chart::LayerNodeContext context{nullptr, &root, text, atlas, false};
        auto& s = rig.layer.mailbox().back();
        s = LadderSnapshot{};
        s.valid = s.complete = true;
        s.serverTick = 1;
        s.group = 1;
        s.binLo = 85;
        s.rowCount = 31;
        s.radar = radar;
        using R = chart::LadderRow;
        const auto set = [&](int64_t k, float b, float a, uint8_t flags) {
            s.rows[size_t(k - 85)].bid = b;
            s.rows[size_t(k - 85)].ask = a;
            s.rows[size_t(k - 85)].flags = flags;
        };
        set(95, 1, 0, 0);
        set(96, 5, 0, R::kBidLarge);
        set(97, 5, 0, R::kBidLarge | R::kBidHot);
        set(98, 0, 0, R::kBidHot); // hot without size: no bar
        set(101, 0, 5, R::kAskLarge);
        set(102, 0, 1, R::kAskHot); // hot but not large: plain
        rig.layer.mailbox().post();
        rig.sync();
        rig.layer.updateNodes(rig.chartFrame(), context);
        ASSERT_TRUE(rig.layer.drawing());
        auto* bars = static_cast<QSGGeometryNode*>(root.firstChild()->firstChild()->firstChild());
        ASSERT_EQ(rig.layer.vertexCount(), 5 * 6);
        EXPECT_EQ(barColour(bars, 0), bid);
        EXPECT_EQ(barColour(bars, 1), radar ? bidWall : bid);
        EXPECT_EQ(barColour(bars, 2), radar ? hotWall : bid);
        EXPECT_EQ(barColour(bars, 3), radar ? askWall : ask);
        EXPECT_EQ(barColour(bars, 4), ask);
    }
}

TEST(WallRadar, WarmedRadarPublicationsAllocateNothing) {
    RadarRig rig;
    std::vector<BookLevelUpdate> levels;
    for (int i = 0; i < 400; ++i) levels.push_back({i < 200, 98.0 + i * 0.01, (i % 37 == 0 ? 20.0 : 1.0) + i % 3});
    auto a = book(97, 103, 0.01, levels);
    rig.view(camera(98, 102, 0.05));
    rig.feed.publish(*a);
    int64_t t = 1000;
    for (int i = 0; i < 10; ++i) {
        rig.mono = t += 67;
        rig.feed.publishRows();
    }
    // The probe line formats (it allocates); the publish itself must not.
    QLoggingCategory::setFilterRules("sentinel.probe.debug=false");
    size_t allocations = 0;
    for (int i = 0; i < 100; ++i) {
        rig.mono = t += 67;
        rig.view(chart_raster::shiftedRaster(camera(98, 102, 0.05), 0, i % 7));
        gAllocations = 0;
        gCounting = true;
        rig.feed.publishRows();
        gCounting = false;
        allocations += gAllocations.load();
    }
    QLoggingCategory::setFilterRules("sentinel.probe.debug=true");
    EXPECT_EQ(allocations, 0u);
    EXPECT_GE(rig.feed.tracker().acceptedUpdates(), 100u);
    EXPECT_EQ(rig.feed.tracker().rejectedUpdates(), 0u);
}

struct Bound {
    QQuickWindow window;
    QQuickItem* item = new QQuickItem(window.contentItem());
    chart::ChartLayers layers;
    chart::LayerVisibility visibility;
    GridViewState view;
    chart::PublishedFrame published;
    Bound() {
        view.setViewportSize(640, 320);
        view.setViewport(0, 60 * kTf, 99.5, 100.5);
        chart::bindLadder({*item, layers, visibility, view, published, [](double, double) {}});
    }
    chart::BookLadderFeed& feed() { return layers.ladderFeed; }
    bool settle() {
        return QTest::qWaitFor([&] { return !feed().pending(); }, 3 * chart::BookLadderFeed::kIntervalMs);
    }
};

TEST(LadderBinding, PansDragsTheToggleAndExposureDriveTheFeed) {
    Bound b;
    EXPECT_FALSE(b.feed().active()) << "off by default";
    b.visibility.setLadderEnabled(true);
    EXPECT_TRUE(b.feed().active());
    EXPECT_TRUE(b.feed().pending()) << "turning it on publishes";
    ASSERT_TRUE(b.settle());
    b.view.setViewport(0, 60 * kTf, 99.6, 100.6); // a committed pan or zoom
    EXPECT_TRUE(b.feed().pending());
    ASSERT_TRUE(b.settle());
    b.view.handlePanStart(QPointF(100, 100));
    b.view.handlePanMove(QPointF(100, 160)); // held: only the visual offset changes
    EXPECT_TRUE(b.feed().pending()) << "a held drag re-arms through panVisualOffsetChanged";
    ASSERT_TRUE(b.settle());
    b.view.handlePanEnd();
    ASSERT_TRUE(b.settle());
    b.item->setVisible(false);
    EXPECT_FALSE(b.feed().active()) << "item hidden";
    b.item->setVisible(true);
    EXPECT_TRUE(b.feed().active());
    b.window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&b.window) || b.window.isVisible());
    EXPECT_TRUE(b.feed().active());
    b.window.hide();
    EXPECT_FALSE(b.feed().active()) << "window hidden";
    b.window.show();
    EXPECT_TRUE(b.feed().active());
    b.visibility.setLadderEnabled(false);
    EXPECT_FALSE(b.feed().active());
    EXPECT_FALSE(b.feed().pending());
}

TEST(LadderBinding, TheWallRadarToggleAndThresholdReachTheFeed) {
    {
        Bound b;
        EXPECT_FALSE(b.feed().radar()) << "off by default";
        EXPECT_DOUBLE_EQ(b.feed().wallMultiple(), chart::LayerVisibility::kWallMultiple);
        b.visibility.setWallRadarEnabled(true);
        EXPECT_TRUE(b.feed().radar());
        EXPECT_FALSE(b.feed().active()) << "the radar alone does not turn the ladder on";
        b.visibility.setWallMultiple(6);
        EXPECT_DOUBLE_EQ(b.feed().wallMultiple(), 6);
        b.visibility.setWallRadarEnabled(false);
        EXPECT_FALSE(b.feed().radar());
    }
    chart::LayerVisibility on; // the initial state reaches the feed at bind time
    on.setWallRadarEnabled(true);
    on.setWallMultiple(3);
    QQuickWindow window;
    auto* item = new QQuickItem(window.contentItem());
    chart::ChartLayers layers;
    GridViewState view;
    chart::PublishedFrame published;
    chart::bindLadder({*item, layers, on, view, published, [](double, double) {}});
    EXPECT_TRUE(layers.ladderFeed.radar());
    EXPECT_DOUBLE_EQ(layers.ladderFeed.wallMultiple(), 3);
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("SENTINEL_PROBES", "ladder");
    QGuiApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        if (message.startsWith(QLatin1String("[ladder."))) gProbeLines.append(message);
        if (message.startsWith(QLatin1String("Wall radar"))) ++wallRadarWarnings;
    });
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
