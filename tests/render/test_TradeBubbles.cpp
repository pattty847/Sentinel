#include <gtest/gtest.h>
#include "render/TradeBubbleNode.hpp"
#include "chart/shell/TradeBubbleStyleStore.hpp"
#include "themes/ThemePalette.hpp"
#include "ConfigLoader.hpp"
#include "chart/feeds/TradeBubbleFeed.hpp"
#include "config/GuiConfigStore.hpp"
#include <QFile>
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include <QTemporaryDir>
#include <QSettings>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <iostream>

namespace {
thread_local bool countAllocations = false;
thread_local size_t allocations = 0;
}
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

using namespace trade_bubbles;
namespace {
void expectDrawCount(const TradeBubbleNode& node) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    EXPECT_EQ(node.geometry()->vertexCount(), node.usedVertexCount());
#else
    EXPECT_EQ(node.geometry()->vertexCount(), int(Layout::MaxBubbles * 6));
    if (node.usedVertexCount() < node.geometry()->vertexCount()) {
        const auto* unused = static_cast<const float*>(node.geometry()->vertexData()) + node.usedVertexCount() * 14;
        for (int i = 0; i < 6 * 14; ++i) EXPECT_EQ(unused[i], 0);
    }
#endif
}
TimeAxisMapping mapping(double width = 640, double height = 320) {
    TimeAxisMapping m;
    m.valid = true;
    m.viewStartMs = m.dataStartMs = 1000;
    m.viewEndMs = 2000;
    m.viewMinPrice = m.dataMinPrice = 100;
    m.viewMaxPrice = m.dataMaxPrice = 200;
    m.appendMs = 100;
    m.tickSize = 1;
    m.drawRect = {10,20,width,height};
    m.srcRect = {0,0,10,100};
    return m;
}
}
TEST(TradeBubbles, PreallocatedScratchStaysWithinPerChartBudget) {
    const size_t scratch = Layout::scratchBytes() + chart::bubbles::ReferenceStatistics::storageBytes();
    const size_t tape = sizeof(Tape);
    const size_t vertices = Layout::MaxBubbles * 6 * 14 * sizeof(float);
    std::cout << "BUBBLE_MEMORY tape=" << tape << " scratch=" << scratch
              << " vertices=" << vertices << " total=" << tape+scratch+vertices << '\n';
    EXPECT_LT(scratch,25u*1024*1024);
}
TEST(TradeBubbles, PlacementProjectsPolicyMarksAndCapsules) {
    Layout layout;
    auto m = mapping();
    const Sample rows[] = {{1500,150,1,AggressorSide::Buy,"1"}, {1500,175,1,AggressorSide::Buy,"2"}};
    const auto b = layout.build({rows, {}}, m, {chart::bubbles::Mode::Individual, {0,325}, 1});
    ASSERT_EQ(b.size(),1);
    EXPECT_FLOAT_EQ(b[0].x,330);
    EXPECT_FLOAT_EQ(b[0].minY,100);
    EXPECT_FLOAT_EQ(b[0].maxY,180);
    EXPECT_FLOAT_EQ(b[0].radius,9);
    EXPECT_FLOAT_EQ(b[0].opacity,1);
}
TEST(TradeBubbles, PlacementUsesExchangeTimeAndPriceWithoutRingOffset) {
    Layout layout;
    auto m = mapping(); m.timeOffset = .75f;
    const Sample trades[] = {{1250,175,2,AggressorSide::Buy}};
    const chart::bubbles::Options opts{chart::bubbles::Mode::Individual,{0,350},0};
    const auto b = layout.build({trades,{}},m,opts);
    ASSERT_EQ(b.size(),1);
    EXPECT_FLOAT_EQ(b[0].x,170);
    EXPECT_FLOAT_EQ(b[0].y,100);
    m.viewStartMs += 100; m.viewEndMs += 100; m.srcRect.translate(1,0);
    const auto p = layout.build({trades,{}},m,opts);
    ASSERT_EQ(p.size(),1);
    EXPECT_FLOAT_EQ(p[0].x,106);
    EXPECT_FLOAT_EQ(p[0].y,100);
}
TEST(TradeBubbles, NearbyExecutionsStaySeparateWithinCapacity) {
    auto tape = std::make_unique<Tape>();
    for (int i = 0; i < 60; ++i)
        ASSERT_TRUE(tape->append({1000+i/10,150,1,AggressorSide::Buy}));
    Layout layout;
    const auto m = mapping();
    const auto b = layout.build(tape->samples(),m,{});
    ASSERT_EQ(b.size(),60);
    EXPECT_EQ(layout.binSizePx(),0);
    for (const auto& bubble : b) {
        EXPECT_EQ(bubble.notional,150);
    }
}
TEST(TradeBubbles, SlotAggregateAndNotablesUseAggregateReferenceAndRadiusCap) {
    Layout layout;
    auto m = mapping(); m.appendMs = 1000; m.srcRect.setWidth(1);
    const Sample rows[] = {{1200,150,1,AggressorSide::Buy}, {1300,150,1,AggressorSide::Buy}};
    const auto bubbles = layout.build({rows,{}},m,{chart::bubbles::Mode::Slot,{0,150,1200},0});
    ASSERT_EQ(bubbles.size(),3);
    for (const auto& bubble : bubbles) {
        if (bubble.notional == 300) EXPECT_FLOAT_EQ(bubble.radius,4.5f);
        else {
            EXPECT_NEAR(bubble.radius,9*std::sqrt(150.0/1200),1e-5);
            EXPECT_LE(bubble.radius,4.5f);
        }
    }
}
TEST(TradeBubbles, FirstSlotFrameHasVisibleRadiiWithinUrgentFloor) {
    const Sample rows[] = {{1200,150,1,AggressorSide::Buy}, {1300,150,1,AggressorSide::Buy}};
    const Samples samples{rows,{}};
    const PolicyRows policy{samples};
    chart::bubbles::ReferenceStatistics stats;
    const chart::bubbles::WindowStamp stamp{1,2,1,2,2,2};
    const chart::bubbles::VisibleView view{1000,2000,1000,0};
    stats.update(policy,policy,stamp,view,chart::bubbles::Mode::Individual,1000);
    const auto slot = stats.update(policy,policy,stamp,view,chart::bubbles::Mode::Slot,1050);
    Layout layout;
    auto m = mapping(); m.appendMs = 1000;
    const auto bubbles = layout.build(samples,m,{chart::bubbles::Mode::Slot,slot,0});
    ASSERT_FALSE(bubbles.empty());
    for (const auto& bubble : bubbles) EXPECT_GT(bubble.radius,0);
}
TEST(TradeBubbles, SpatialOverflowRemainsBoundedAndReusesScratch) {
    auto tape = std::make_unique<Tape>();
    for (int i=0; i<100000; ++i)
        tape->append({1000+i/100,100.+(i%100),1,AggressorSide::Buy});
    Layout layout;
    const auto m = mapping(1847,901);
    auto b = layout.build(tape->samples(),m,{chart::bubbles::Mode::Individual,{0,195},1});
    EXPECT_LE(b.size(),Layout::MaxBubbles);
    EXPECT_GT(b.size(),1000);
    EXPECT_GE(layout.binSizePx(),6);
    EXPECT_EQ(layout.scannedRows(),100000);
}
TEST(TradeBubbles, PredecessorChangesInvalidatePassiveOpacity) {
    auto tape = std::make_unique<Tape>();
    tape->append({999,150,1,AggressorSide::Buy});
    tape->append({1500,150,1,AggressorSide::Buy});
    TradeBubbleNode node;
    chart::bubbles::Options options{chart::bubbles::Mode::Individual,{0,150},1};
    node.sync(*tape,mapping(),true,options);
    const auto* vertices = static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(vertices[7],.14f,1e-6);
    const auto builds = node.rebuildCount();
    tape->append({999,149,1,AggressorSide::Sell});
    node.sync(*tape,mapping(),true,options);
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_FLOAT_EQ(vertices[7],.4f);
}
TEST(TradeBubbles, RebuildsInBothModesDoNotAllocateAndKeepQsgStorage) {
    auto tape = std::make_unique<Tape>();
    for (int i = 0; i < 100000; ++i)
        tape->append({1000+i/100,100.+(i%100),1,AggressorSide::Buy});
    TradeBubbleNode node;
    auto m = mapping(1847,901);
    chart::bubbles::Options options{chart::bubbles::Mode::Individual,{150,195},1};
    node.sync(*tape,m,true,options);
    auto* storage = node.geometry()->vertexData();
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 6; ++i) {
        options.mode = i % 2 ? chart::bubbles::Mode::Slot : chart::bubbles::Mode::Individual;
        m.drawRect.setWidth(1848+i);
        node.sync(*tape,m,true,options);
    }
    countAllocations = false;
    EXPECT_EQ(allocations, 0);
    EXPECT_EQ(node.geometry()->vertexData(), storage);
    EXPECT_GT(node.usedVertexCount(), 0);
}
TEST(TradeBubbles, PartialSlotsUseWholeBarAndTimeframeChangesInvalidateGeometry) {
    auto tape = std::make_unique<Tape>();
    tape->append({1200,100,1,AggressorSide::Buy});
    tape->append({1600,200,1,AggressorSide::Buy});
    auto m = mapping();
    m.viewStartMs = 1500; m.viewEndMs = 1700; m.appendMs = 1000;
    m.srcRect = {.5,0,.2,100};
    chart::bubbles::Options options{chart::bubbles::Mode::Slot,{0,200},1};
    TradeBubbleNode node;
    node.sync(*tape,m,true,options);
    EXPECT_EQ(node.lastScanRows(), 2);
    EXPECT_EQ(node.usedVertexCount(), 18);
    const auto builds = node.rebuildCount();
    // Keep the time scale fixed, but change bar alignment/centre.
    m.appendMs = 2000; m.srcRect = {.25,0,.1,100};
    node.sync(*tape,m,true,options);
    EXPECT_EQ(node.rebuildCount(), builds + 1);
}
TEST(TradeBubbles, CapsulesKeepMinimumWidthAndPassiveOpacity) {
    auto tape = std::make_unique<Tape>();
    tape->append({1500,150,1,AggressorSide::Buy,"1"});
    tape->append({1500,181.25,1,AggressorSide::Buy,"2"});
    tape->append({1600,181.25,1,AggressorSide::Buy,"3"});
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,{chart::bubbles::Mode::Individual,{0,100000},1});
    ASSERT_EQ(node.usedVertexCount(),12);
    const auto* v = static_cast<const float*>(node.geometry()->vertexData());
    const double radius = (v[14]-v[0])/2;
    constexpr double pi = 3.14159265358979323846;
    EXPECT_GE(radius,1.5);
    EXPECT_GT(pi*radius*radius+2*100*radius,pi*4);
    EXPECT_FLOAT_EQ(v[7],.4f);
    EXPECT_NEAR(v[6*14+7],.14f,1e-6);
}
TEST(TradeBubbles, LargerCirclesDrawFirstAcrossSidesAndNeighbouringCells) {
    Layout layout;
    const Sample rows[]={{1500,150,1,AggressorSide::Buy}, {1500,150,4,AggressorSide::Sell},
                         {1501,150,2,AggressorSide::Buy}};
    const chart::bubbles::Options options{chart::bubbles::Mode::Individual,{0,600},0};
    auto b=layout.build({rows,{}},mapping(),options);
    ASSERT_EQ(b.size(),3);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_GT(b[0].radius,b[1].radius);
    EXPECT_GT(b[1].radius,b[2].radius);
    auto tape=std::make_unique<Tape>();
    for (int i=0;i<5000;++i) { auto row=rows[i%3]; row.size/=1000; tape->append(row); }
    StyleModel look; look.minimumBaseSize = 0;
    b=layout.build(tape->samples(),mapping(),options,0,look);
    ASSERT_EQ(b.size(),2);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_GT(b.front().radius,b.back().radius);
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,options,look);
    const auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_FLOAT_EQ(v[4],246.f/255*.14f);
    EXPECT_FLOAT_EQ(v[6],93.f/255*.14f);
}
TEST(TradeBubbles, OrderedRingBinarySearchBoundsScanAndPanReusesGeometry) {
    auto tape=std::make_unique<Tape>();
    for (int64_t i=1;i<=int64_t(Tape::Capacity)+10;++i)
        tape->append({i,150,1,AggressorSide::Buy});
    const auto window=tape->visible(1500,1503);
    ASSERT_EQ(window.rows.size(),3);
    EXPECT_EQ(window.rows.front().timeMs,1500);
    EXPECT_EQ(window.rows.back().timeMs,1502);
    EXPECT_LE(window.comparisons,34);
    const auto wrapped=tape->visible(99998,100009);
    ASSERT_EQ(wrapped.rows.size(),11);
    EXPECT_FALSE(wrapped.rows.first.empty());
    EXPECT_FALSE(wrapped.rows.second.empty());
    for (size_t i=0;i<wrapped.rows.size();++i) EXPECT_EQ(wrapped.rows[i].timeMs,99998+int64_t(i));
    EXPECT_LE(wrapped.comparisons,34);
    // Use sub-ms pan with no membership changes, as in smooth follow-live.
    auto m=mapping(); m.viewStartMs=1499.1; m.viewEndMs=1502.1;
    m.srcRect={4.991,0,.03,100};
    TradeBubbleNode node;
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    ASSERT_EQ(node.lastScanRows(),3);
    const auto builds=node.rebuildCount();
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.rebuildCount(),builds);
    EXPECT_EQ(node.lastScanRows(),0);
    const auto* vertices=static_cast<const float*>(node.geometry()->vertexData());
    const float x=vertices[0];
    for (int i=0;i<5;++i) {
        m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
        tape->append({100100+i,150,1,AggressorSide::Buy}); // Outside view; ring eviction also outside.
        node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
        EXPECT_EQ(node.rebuildCount(),builds);
        EXPECT_EQ(node.lastScanRows(),0);
        EXPECT_LE(node.rangeComparisons(),34);
        EXPECT_FLOAT_EQ(vertices[0],x);
        EXPECT_NEAR(node.translation()(0,3),-(i+1)*.1*640/3,0.001);
    }
    // Late insertion inside the view must invalidate the cached mesh.
    ASSERT_TRUE(tape->append({1501,151,1,AggressorSide::Sell}));
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_EQ(node.lastScanRows(),4);
    EXPECT_EQ(node.usedVertexCount(),24);
    // Moving across a boundary drops a row and forces another rebuild.
    m.viewStartMs=1500.1; m.viewEndMs=1503.1; m.srcRect.translate(.005,0);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.rebuildCount(),builds+2);
}
TEST(TradeBubbles, DeduplicatesRetainedIdsButPreservesUnidentifiedExecutions) {
    auto tape=std::make_unique<Tape>();
    Sample row{1500,150,1,AggressorSide::Buy,"exchange/123"};
    ASSERT_TRUE(tape->append(row));
    const auto revision=tape->revision();
    EXPECT_FALSE(tape->append(row));
    row.timeMs=1501; row.size=2;
    EXPECT_FALSE(tape->append(row)); // ID, not payload identity.
    EXPECT_EQ(tape->revision(),revision);
    row.tradeId="exchange/124";
    EXPECT_TRUE(tape->append(row));
    row.tradeId.clear();
    EXPECT_TRUE(tape->append(row));
    EXPECT_TRUE(tape->append(row));
    EXPECT_EQ(tape->samples().size(),4);
    tape->clear();
    row.tradeId="exchange/123";
    EXPECT_TRUE(tape->append(row)); // Symbol/clear resets the ID namespace.
    for (size_t i=0;i<Tape::Capacity;++i)
        tape->append({2000+int64_t(i),150,1,AggressorSide::Buy});
    row.timeMs=200000;
    EXPECT_TRUE(tape->append(row)); // Expired IDs do not accumulate forever.
}
TEST(TradeBubbles, InvalidAndOffscreenRowsCannotReachGeometry) {
    auto tape=std::make_unique<Tape>();
    EXPECT_FALSE(tape->append({0,150,1,AggressorSide::Buy}));
    EXPECT_FALSE(tape->append({1500,NAN,1,AggressorSide::Buy}));
    EXPECT_FALSE(tape->append({1500,150,INFINITY,AggressorSide::Buy}));
    EXPECT_FALSE(tape->append({1500,150,1,AggressorSide::Unknown}));
    EXPECT_TRUE(tape->samples().empty());
    Layout layout;
    const Sample rows[]={{999,150,1,AggressorSide::Buy},{2000,150,1,AggressorSide::Sell},
                         {1500,201,1,AggressorSide::Sell}};
    EXPECT_TRUE(layout.build({rows,{}},mapping(),{}).empty());
}
TEST(TradeBubbles, TapeAndQsgCapacityNeverGrowAcrossFramesAndToggleClearsGeometry) {
    auto tape=std::make_unique<Tape>();
    const auto* storage=tape->storage();
    for (size_t i=0;i<Tape::Capacity+10;++i) tape->append({1500,150,1,AggressorSide::Buy});
    ASSERT_EQ(tape->samples().size(),Tape::Capacity);
    EXPECT_EQ(tape->storage(),storage);
    TradeBubbleNode node;
    auto* vertices=node.geometry()->vertexData();
    auto m=mapping();
    for (int i=0;i<30;++i) {
        m.drawRect.setWidth(600+i);
        node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
        ASSERT_EQ(node.geometry()->vertexData(),vertices);
        ASSERT_EQ(node.usedVertexCount(),6);
        expectDrawCount(node);
    }
    // Overload falls back to one capped spatial bin without growing geometry.
    auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(v[0],m.timeToScreenX(1500)-18,1e-4);
    EXPECT_NEAR(v[1],m.priceToScreenY(150)-18,1e-4);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.usedVertexCount(),0);
    expectDrawCount(node);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.usedVertexCount(),6);
    tape->clear();
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.usedVertexCount(),0);
    expectDrawCount(node);
    EXPECT_EQ(node.geometry()->vertexData(),vertices);
}
TEST(TradeBubbles, UnchangedFramesAndEmptyMeshesLeaveTheSceneGraphAlone) {
    // A dirty mark after synchronization asks the window for another frame, so
    // an idle chart must get none from the bubble node (UgrGpu idle test).
    auto tape=std::make_unique<Tape>();
    TradeBubbleNode node;
    auto m=mapping();
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1}); // off, empty: nothing to draw
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});  // on, empty: still nothing
    EXPECT_EQ(node.dirtyMarks(),0u);
    ASSERT_TRUE(tape->append({1500,150,1,AggressorSide::Buy}));
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1});
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1}); // identical frame
    m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1}); // follow-live pan: translation only
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1}); // off: the drawn mesh is removed
    EXPECT_EQ(node.dirtyMarks(),2u);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1});
    node.clear();                                     // nothing drawn: no mark
    EXPECT_EQ(node.dirtyMarks(),2u);
}
TEST(TradeBubbles, ZeroMinimumLoadsAndPersistsAsAll) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QSettings ini(dir.filePath("legacy.ini"),QSettings::IniFormat);
    ini.setValue("heatmap/old/tradeMinNotional",0);
    heatmap::HeatmapSettingsStore store(ini);
    EXPECT_EQ(store.load("old",{}).tradeMinNotional,0);
    auto current = store.load("old",{});
    ASSERT_TRUE(store.applyChartPatch("old",current,{{"tradeMinNotional",-1}},true,{},"BTC-USD",60000).isEmpty());
    ini.sync();
    EXPECT_EQ(store.load("old",{}).tradeMinNotional,-1);
    EXPECT_FALSE(ini.contains("heatmap/old/tradeMinNotionalModeVersion"));
}
TEST(TradeBubbles, SettingsRoundTripPerChartAndTransientPatchesStayTransient) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path=dir.filePath("trades.ini");
    {
        QSettings ini(path,QSettings::IniFormat);
        heatmap::HeatmapSettingsStore store(ini);
        auto s=store.load("one",{});
        EXPECT_FALSE(s.showTrades); EXPECT_EQ(s.tradeMinNotional,0);
        EXPECT_EQ(s.bubblePlacement,"individual");
        ASSERT_TRUE(store.applyChartPatch("one",s,{{"showTrades",true},{"tradeMinNotional",1000},{"bubblePlacement","slot"}},true,{},"BTC-USD",60000).isEmpty());
        ASSERT_TRUE(store.applyChartPatch("one",s,{{"tradeMinNotional",100000},{"bubblePlacement","auto"}},false,{},"BTC-USD",60000).isEmpty());
        EXPECT_EQ(s.tradeMinNotional,100000);
        ini.sync();
    }
    QSettings ini(path,QSettings::IniFormat);
    heatmap::HeatmapSettingsStore store(ini);
    const auto s=store.load("one",{});
    EXPECT_TRUE(s.showTrades); EXPECT_EQ(s.tradeMinNotional,1000);
    EXPECT_EQ(s.bubblePlacement,"slot");
    EXPECT_FALSE(store.load("two",{}).showTrades);
    EXPECT_EQ(heatmap::settingsJson(s)["tradeMinNotional"].toDouble(),1000);
    auto invalid=s;
    EXPECT_FALSE(heatmap::applySettingsPatch(invalid,{{"showTrades","yes"}}).isEmpty());
    EXPECT_EQ(invalid,s);
    EXPECT_FALSE(heatmap::applySettingsPatch(invalid,{{"bubblePlacement","unknown"}}).isEmpty());
    EXPECT_EQ(invalid,s);
}

