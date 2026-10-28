// Wall radar badges and edge pills (LadderBadges through LadderLayer): layout in
// logical pixels on the drawn rows, edge pills by real price against the drawn window,
// the badge node outside the strip's clip, the text stage, the text renderer's
// reservation (zero tail, no allocation through endFrame on a real window).
#include "chart/layers/ChartLayers.hpp"
#include "chart/layers/LadderBadges.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "render/ChartTextNode.hpp"
#include "render/ChartTextRenderer.hpp"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Allocation counter (test binary only).
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
using chart::BadgePill;
using chart::LadderRow;
using chart::LadderSnapshot;
using S = LadderSnapshot;
constexpr int64_t kTf = 60'000;
constexpr double kGutter = 120;

const ChartTextAtlas& atlas() {
    static ChartTextAtlas a = [] {
        ChartTextAtlas out;
        ChartTextAtlas::BuildParams p; // as ChartTextLayer::buildMsdfAtlas
        p.fontFamily = "Roboto Mono";
        p.fontPx = 64;
        p.pxRange = 4.0f;
        p.charset = QStringLiteral(" 0123456789.+-,:/$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
        p.resourceFont = QStringLiteral(":/fonts/RobotoMono/RobotoMono-Regular.ttf");
        out.build(p);
        return out;
    }();
    return a;
}

chart_raster::RasterInputs inputs(double lo, double hi, double tick, double dpr, double height) {
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
    return in;
}

chart_raster::RasterCamera camera(double lo, double hi, double tick = 1, double dpr = 1, double height = 320) {
    return chart_raster::computeRaster(inputs(lo, hi, tick, dpr, height), {});
}

std::string text(const BadgePill& p) { return std::string(p.text, size_t(p.length)); }
int inkChars(const BadgePill& p) {
    int n = 0;
    for (int i = 0; i < p.length; ++i) n += p.text[i] != ' ';
    return n;
}

struct BadgeRig {
    chart::LadderLayer layer;
    QSGNode root;
    ChartTextRenderer text;
    chart_raster::RasterCamera cam;
    QQuickWindow* window = nullptr;
    bool atlasReady = true;
    uint64_t serial = 0;
    explicit BadgeRig(const chart_raster::RasterCamera& c) : cam(c) { root.appendChildNode(layer.createContainer()); }
    // A fresh valid, complete radar snapshot covering bins [lo, lo + count) at bin 1.
    LadderSnapshot& snapshot(int64_t lo = 0, int count = 4096, double serverTick = 1, int64_t group = 1) {
        auto& s = layer.mailbox().back();
        s.valid = s.complete = s.radar = true;
        s.serverTick = serverTick;
        s.group = group;
        s.binLo = lo;
        s.rowCount = count;
        s.wallMultiple = 4;
        s.walls = {};
        for (auto& row : s.rows) row = LadderRow{};
        s.serial = ++serial; // the mailbox alternates two buffers: one counter for both
        return s;
    }
    LadderRow& row(int64_t k) { return layer.mailbox().back().rows[size_t(k - layer.mailbox().back().binLo)]; }
    void wall(int64_t k, int side, float size, uint32_t seenMs = 60'000, uint16_t flickers = 0, bool hot = false) {
        auto& r = row(k);
        (side == S::kBid ? r.bid : r.ask) = size;
        (side == S::kBid ? r.bidSeenMs : r.askSeenMs) = seenMs;
        (side == S::kBid ? r.bidFlickers : r.askFlickers) = flickers;
        r.flags |= side == S::kBid ? LadderRow::kBidLarge : LadderRow::kAskLarge;
        if (hot) r.flags |= side == S::kBid ? LadderRow::kBidHot : LadderRow::kAskHot;
    }
    void candidate(int side, int direction, double price, float size, bool capped = false) {
        layer.mailbox().back().walls[size_t(side)][size_t(direction)] = {price, size, true, capped};
    }
    chart::ChartFrame frame() const {
        chart::ChartFrame f;
        f.context.raster = cam;
        f.context.rightGutterPx = kGutter;
        f.context.itemBounds = QRectF(0, 0, 640, cam.heightDev / cam.dpr);
        f.context.surfaceBounds = f.context.itemBounds;
        f.context.surfaceDpr = cam.dpr;
        return f;
    }
    // One frame as the registry runs it: sync, the layer's nodes, then the text stage.
    void draw() {
        const auto f = frame();
        chart::LayerNodeContext context{window, &root, text, atlas(), atlasReady};
        layer.sync(f);
        layer.updateNodes(f, context);
        text.beginFrame(&root, window, atlas());
        layer.submitLabels(f, context);
        text.endFrame();
    }
    void post() {
        layer.mailbox().post();
        draw();
    }
    const std::vector<BadgePill>& pills() const { return layer.badges().pills(); }
    int rowBadges() const { return layer.badges().stats().rowBadges; }
    QSGOpacityNode* gate() { return static_cast<QSGOpacityNode*>(root.firstChild()); }
};

// --------------------------------------------------------------- formatting
TEST(LadderBadges, FormatsMoneyAgesAndFlickersInFixedBuffers) {
    char b[chart::kBadgeChars];
    const auto money = [&](double v) { return std::string(b, size_t(chart::formatMoney(v, b))); };
    EXPECT_EQ(money(0), "$0.00");
    EXPECT_EQ(money(0.5), "$0.50");
    EXPECT_EQ(money(61.26), "$61.3");
    EXPECT_EQ(money(999), "$999");
    EXPECT_EQ(money(999.6), "$1.00k");
    EXPECT_EQ(money(12'345), "$12.3k");
    EXPECT_EQ(money(999'900), "$1.00M");
    EXPECT_EQ(money(1'070'000), "$1.07M");
    EXPECT_EQ(money(2.1e9), "$2.10B");
    EXPECT_EQ(money(5e12), "$999B+");
    EXPECT_EQ(money(std::nan("")), "$0.00");
    const auto age = [&](uint32_t ms) { return std::string(b, size_t(chart::formatAge(ms, b))); };
    EXPECT_EQ(age(12'999), "12s");
    EXPECT_EQ(age(59'999), "59s");
    EXPECT_EQ(age(60'000), "1m00s");
    EXPECT_EQ(age(192'000), "3m12s");
    EXPECT_EQ(age(2 * 3'600'000 + 5 * 60'000), "2h05m");
    EXPECT_EQ(age(0xffffffffu), "99h+");
    EXPECT_EQ(std::string(b, size_t(chart::formatFlickers(65535, b))), "x65535");
}

// ------------------------------------------------------------ (16) layout
TEST(LadderBadges, RowsShorterThanAPillGetNoBadgeButKeepTheirWallColour) {
    for (const double dpr : {1.0, 2.0}) {
        SCOPED_TRACE(dpr);
        BadgeRig rig(camera(80, 110, 1, dpr)); // 30 rows in 320 px: 10.7 px each
        rig.snapshot();
        rig.wall(95, S::kBid, 10);
        rig.post();
        ASSERT_TRUE(rig.layer.drawing());
        EXPECT_EQ(rig.rowBadges(), 0);
        EXPECT_EQ(rig.text.submittedGlyphs(), 0);
        EXPECT_EQ(rig.layer.badgeVertexCount(), 0);
        EXPECT_EQ(rig.layer.vertexCount(), 6) << "the wall bar still draws";
    }
}

TEST(LadderBadges, AGlideFrameAtDpr2NeedsTheDrawnRowHeight) {
    for (const double height : {319.0, 321.0}) { // 20 rows: 15.95 and 16.05 logical px
        SCOPED_TRACE(height);
        BadgeRig rig(chart_raster::continuousRaster(inputs(90, 110, 1, 2.0, height)));
        ASSERT_TRUE(rig.cam.free);
        ASSERT_NE(rig.cam.rowPxF, std::round(rig.cam.rowPxF)) << "fractional rows";
        rig.snapshot();
        rig.wall(100, S::kBid, 10);
        rig.post();
        ASSERT_TRUE(rig.layer.drawing());
        EXPECT_EQ(rig.rowBadges(), height > 320 ? 1 : 0);
    }
}

TEST(LadderBadges, AdjacentWallRowsGetSeparatePillsRightAlignedLeftOfTheStrip) {
    for (const double dpr : {1.0, 2.0}) {
        SCOPED_TRACE(dpr);
        BadgeRig rig(camera(90, 110, 1, dpr)); // 16 px rows
        rig.snapshot();
        rig.wall(100, S::kBid, 12);
        rig.wall(99, S::kBid, 10);
        rig.post();
        ASSERT_EQ(rig.pills().size(), 2u);
        const QRectF a = rig.pills()[0].rect, b = rig.pills()[1].rect;
        EXPECT_FALSE(a.intersects(b));
        EXPECT_DOUBLE_EQ(a.right(), 640 - chart::kBadgeInsetPx);
        EXPECT_DOUBLE_EQ(b.right(), 640 - chart::kBadgeInsetPx);
        EXPECT_LT(a.top(), b.top()) << "from the top: bin 100 first";
        EXPECT_NEAR(a.top() * dpr, std::round(a.top() * dpr), 1e-9) << "on device pixels";
        EXPECT_NEAR(rig.cam.yDev(100.5) / dpr, a.center().y(), 0.5) << "centred on its row";
        EXPECT_EQ(text(rig.pills()[0]), "$1.21k  1m00s");
        EXPECT_EQ(rig.layer.badgeVertexCount(), 2 * 6);
    }
}

TEST(LadderBadges, ABinWithBothSidesLargeGetsOneBadgeForTheLargerNotional) {
    BadgeRig rig(camera(90, 110));
    rig.snapshot();
    rig.wall(100, S::kBid, 10);
    rig.wall(100, S::kAsk, 11);
    rig.wall(102, S::kBid, 12, 5'000); // new
    rig.wall(102, S::kAsk, 9, 60'000, 3, true);
    rig.wall(104, S::kBid, 7);
    rig.wall(104, S::kAsk, 7, 60'000, 3, true); // tie: ask (hot)
    rig.post();
    ASSERT_EQ(rig.pills().size(), 3u);
    EXPECT_EQ(rig.pills()[0].colour, BadgePill::Hot) << "bin 104: tie -> ask";
    EXPECT_EQ(text(rig.pills()[0]), "$732  1m00s  x3");
    EXPECT_EQ(rig.pills()[1].colour, BadgePill::Bid) << "bin 102: bid larger";
    EXPECT_EQ(text(rig.pills()[1]), "$1.23k  5s  new");
    EXPECT_EQ(rig.pills()[2].colour, BadgePill::Ask) << "bin 100: ask larger";
}

TEST(LadderBadges, ARowCutByTheEdgeOrUnderAnEdgePillHasNoBadge) {
    { // bin 110 is half above the top edge
        BadgeRig rig(camera(90.5, 110.5));
        rig.snapshot();
        rig.wall(110, S::kAsk, 10);
        rig.wall(105, S::kAsk, 10);
        rig.post();
        ASSERT_EQ(rig.pills().size(), 1u);
        EXPECT_GE(rig.pills()[0].rect.top(), 64.0) << "only bin 105";
    }
    { // a top-edge pill over bin 109's badge
        BadgeRig rig(camera(90, 110));
        rig.snapshot();
        rig.wall(109, S::kAsk, 10);
        rig.wall(105, S::kAsk, 10);
        rig.candidate(S::kAsk, S::kAbove, 115, 100);
        rig.post();
        EXPECT_EQ(rig.layer.badges().stats().edgePills, 1);
        EXPECT_EQ(rig.rowBadges(), 1) << "bin 109 suppressed, bin 105 drawn";
    }
}

TEST(LadderBadges, AtMost64RowBadgesFromTheTopTheRestCounted) {
    BadgeRig rig(camera(0, 80, 1, 1, 1280)); // 80 rows of 16 px
    rig.snapshot();
    for (int64_t k = 0; k < 80; ++k) rig.wall(k, S::kBid, 10);
    rig.post();
    EXPECT_EQ(rig.rowBadges(), chart::kMaxBadges);
    EXPECT_EQ(rig.layer.badges().stats().dropped, 80 - chart::kMaxBadges);
    EXPECT_LT(rig.pills().front().rect.top(), 16.0) << "the top row first";
}

TEST(LadderBadges, RadarOffOrNoAtlasDrawsNoBadges) {
    for (const bool radar : {false, true}) {
        BadgeRig rig(camera(90, 110));
        rig.atlasReady = radar ? false : true;
        rig.snapshot().radar = radar;
        rig.wall(100, S::kBid, 10);
        rig.post();
        ASSERT_TRUE(rig.layer.drawing());
        EXPECT_TRUE(rig.pills().empty());
        EXPECT_EQ(rig.layer.badgeVertexCount(), 0);
    }
}

// ------------------------------------------------------------ (17) arrows
TEST(LadderBadges, EdgePillsStackAskNearestTheEdgeAndFollowTheCurrentWindow) {
    using chart::kEdgeInsetPx;
    using chart::kPillGapPx;
    using chart::kPillH;
    BadgeRig rig(camera(90, 110));
    rig.snapshot();
    rig.candidate(S::kAsk, S::kAbove, 120, 50);
    rig.candidate(S::kBid, S::kAbove, 115, 40);
    rig.candidate(S::kAsk, S::kBelow, 80, 30, true);
    rig.candidate(S::kBid, S::kBelow, 85, 20);
    rig.post();
    const auto& p = rig.pills();
    ASSERT_EQ(p.size(), 4u);
    EXPECT_EQ(p[0].colour, BadgePill::Ask);
    EXPECT_EQ(p[0].arrow, 1);
    EXPECT_DOUBLE_EQ(p[0].rect.top(), kEdgeInsetPx);
    EXPECT_EQ(p[1].colour, BadgePill::Bid);
    EXPECT_DOUBLE_EQ(p[1].rect.top(), kEdgeInsetPx + kPillH + kPillGapPx);
    EXPECT_EQ(p[2].colour, BadgePill::Ask);
    EXPECT_EQ(p[2].arrow, -1);
    EXPECT_DOUBLE_EQ(p[2].rect.top(), 320 - kEdgeInsetPx - kPillH);
    EXPECT_EQ(p[3].colour, BadgePill::Bid);
    EXPECT_DOUBLE_EQ(p[3].rect.top(), 320 - kEdgeInsetPx - 2 * kPillH - kPillGapPx);
    EXPECT_EQ(text(p[0]), "$6.00k  +$10.0");
    EXPECT_EQ(text(p[2]), "$2.40k  -$10.0");
    EXPECT_TRUE(p[2].hollow);
    for (const auto& pill : p) EXPECT_DOUBLE_EQ(pill.rect.right(), 640 - chart::kBadgeInsetPx);
    // Three filled pills (6 + 3) and one capped (6 + 18 outline).
    EXPECT_EQ(rig.layer.badgeVertexCount(), 3 * (6 + 3) + 6 + 18);

    // The camera moves down after the publish: the bid "above" at 115 is on screen now.
    rig.cam = camera(100, 120);
    rig.draw();
    ASSERT_TRUE(rig.layer.drawing());
    ASSERT_EQ(rig.layer.badges().stats().edgePills, 3);
    EXPECT_EQ(rig.pills()[0].colour, BadgePill::Ask) << "120 is at the drawn top: above";
    EXPECT_EQ(rig.pills()[0].arrow, 1);
    EXPECT_EQ(rig.pills()[1].arrow, -1);
    EXPECT_EQ(rig.pills()[2].arrow, -1);
}

TEST(LadderBadges, ABidAboveTheWindowPointsUpAndAnEdgeKeepsTheLargerOfASide) {
    BadgeRig rig(camera(90, 110));
    rig.snapshot();
    rig.candidate(S::kBid, S::kAbove, 112, 5);
    rig.post();
    ASSERT_EQ(rig.pills().size(), 1u);
    EXPECT_EQ(rig.pills()[0].colour, BadgePill::Bid);
    EXPECT_EQ(rig.pills()[0].arrow, 1);
    EXPECT_DOUBLE_EQ(rig.pills()[0].rect.top(), chart::kEdgeInsetPx) << "alone: at the edge";
    // Both bid candidates above after a pan: one pill, the larger.
    rig.snapshot();
    rig.candidate(S::kBid, S::kAbove, 112, 5);
    rig.candidate(S::kBid, S::kBelow, 89, 9);
    rig.post();
    rig.cam = camera(70, 88);
    rig.draw();
    ASSERT_EQ(rig.pills().size(), 1u);
    EXPECT_EQ(text(rig.pills()[0]).substr(0, 6), "$801  ") << "89 x 9";
}

// ------------------------------------------------------------ (18) nodes
TEST(LadderBadges, TheBadgeNodeIsOutsideTheStripClipAndRecreatedWithTheRoot) {
    BadgeRig rig(camera(90, 110));
    auto* gate = rig.gate();
    ASSERT_EQ(gate->childCount(), 2);
    EXPECT_EQ(gate->firstChild()->type(), QSGNode::ClipNodeType);
    EXPECT_EQ(gate->lastChild()->type(), QSGNode::GeometryNodeType);
    EXPECT_EQ(gate->firstChild()->childCount(), 1) << "only the bars under the clip";
    const auto creations = rig.layer.nodeCreations();
    rig.layer.onRootRebuilt();
    QSGNode root2;
    root2.appendChildNode(rig.layer.createContainer());
    EXPECT_EQ(rig.layer.nodeCreations(), creations + 1);
    EXPECT_EQ(root2.firstChild()->childCount(), 2);
}

// ------------------------------------------------------------ (14) text stage
TEST(LadderBadges, TheTextStageSubmitsTheBadgeGlyphsAndUpdateNodesNone) {
    chart::ChartLayers layers;
    BadgeRig rig(camera(90, 110)); // for its frame and snapshot helpers
    auto& s = layers.ladder.mailbox().back();
    s = LadderSnapshot{};
    s.valid = s.complete = s.radar = true;
    s.serverTick = 1;
    s.group = 1;
    s.binLo = 0;
    s.rowCount = 4096;
    s.serial = 1;
    for (const int64_t k : {100, 103}) {
        s.rows[size_t(k)].bid = 10;
        s.rows[size_t(k)].bidSeenMs = 61'000;
        s.rows[size_t(k)].flags = LadderRow::kBidLarge;
    }
    s.walls[S::kAsk][S::kAbove] = {130, 2, true, false};
    layers.ladder.mailbox().post();
    QSGNode root;
    root.appendChildNode(layers.ladder.createContainer());
    auto& text = layers.text.renderer;
    chart::LayerNodeContext context{nullptr, &root, text, atlas(), true};
    const auto f = rig.frame();
    layers.ladder.sync(f);
    text.beginFrame(&root, nullptr, atlas());
    layers.ladder.updateNodes(f, context);
    EXPECT_EQ(text.submittedGlyphs(), 0) << "updateNodes submits no text";
    ASSERT_TRUE(layers.ladder.drawing());
    layers.text.updateNodes(f, context); // begin, axes, labels (heatmap, ladder), end
    int ink = 0;
    for (const auto& p : layers.ladder.badges().pills()) ink += inkChars(p);
    ASSERT_EQ(layers.ladder.badges().pills().size(), 3u);
    EXPECT_GT(ink, 0);
    EXPECT_EQ(text.submittedGlyphs(), ink);
    EXPECT_EQ(int(layers.ladder.badges().glyphs().size()), ink);
    EXPECT_EQ(text.droppedGlyphs(), 0);
}

// ------------------------------------------------------------ (15b) zero tail
void poison(ChartTextNode& node) {
    auto* g = node.geometry();
    std::memset(g->vertexData(), 0x7f, size_t(g->vertexCount()) * size_t(g->sizeOfVertex()));
}
std::vector<ChartGlyphInstance> glyphs(int n) {
    std::vector<ChartGlyphInstance> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out[size_t(i)].rect = QRectF(i, 1, 2, 3), out[size_t(i)].uv = QRectF(0.1, 0.1, 0.2, 0.2);
    return out;
}
bool zeroFrom(const ChartTextNode& node, int first, int end) {
    for (int g = first; g < end; ++g)
        if (!node.vertexIsZero(g)) return false;
    return true;
}

TEST(ChartTextNodeReserve, TheFirstUpdateAfterAReserveZeroesTheWholeTail) {
    {
        ChartTextNode node;
        node.reserve(1000);
        poison(node); // what an uninitialised allocation may hold
        node.updateGeometry({});
        EXPECT_TRUE(zeroFrom(node, 0, 1000));
    }
    {
        ChartTextNode node;
        node.reserve(1000);
        poison(node);
        node.updateGeometry(glyphs(1));
        EXPECT_FALSE(node.vertexIsZero(0));
        EXPECT_TRUE(zeroFrom(node, 1, 1000));
    }
    {
        ChartTextNode node;
        node.reserve(100);
        node.updateGeometry(glyphs(100));
        node.reserve(1000);
        EXPECT_EQ(node.capacityGlyphs(), 1000);
        poison(node);
        node.updateGeometry(glyphs(10));
        EXPECT_TRUE(zeroFrom(node, 10, 1000));
        node.reserve(500); // not larger: a no-op
        EXPECT_EQ(node.capacityGlyphs(), 1000);
    }
}

// --------------------------------------------- (15) allocation through endFrame
struct ColourNodes {
    ChartTextNode* bid = nullptr;
    ChartTextNode* ask = nullptr;
    ChartTextNode* hot = nullptr;
};
ColourNodes colourNodes(QSGNode& root) {
    ColourNodes out;
    const auto same = [](const QVector4D& a, QRgb b) { return std::abs(a.x() - qRed(b) / 255.0f) < 1e-3f && std::abs(a.y() - qGreen(b) / 255.0f) < 1e-3f && std::abs(a.z() - qBlue(b) / 255.0f) < 1e-3f; };
    for (QSGNode* top = root.firstChild(); top; top = top->nextSibling())
        for (QSGNode* n = top->firstChild(); n; n = n->nextSibling()) {
            auto* node = dynamic_cast<ChartTextNode*>(n);
            if (!node) continue;
            const auto& colour = static_cast<ChartTextMaterial*>(node->material())->color();
            if (same(colour, chart::kBidText)) out.bid = node;
            if (same(colour, chart::kAskText)) out.ask = node;
            if (same(colour, chart::kHotText)) out.hot = node;
        }
    return out;
}

// Frame f (1-based): min(f, 70) wall rows from the top (at most 64 badges); asks from frame 30, the hot
// colour from frame 45, the four edge pills from frame 60; ages and sizes cross digit
// boundaries ($999k -> $1.00M, 59s -> 1m00s, x9 -> x10).
void fillFrame(BadgeRig& rig, int f) {
    auto& s = rig.snapshot(1000, 200);
    for (int64_t k = 1000; k < 1200; ++k) rig.row(k).bid = 1;
    const int walls = std::min(f, 70);
    for (int j = 0; j < walls; ++j) {
        const int64_t k = 1074 - j;
        const int side = f >= 30 && j % 2 ? S::kAsk : S::kBid;
        const float size = float((995'000.0 + f * 1000.0) / (double(k) + 0.5));
        rig.wall(k, side, size, uint32_t((f + 50) * 1000), uint16_t(j == 1 ? f : 0), f >= 45 && j == 5);
    }
    if (f >= 60) {
        rig.candidate(S::kAsk, S::kAbove, 1090, 5, true);
        rig.candidate(S::kBid, S::kAbove, 1080, 4);
        rig.candidate(S::kAsk, S::kBelow, 990, 3);
        rig.candidate(S::kBid, S::kBelow, 995, 2);
    }
    (void)s;
}

TEST(LadderBadgesGpu, WarmedFramesAllocateNothingThroughEndFrame) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
        GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    gAllocations = 0;
    gCounting = true;
    { chart::LadderMailbox probe; } // library allocations (a local new/delete pair can be elided)
    gCounting = false;
    ASSERT_GE(gAllocations.load(), 1u) << "the counter sees allocations";

    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(640, 1200), &error)) << error.toStdString();
    ASSERT_TRUE(atlas().isBuilt());
    BadgeRig rig(camera(1000, 1075, 1, 1, 1200)); // 75 rows of 16 px
    rig.window = scene.window();

    fillFrame(rig, 1);
    rig.post();
    ASSERT_TRUE(rig.layer.drawing());
    EXPECT_EQ(rig.text.textureCount(), 3) << "every badge colour reserved with its texture";
    const size_t bytes = rig.text.capacityBytes();
    const int capacity = rig.text.nodeCapacityGlyphs();
    EXPECT_GE(capacity, 3 * chart::kMaxBadgeGlyphs);
    const size_t layoutBytes = rig.layer.badges().capacityBytes();
    std::vector<std::string> seen;
    size_t allocations = 0;
    int maxBadges = 0;
    for (int f = 2; f <= 120; ++f) {
        SCOPED_TRACE(f);
        fillFrame(rig, f);
        rig.layer.mailbox().post();
        gAllocations = 0;
        gCounting = true;
        rig.draw();
        gCounting = false;
        allocations += gAllocations.load();
        EXPECT_EQ(gAllocations.load(), 0u);
        ASSERT_TRUE(rig.layer.drawing());
        maxBadges = std::max(maxBadges, rig.rowBadges());
        for (const auto& p : rig.pills()) seen.push_back(text(p));
        const auto nodes = colourNodes(rig.root);
        ASSERT_TRUE(nodes.bid && nodes.ask && nodes.hot);
        EXPECT_FALSE(nodes.bid->vertexIsZero(0)) << "bid text drawn";
        EXPECT_EQ(!nodes.ask->vertexIsZero(0), f >= 30) << "ask text from frame 30";
        EXPECT_EQ(!nodes.hot->vertexIsZero(0), f >= 45) << "hot text from frame 45";
        EXPECT_EQ(rig.layer.badges().stats().edgePills, f >= 60 ? 4 : 0);
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(rig.text.textureCount(), 3);
    EXPECT_EQ(rig.text.capacityBytes(), bytes);
    EXPECT_EQ(rig.text.nodeCapacityGlyphs(), capacity);
    EXPECT_EQ(rig.layer.badges().capacityBytes(), layoutBytes);
    EXPECT_EQ(maxBadges, chart::kMaxBadges);
    const auto has = [&](const char* needle) {
        for (const auto& t : seen)
            if (t.find(needle) != std::string::npos) return true;
        return false;
    };
    EXPECT_TRUE(has("$999k") || has("$998k")) << "below a million";
    EXPECT_TRUE(has("$1.00M"));
    EXPECT_TRUE(has("59s") && has("1m00s"));
    EXPECT_TRUE(has("x9") && has("x10"));

    // A second reservation allocates nothing.
    rig.text.beginFrame(&rig.root, rig.window, atlas());
    gAllocations = 0;
    gCounting = true;
    EXPECT_TRUE(rig.text.reserve(QColor::fromRgba(chart::kBidText), chart::kMaxBadgeGlyphs));
    gCounting = false;
    rig.text.endFrame();
    EXPECT_EQ(gAllocations.load(), 0u);
    EXPECT_EQ(rig.text.nodeCapacityGlyphs(), capacity);

    // A new root: the first frame reserves again (and allocates), then nothing.
    rig.layer.onRootRebuilt();
    rig.text.onRootRebuilt();
    QSGNode root2;
    root2.appendChildNode(rig.layer.createContainer());
    const auto drawOn = [&](QSGNode& root) {
        const auto frame = rig.frame();
        chart::LayerNodeContext context{rig.window, &root, rig.text, atlas(), true};
        rig.layer.sync(frame);
        rig.layer.updateNodes(frame, context);
        rig.text.beginFrame(&root, rig.window, atlas());
        rig.layer.submitLabels(frame, context);
        rig.text.endFrame();
    };
    gAllocations = 0;
    gCounting = true;
    drawOn(root2);
    gCounting = false;
    EXPECT_GT(gAllocations.load(), 0u) << "the new root's nodes and textures";
    EXPECT_EQ(rig.text.textureCount(), 3);
    const auto fresh = colourNodes(root2);
    ASSERT_TRUE(fresh.bid && fresh.ask && fresh.hot);
    for (auto* node : {fresh.bid, fresh.ask, fresh.hot}) {
        int used = 0;
        while (used < node->capacityGlyphs() && !node->vertexIsZero(used)) ++used;
        EXPECT_TRUE(zeroFrom(*node, used, node->capacityGlyphs())) << "the new node's unused tail";
    }
    for (int i = 0; i < 3; ++i) {
        gAllocations = 0;
        gCounting = true;
        drawOn(root2);
        gCounting = false;
        EXPECT_EQ(gAllocations.load(), 0u) << "frame " << i + 2 << " of the new root";
    }
}

TEST(LadderBadgesGpu, AReservedNodeZeroesItsTailThroughTheRenderer) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
        GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(64, 64), &error)) << error.toStdString();
    ChartTextRenderer text;
    QSGNode root;
    EXPECT_FALSE(text.reserve(QColor(Qt::red), 1000)) << "outside a frame";
    text.beginFrame(&root, scene.window(), atlas());
    ASSERT_TRUE(text.reserve(QColor(Qt::red), 1000));
    EXPECT_EQ(text.textureCount(), 1);
    auto* node = dynamic_cast<ChartTextNode*>(root.firstChild()->firstChild());
    ASSERT_TRUE(node);
    poison(*node);
    text.endFrame(); // the colour is unused this frame
    EXPECT_TRUE(zeroFrom(*node, 0, 1000));
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
