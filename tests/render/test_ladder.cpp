// The chart-edge book ladder: the GUI-thread feed (native-grid binning, completeness,
// invalidation, withdrawal, generation, suspension) and the render-thread layer
// (validation against the drawn frame, retained geometry, no per-frame allocation).
#include "chart/feeds/BookLadderFeed.hpp"
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
#include <QGuiApplication>
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
    int64_t now = 1000;
    Rig() {
        feed.setPorts({[this] { return frame; }, [this](double b, double a) { tops.emplace_back(b, a); },
                       [this] { ++updates; }, [this] { return now; }});
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
    EXPECT_EQ(rig.layer.dirtyMarks(), marks + 1);
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
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("SENTINEL_PROBES", "ladder");
    QGuiApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        if (message.startsWith(QLatin1String("[ladder."))) gProbeLines.append(message);
    });
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