#include "render/TradeBubbleOverlayItem.hpp"
#include "chart/feeds/CvdFeed.hpp"
#include <cstring>
TEST(TradeBubbles, DockedPaneClipsWithoutChangingMappingOrMeshAndAllocatesNothing) {
    auto tape = std::make_shared<Tape>();
    tape->append({1500,105,2,AggressorSide::Buy});
    trade_bubbles::RenderLink link;
    link.frame = std::make_shared<RenderFrame>(tape);
    auto& f = *link.frame;
    f.mapping = mapping(); f.enabled = true;
    f.options = {chart::bubbles::Mode::Individual,{0,210},1};
    TradeBubbleNode node; link.node = &node;
    chart::CvdPresentation pane; link.presentation = &pane;
    pane.dataTimeframeMs = 1000; pane.hasData = true;
    link.sync();
    ASSERT_GT(node.usedVertexCount(), 0);
    const QByteArray before(static_cast<const char*>(node.geometry()->vertexData()), node.usedVertexCount() * 14 * sizeof(float));
    const auto builds = node.rebuildCount();
    EXPECT_EQ(node.clipRect(), f.mapping.drawRect);
    for (const int strip : {0, 60}) {
        pane.flowFooter = true; pane.stripPx = strip;
        allocations = 0; countAllocations = true;
        link.sync(); link.sync();
        countAllocations = false;
        EXPECT_EQ(allocations, 0u);
        EXPECT_EQ(node.rebuildCount(), builds);
        EXPECT_EQ(node.clipRect().bottom(), f.mapping.drawRect.bottom() - strip - chart::FlowFooterLayout::height);
        EXPECT_EQ(std::memcmp(before.constData(), node.geometry()->vertexData(), size_t(before.size())), 0);
        const auto* vertices = static_cast<const float*>(node.geometry()->vertexData());
        bool covered = false;
        for (int i = 0; i < node.usedVertexCount(); ++i) {
            covered |= vertices[i * 14 + 1] > node.clipRect().bottom();
        }
        EXPECT_TRUE(covered) << "the fixture must exercise the excluded part of the bubble";
    }
    pane.flowFooter = false; pane.stripPx = 0; link.sync();
    EXPECT_EQ(node.clipRect(), f.mapping.drawRect);
    EXPECT_EQ(std::memcmp(before.constData(), node.geometry()->vertexData(), size_t(before.size())), 0);
}

TEST(TradeBubbleStyle, FreshStoreUsesOwnerDefaultsAndDirectionTokens) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("style.ini"), QSettings::IniFormat);
    chart::TradeBubbleStyleStore store(ini);
    const auto s = store.load("BTC-USD");
    EXPECT_EQ(s, StyleModel{});
    EXPECT_EQ(s.buy.color.r, theme::positive().red());
    EXPECT_EQ(s.buy.color.g, theme::positive().green());
    EXPECT_EQ(s.sell.color.r, theme::negative().red());
    EXPECT_EQ(s.sell.color.b, theme::negative().blue());
    EXPECT_TRUE(heatmap::HeatmapChartSettings{}.tradesAboveCandles);
}
TEST(TradeBubbleStyle, RoundTripClampsAndResetRestoresDefaults) {
    QTemporaryDir dir;
    const auto path = dir.filePath("style.ini");
    StyleModel s;
    s.buy = {{1,2,3,255}, .17, false, 37};
    s.sell = {{4,5,6,255}, .83, true, 11};
    s.ring = {{7,8,9,255}, .71, false, 2.3};
    s.minimumBaseSize = .12345678;
    {
        QSettings ini(path, QSettings::IniFormat);
        chart::TradeBubbleStyleStore store(ini);
        store.save("BTC-USD", s, true);
        ini.sync();
    }
    QSettings ini(path, QSettings::IniFormat);
    chart::TradeBubbleStyleStore store(ini);
    EXPECT_EQ(store.load("BTC-USD"), s);
    s.buy.opacity = -2; s.sell.opacity = 3; s.ring.opacity = -1;
    s.buy.width = 1; s.sell.width = 500; s.ring.width = 4;
    s.minimumBaseSize = -1;
    store.save("BTC-USD", s, true);
    const auto clipped = store.load("BTC-USD");
    EXPECT_EQ(clipped.buy.opacity, 0); EXPECT_EQ(clipped.sell.opacity, 1);
    EXPECT_EQ(clipped.ring.opacity, 0);
    EXPECT_EQ(clipped.buy.width, 4); EXPECT_EQ(clipped.sell.width, 48);
    EXPECT_EQ(clipped.ring.width, 3); EXPECT_EQ(clipped.minimumBaseSize, 0);
    ini.setValue("tradeBubbles/style/ring.width", -4);
    ini.setValue("tradeBubbles/style/buy.opacity", 5);
    ini.setValue("tradeBubbles/style/sell.width", 2);
    ini.setValue("tradeBubbles/minimum/BTC-USD", -9);
    EXPECT_EQ(store.load("BTC-USD").ring.width, 0);
    EXPECT_EQ(store.load("BTC-USD").buy.opacity, 1);
    EXPECT_EQ(store.load("BTC-USD").sell.width, 4);
    EXPECT_EQ(store.load("BTC-USD").minimumBaseSize, 0);
    store.clear("BTC-USD");
    EXPECT_FALSE(store.hasMinimum("BTC-USD"));
    EXPECT_EQ(store.load("BTC-USD"), StyleModel{});
}
TEST(TradeBubbleStyle, ProductMinimumIsIndependentAndUsesObservedPrices) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("style.ini"), QSettings::IniFormat);
    chart::TradeBubbleStyleStore store(ini);
    chart::TradeBubbleStyleStore::notePrice("BTC-USD", 80000);
    chart::TradeBubbleStyleStore::notePrice("ETH-USD", 4000);
    EXPECT_DOUBLE_EQ(store.load("BTC-USD").minimumBaseSize, .38);
    EXPECT_DOUBLE_EQ(store.load("ETH-USD").minimumBaseSize, 7.6);
    auto s = store.load("ETH-USD"); s.minimumBaseSize = 2;
    store.save("ETH-USD", s, true);
    EXPECT_DOUBLE_EQ(store.load("BTC-USD").minimumBaseSize, .38);
    EXPECT_DOUBLE_EQ(store.load("ETH-USD").minimumBaseSize, 2);
    store.clear("ETH-USD");
    EXPECT_DOUBLE_EQ(store.load("ETH-USD").minimumBaseSize, 7.6);
    EXPECT_DOUBLE_EQ(defaultMinimum("OTHER-USD", 2000, 0), 19);
    EXPECT_DOUBLE_EQ(defaultMinimum("OTHER-USD", 0, 80000), 0);
}
TEST(TradeBubbleStyle, MeshUsesDirectionFillAndLogicalRingForBothSides) {
    auto tape = std::make_unique<Tape>();
    tape->append({999,149,1,AggressorSide::Buy});
    tape->append({1200,150,1,AggressorSide::Buy});
    tape->append({1300,151,1,AggressorSide::Sell});
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,{});
    ASSERT_EQ(node.usedVertexCount(),12);
    const auto* v = static_cast<const float*>(node.geometry()->vertexData());
    for (int side = 0; side < 2; ++side) {
        const auto* vertex = v + side*6*14;
        const auto c = side == 0 ? theme::positive() : theme::negative();
        EXPECT_FLOAT_EQ(vertex[4],c.red()/255.f*.4f);
        EXPECT_FLOAT_EQ(vertex[5],c.green()/255.f*.4f);
        EXPECT_FLOAT_EQ(vertex[6],c.blue()/255.f*.4f);
        EXPECT_FLOAT_EQ(vertex[7],.4f);
        EXPECT_FLOAT_EQ(vertex[9],15/255.f);
        EXPECT_FLOAT_EQ(vertex[10],17/255.f);
        EXPECT_FLOAT_EQ(vertex[11],20/255.f);
        EXPECT_FLOAT_EQ(vertex[12],1);
        const float radius = (vertex[14]-vertex[0])/2;
        EXPECT_NEAR(vertex[13]*radius,1,1e-5);
    }
    const auto builds = node.rebuildCount();
    StyleModel look; look.buy.opacity = .7; look.ring.visible = false;
    node.sync(*tape,mapping(),true,{},look);
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_FLOAT_EQ(v[7],.7f); EXPECT_FLOAT_EQ(v[13],0);
    look.buy.visible = false;
    node.sync(*tape,mapping(),true,{},look);
    EXPECT_EQ(node.usedVertexCount(),6);
}
TEST(TradeBubbleStyle, HugeTradeCapsAtLogicalRadiusAcrossDprAndEdits) {
    auto tape = std::make_unique<Tape>();
    tape->append({1500,150,1e100,AggressorSide::Buy});
    TradeBubbleNode node;
    for (double dpr : {1.,1.5,2.,3.}) {
        auto m = mapping(); m.dpr = dpr;
        node.sync(*tape,m,true,{});
        ASSERT_EQ(node.usedVertexCount(),6);
        const auto* v = static_cast<const float*>(node.geometry()->vertexData());
        EXPECT_NEAR((v[14]-v[0])/2*dpr,18*dpr,1e-5);
        StyleModel s; s.buy.width = 48;
        node.sync(*tape,m,true,{},s);
        EXPECT_NEAR((v[14]-v[0])/2,48,1e-5);
    }
}
TEST(TradeBubbleStyle, BaseSizeFilterIsInclusiveAfterPlacement) {
    auto tape = std::make_unique<Tape>();
    tape->append({1200,150,.37,AggressorSide::Buy});
    tape->append({1300,150,.38,AggressorSide::Buy});
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,{});
    ASSERT_EQ(node.usedVertexCount(),6);
    const auto* v = static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR((v[0]+v[14])/2,mapping().timeToScreenX(1300),1e-5);
    StyleModel s; s.minimumBaseSize = .39;
    node.sync(*tape,mapping(),true,{},s);
    EXPECT_EQ(node.usedVertexCount(),0);
}

TEST(TradeBubbleStyle, YamlDefaultsReachStoreAndReset) {
    QTemporaryDir dir;
    QFile file(dir.filePath("client.yaml"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("bubbles:\n  fill_opacity: 0.25\n  max_radius: 26\n  minimum_btc: 0.5\n  ring_width: 0.7\n  ring_visible: false\n");
    file.close();
    ClientConfig config;
    ASSERT_TRUE(ConfigLoader::loadClientConfig(file.fileName().toStdString(), &config));
    const auto original = GuiConfigStore::instance().clientConfig();
    GuiConfigStore::instance().setClientConfig(config);
    QSettings ini(dir.filePath("style.ini"), QSettings::IniFormat);
    chart::TradeBubbleStyleStore store(ini);
    const auto defaults = store.load("BTC-USD");
    EXPECT_DOUBLE_EQ(defaults.buy.opacity,.25);
    EXPECT_DOUBLE_EQ(defaults.sell.width,26);
    EXPECT_DOUBLE_EQ(defaults.minimumBaseSize,.5);
    EXPECT_DOUBLE_EQ(defaults.ring.width,.7);
    EXPECT_FALSE(defaults.ring.visible);
    store.save("BTC-USD", StyleModel{}, true);
    EXPECT_DOUBLE_EQ(store.load("BTC-USD").buy.opacity,.4);
    store.clear("BTC-USD");
    EXPECT_EQ(store.load("BTC-USD"),defaults);
    GuiConfigStore::instance().setClientConfig(original);
}

TEST(TradeBubbleStyle, FeedLoadsPersistedStyleAndUpdatesPlainFrameOnEdit) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("style.ini"), QSettings::IniFormat);
    auto store = std::make_shared<chart::TradeBubbleStyleStore>(ini);
    chart::TradeBubbleFeed feed(store);
    heatmap::HeatmapChartSettings settings;
    settings.showTrades = true;
    settings.sensitivitySymbol = "BTC-USD";
    settings.palettePreset = "Monochrome";
    feed.setSettings(settings, [] {});
    Trade trade{};
    trade.product_id = "BTC-USD"; trade.price = 150; trade.size = .38;
    trade.side = AggressorSide::Buy;
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1500));
    feed.append(trade);
    feed.publish(mapping());
    const auto frame = feed.tradeBubbleRenderFrame();
    EXPECT_EQ(frame->style, StyleModel{}); // independent of the heatmap palette
    TradeBubbleNode node;
    node.sync(*frame->tape,frame->mapping,frame->enabled,frame->options,frame->style);
    EXPECT_EQ(node.usedVertexCount(),6);
    auto edited = frame->style;
    edited.minimumBaseSize = .39;
    store->save("BTC-USD",edited, true);
    // No ingestion or UGR publication is needed for style changes to reach the sibling.
    EXPECT_EQ(frame->style,edited);
    node.sync(*frame->tape,frame->mapping,frame->enabled,frame->options,frame->style);
    EXPECT_EQ(node.usedVertexCount(),0);
    chart::TradeBubbleFeed reopened(store);
    reopened.setSettings(settings, [] {});
    EXPECT_EQ(reopened.tradeBubbleRenderFrame()->style,edited);
    store->clear("BTC-USD");
    EXPECT_EQ(frame->style,StyleModel{});
}

TEST(TradeBubbleStyleReview, SubMinimumFillsFormOneQualifyingSweep) {
    Layout layout;
    const Sample rows[] = {{1500,150,.20,AggressorSide::Buy,"1"},
                           {1500,160,.20,AggressorSide::Buy,"2"}};
    const auto b = layout.build({rows,{}},mapping(),{});
    ASSERT_EQ(b.size(),1);
    EXPECT_NEAR(b[0].notional,62,1e-10);
    EXPECT_NEAR(b[0].y,mapping().priceToScreenY(155),1e-5);
    const Sample small[] = {{1500,150,.19,AggressorSide::Buy,"1"},
                            {1500,160,.18,AggressorSide::Buy,"2"}};
    EXPECT_TRUE(layout.build({small,{}},mapping(),{}).empty());
}
TEST(TradeBubbleStyleReview, BaseMinimumAppliesToWholeSweepWithoutSplittingIt) {
    Layout layout;
    const Sample rows[] = {{1500,150,.5,AggressorSide::Buy,"1"},
                           {1500,160,.2,AggressorSide::Buy,"2"},
                           {1500,170,.5,AggressorSide::Buy,"3"}};
    auto b = layout.build({rows,{}},mapping(),{});
    ASSERT_EQ(b.size(),1);
    EXPECT_NEAR(b[0].notional,192,1e-10);
    StyleModel s; s.minimumBaseSize = 1.21;
    EXPECT_TRUE(layout.build({rows,{}},mapping(),{},0,s).empty());
}
TEST(TradeBubbleStyleReview, SlotVwapIncludesEveryFillBeforeBaseFiltering) {
    Layout layout;
    const Sample rows[] = {{1200,150,.5,AggressorSide::Buy,"1"},
                           {1300,180,.2,AggressorSide::Buy,"3"}};
    auto m = mapping(); m.appendMs = 1000; m.srcRect.setWidth(1);
    const auto b = layout.build({rows,{}},m,{chart::bubbles::Mode::Slot});
    ASSERT_FALSE(b.empty());
    const auto aggregate = std::find_if(b.begin(),b.end(),[](const Bubble& x) { return std::abs(x.notional-111)<1e-10; });
    ASSERT_NE(aggregate,b.end());
    EXPECT_NEAR(aggregate->y,m.priceToScreenY(111/.7),1e-5);
}
TEST(TradeBubbleStyleReview, OverloadFallbackPreservesAllQualifyingNotional) {
    auto tape = std::make_unique<Tape>();
    double total = 0;
    for (int i=0; i<12000; ++i) {
        const double price = 101 + (i%97), size = 1 + (i%3);
        ASSERT_TRUE(tape->append({1000+i/20,price,size,i%2 ? AggressorSide::Buy : AggressorSide::Sell}));
        total += price*size;
    }
    Layout layout;
    auto m = mapping(1847,901);
    const auto b = layout.build(tape->samples(),m,{});
    ASSERT_LE(b.size(),Layout::MaxBubbles);
    double drawn = 0;
    for (const auto& bubble : b) drawn += bubble.notional;
    EXPECT_NEAR(drawn,total,1e-6);
    EXPECT_GE(layout.binSizePx(),6);
}
TEST(TradeBubbleStyleReview, SamePriceMarksUsePassiveOpacityFactor) {
    auto tape = std::make_unique<Tape>();
    tape->append({1200,150,1,AggressorSide::Buy});
    tape->append({1300,150,1,AggressorSide::Buy});
    TradeBubbleNode node;
    StyleModel s; s.buy.opacity = .8;
    node.sync(*tape,mapping(),true,{},s);
    ASSERT_EQ(node.usedVertexCount(),12);
    const auto* vertices = static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(vertices[7],.8*.35,1e-7);
    EXPECT_NEAR(vertices[6*14+7],.8*.35,1e-7);
    EXPECT_NEAR(vertices[12],.35,1e-7);
}
TEST(TradeBubbleStyleReview, AutomaticMinimumIsRoundedStableAndKeepsOffscreenAppendCached) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("style.ini"),QSettings::IniFormat);
    auto store = std::make_shared<chart::TradeBubbleStyleStore>(ini);
    chart::TradeBubbleStyleStore::notePrice("BTC-USD",80000);
    chart::TradeBubbleFeed feed(store);
    heatmap::HeatmapChartSettings settings; settings.showTrades = true;
    settings.sensitivitySymbol = "STABILITY-USD";
    feed.setSettings(settings,[] {});
    Trade trade{std::chrono::system_clock::time_point(std::chrono::milliseconds(1500)),
                "STABILITY-USD","1",AggressorSide::Buy,150,1000};
    feed.append(trade); feed.publish(mapping());
    const auto frame = feed.tradeBubbleRenderFrame();
    EXPECT_DOUBLE_EQ(frame->style.minimumBaseSize,200); // 202.666... -> two significant figures
    TradeBubbleNode node;
    node.sync(*frame->tape,frame->mapping,true,frame->options,frame->style);
    const auto builds = node.rebuildCount();
    for (int i=0; i<4; ++i) {
        trade.timestamp += std::chrono::milliseconds(2000);
        trade.trade_id = std::to_string(i+2); trade.price = 150.5+i;
        feed.append(trade); feed.publish(mapping());
        EXPECT_DOUBLE_EQ(frame->style.minimumBaseSize,200);
        node.sync(*frame->tape,frame->mapping,true,frame->options,frame->style);
        EXPECT_EQ(node.rebuildCount(),builds);
    }
    trade.timestamp += std::chrono::milliseconds(2000);
    trade.trade_id = "large"; trade.price = 180;
    feed.append(trade);
    EXPECT_DOUBLE_EQ(frame->style.minimumBaseSize,170);
}
TEST(TradeBubbleStyleReview, AppearanceEditDoesNotPinAutomaticMinimum) {
    QTemporaryDir dir;
    QSettings ini(dir.filePath("style.ini"),QSettings::IniFormat);
    chart::TradeBubbleStyleStore store(ini);
    chart::TradeBubbleStyleStore::notePrice("BTC-USD",80000);
    chart::TradeBubbleStyleStore::notePrice("UNPINNED-USD",4000);
    auto style = store.load("UNPINNED-USD");
    style.buy.opacity = .6;
    store.save("UNPINNED-USD",style);
    EXPECT_FALSE(store.hasMinimum("UNPINNED-USD"));
    chart::TradeBubbleStyleStore::notePrice("UNPINNED-USD",5000);
    EXPECT_LT(store.load("UNPINNED-USD").minimumBaseSize,style.minimumBaseSize);
}
