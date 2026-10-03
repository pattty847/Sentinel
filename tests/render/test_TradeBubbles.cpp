#include <gtest/gtest.h>
#include "render/TradeBubbleNode.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include <QTemporaryDir>
#include <QSettings>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <iostream>

using namespace trade_bubbles;
namespace {
void expectDrawCount(const TradeBubbleNode& node) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    EXPECT_EQ(node.geometry()->vertexCount(), node.usedVertexCount());
#else
    EXPECT_EQ(node.geometry()->vertexCount(), int(Layout::MaxBubbles * 6));
    if (node.usedVertexCount() < node.geometry()->vertexCount()) {
        const auto* unused = static_cast<const float*>(node.geometry()->vertexData()) + node.usedVertexCount() * 8;
        for (int i = 0; i < 6 * 8; ++i) EXPECT_EQ(unused[i], 0);
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
TEST(TradeBubbles, PlacementUsesExchangeTimeAndPriceWithoutRingOffset) {
    Layout layout;
    auto m = mapping(); m.timeOffset = .75f;
    const Sample t[] = {{1250,175,2,AggressorSide::Buy}};
    const auto b = layout.build(t,m,0);
    ASSERT_EQ(b.size(),1);
    EXPECT_FLOAT_EQ(b[0].x,170);
    EXPECT_FLOAT_EQ(b[0].y,100);
    m.viewStartMs += 100; m.viewEndMs += 100; m.srcRect.translate(1,0);
    const auto p = layout.build(t,m,0);
    ASSERT_EQ(p.size(),1);
    EXPECT_FLOAT_EQ(p[0].x,106);
    EXPECT_FLOAT_EQ(p[0].y,100);
}
TEST(TradeBubbles, AreaScalesWithNotionalAndRadiusCaps) {
    EXPECT_DOUBLE_EQ(Layout::radius(1000),3);
    EXPECT_DOUBLE_EQ(Layout::radius(4000),6);
    EXPECT_DOUBLE_EQ(Layout::radius(1e10),18);
    Layout layout;
    const Sample t[] = {{1250,150,1000000,AggressorSide::Sell}};
    auto b = layout.build(t,mapping(),0);
    ASSERT_EQ(b.size(),1);
    EXPECT_FLOAT_EQ(b[0].radius,18);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
}
TEST(TradeBubbles, FilterAppliesPerTradeBeforeAggregationAndIncludesThreshold) {
    Layout layout;
    const Sample t[] = {{1500,150,2,AggressorSide::Buy}, {1500,150,2,AggressorSide::Buy},
                        {1500,150,4,AggressorSide::Sell}};
    auto b = layout.build(t,mapping(),600);
    ASSERT_EQ(b.size(),1); // two 300-unit buys cannot pass a 600 filter together
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_NEAR(b[0].radius,3*std::sqrt(.6),1e-6);
    EXPECT_TRUE(layout.build(t,mapping(),601).empty());
}
TEST(TradeBubbles, IndividualExecutionsThroughCapNeverMerge) {
    auto tape = std::make_unique<Tape>();
    for (size_t i = 0; i < Layout::MaxBubbles; ++i)
        ASSERT_TRUE(tape->append({1500,150,1,AggressorSide::Buy}));
    Layout layout;
    const auto b = layout.build(tape->samples(),mapping(1847,901),0);
    ASSERT_EQ(b.size(), Layout::MaxBubbles);
    EXPECT_EQ(layout.binSizePx(), 0);
    for (const auto& circle : b) EXPECT_NEAR(circle.radius, Layout::radius(150), 1e-6);
}
TEST(TradeBubbles, OverflowStartsWithFineBinsAndPreservesCentroids) {
    auto tape = std::make_unique<Tape>();
    // Five thousand executions at two prices only 7 px apart: the old 64x32
    // grid collapsed them into a false intermediate execution price.
    auto m = mapping(1847,901);
    for (int i = 0; i < 5000; ++i)
        ASSERT_TRUE(tape->append({1500,m.screenYToPrice(22 + (i%2)*7),1,AggressorSide::Buy}));
    Layout layout;
    auto b = layout.build(tape->samples(),m,0);
    ASSERT_EQ(b.size(),2);
    EXPECT_GE(layout.binSizePx(),6);
    EXPECT_LE(layout.binSizePx(),8);
    EXPECT_NEAR(std::min(b[0].y,b[1].y),22,1e-4);
    EXPECT_NEAR(std::max(b[0].y,b[1].y),29,1e-4);
    // Actual same-cell aggregation keeps the notional-weighted centroid.
    tape->clear();
    for (int i=0; i<5000; ++i)
        tape->append({1500+i%2,150,i%2 ? 3.0 : 1.0,AggressorSide::Buy});
    b = layout.build(tape->samples(),mapping(6,6),0);
    ASSERT_EQ(b.size(),1);
    EXPECT_NEAR(b[0].x,13.0045,1e-5);
    EXPECT_EQ(b[0].radius,18);
}
TEST(TradeBubbles, SpatialOverflowCoarsensOnlyAsNeededAndKeepsAllVolume) {
    auto tape = std::make_unique<Tape>();
    const auto m = mapping(1847,901);
    for (int x=0; x<128; ++x) for (int y=0; y<64; ++y)
        tape->append({1000+x*7+3,199.0-y*1.5,0.001,AggressorSide::Buy});
    Layout layout;
    const auto b = layout.build(tape->samples(),m,0);
    EXPECT_LE(b.size(),Layout::MaxBubbles);
    EXPECT_GT(b.size(),1000);
    EXPECT_GT(layout.binSizePx(),8); // Explicit, necessary fallback for >4096 occupied fine cells.
    double expected=0, actual=0;
    const auto rows=tape->samples();
    for (size_t i=0;i<rows.size();++i) expected+=rows[i].price*rows[i].size;
    for (const auto& circle:b) actual+=circle.radius*circle.radius*1000/9;
    EXPECT_NEAR(actual,expected,0.001);
}
TEST(TradeBubbles, OverflowHashAndSizingHaveBoundedWorkAndChooseFirstFittingGrid) {
    auto m=mapping(1847,901); m.viewEndMs=1001000; m.srcRect.setWidth(10000);
    for (const auto rect : {QRectF(0,0,1847,901), QRectF(0,0,12000,6000)})
    for (size_t n : {5000u,20000u,100000u}) {
        m.drawRect=rect;
        std::vector<Sample> rows; rows.reserve(n);
        uint32_t seed=1234567;
        auto random=[&] { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
        for (size_t i=0;i<n;++i)
            rows.push_back({1000+int64_t(random()%1000000),100.0+(random()%1000000)/10000.0,.01,
                            i%2 ? AggressorSide::Buy : AggressorSide::Sell});
        Layout layout;
        const bool normalPlot=rect.width()<2000;
        std::span<const Bubble> bubbles;
        std::array<double,9> timings{};
        for (int trial=0; trial<(normalPlot ? 11 : 1); ++trial) {
            const auto start=std::chrono::steady_clock::now();
            bubbles=layout.build(std::span<const Sample>(rows),m,0);
            const auto end=std::chrono::steady_clock::now();
            if (trial>=2) timings[size_t(trial-2)]=std::chrono::duration<double,std::milli>(end-start).count();
        }
        if (normalPlot) {
            std::sort(timings.begin(),timings.end());
            std::cout << "BUBBLE_LAYOUT rows=" << n << " median_ms=" << timings[4]
                      << " max_ms=" << timings.back() << " probes=" << layout.hashProbes()
                      << " cell_visits=" << layout.coarseCellVisits() << " bin_px=" << layout.binSizePx()
                      << " bubbles=" << bubbles.size() << '\n';
        }
        ASSERT_LE(bubbles.size(),Layout::MaxBubbles);
        ASSERT_LE(layout.hashProbes(),(normalPlot ? 8 : 32)*n) << "integer grid keys must not collapse to one probe run";
        EXPECT_LE(layout.coarseCellVisits(),(normalPlot ? 3 : 24)*n) << "size from occupied fine cells, not repeated trade scans";
        EXPECT_EQ(layout.scannedRows(),n);
        EXPECT_LE(layout.fineCellCount(),n);
        EXPECT_EQ(std::fmod(layout.binSizePx(),6),0);
        // Independent oracle over raw executions: every smaller supported bin
        // width must exceed the cap; the selected width must fit exactly.
        for (int width=6; width<=int(layout.binSizePx()); width+=6) {
            std::unordered_set<uint64_t> occupied;
            for (const auto& row:rows) {
                const auto x=uint32_t((m.timeToScreenX(row.timeMs)-m.drawRect.x())/width);
                const auto y=uint32_t((m.priceToScreenY(row.price)-m.drawRect.y())/width);
                occupied.insert((uint64_t(x)<<33)|(uint64_t(y)<<1)|(row.side==AggressorSide::Sell));
            }
            if (width<int(layout.binSizePx())) EXPECT_GT(occupied.size(),Layout::MaxBubbles);
            else EXPECT_EQ(occupied.size(),bubbles.size());
        }
        double expected=0,actual=0;
        for(const auto& row:rows) expected+=row.price*row.size;
        for(const auto& bubble:bubbles) actual+=bubble.radius*bubble.radius*1000/9;
        EXPECT_NEAR(actual,expected,0.03); // sizes avoid the radius cap
    }
}

TEST(TradeBubbles, LargerCirclesDrawFirstAcrossSidesAndNeighbouringCells) {
    Layout layout;
    const Sample rows[]={{1500,150,1,AggressorSide::Buy}, {1500,150,4,AggressorSide::Sell},
                         {1501,150,2,AggressorSide::Buy}};
    auto b=layout.build(rows,mapping(),0);
    ASSERT_EQ(b.size(),3);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_GT(b[0].radius,b[1].radius);
    EXPECT_GT(b[1].radius,b[2].radius);
    auto tape=std::make_unique<Tape>();
    for (int i=0;i<5000;++i) { auto row=rows[i%3]; row.size/=1000; tape->append(row); }
    b=layout.build(tape->samples(),mapping(),0);
    ASSERT_EQ(b.size(),2);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_GT(b[0].radius,b[1].radius);
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,0,Qt::cyan,Qt::yellow);
    const auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_FLOAT_EQ(v[4],0.6f); // First mesh is the larger yellow sell.
    EXPECT_FLOAT_EQ(v[6],0);
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
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
    ASSERT_EQ(node.lastScanRows(),3);
    const auto builds=node.rebuildCount();
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(),builds);
    EXPECT_EQ(node.lastScanRows(),0);
    const auto* vertices=static_cast<const float*>(node.geometry()->vertexData());
    const float x=vertices[0];
    for (int i=0;i<5;++i) {
        m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
        tape->append({100100+i,150,1,AggressorSide::Buy}); // Outside view; ring eviction also outside.
        node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
        EXPECT_EQ(node.rebuildCount(),builds);
        EXPECT_EQ(node.lastScanRows(),0);
        EXPECT_LE(node.rangeComparisons(),34);
        EXPECT_FLOAT_EQ(vertices[0],x);
        EXPECT_NEAR(node.translation()(0,3),-(i+1)*.1*640/3,0.001);
    }
    // Late insertion inside the view must invalidate the cached mesh.
    ASSERT_TRUE(tape->append({1501,151,1,AggressorSide::Sell}));
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_EQ(node.lastScanRows(),4);
    EXPECT_EQ(node.usedVertexCount(),24);
    // Moving across a boundary drops a row and forces another rebuild.
    m.viewStartMs=1500.1; m.viewEndMs=1503.1; m.srcRect.translate(.005,0);
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
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
    EXPECT_TRUE(layout.build(rows,mapping(),0).empty());
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
        node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
        ASSERT_EQ(node.geometry()->vertexData(),vertices);
        ASSERT_EQ(node.usedVertexCount(),6);
        expectDrawCount(node);
    }
    // Real quad corners surround the centre at the capped radius.
    auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(v[0],m.timeToScreenX(1500)-18,1e-4);
    EXPECT_NEAR(v[1],m.priceToScreenY(150)-18,1e-4);
    node.sync(*tape,m,false,0,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.usedVertexCount(),0);
    expectDrawCount(node);
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.usedVertexCount(),6);
    tape->clear();
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
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
    node.sync(*tape,m,false,0,Qt::cyan,Qt::yellow); // off, empty: nothing to draw
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);  // on, empty: still nothing
    EXPECT_EQ(node.dirtyMarks(),0u);
    ASSERT_TRUE(tape->append({1500,150,1,AggressorSide::Buy}));
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow); // identical frame
    m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
    node.sync(*tape,m,true,0,Qt::cyan,Qt::yellow); // follow-live pan: translation only
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,false,0,Qt::cyan,Qt::yellow); // off: the drawn mesh is removed
    EXPECT_EQ(node.dirtyMarks(),2u);
    node.sync(*tape,m,false,0,Qt::cyan,Qt::yellow);
    node.clear();                                     // nothing drawn: no mark
    EXPECT_EQ(node.dirtyMarks(),2u);
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
        ASSERT_TRUE(store.applyChartPatch("one",s,{{"showTrades",true},{"tradeMinNotional",1000}},true,{},"BTC-USD",60000).isEmpty());
        ASSERT_TRUE(store.applyChartPatch("one",s,{{"tradeMinNotional",100000}},false,{},"BTC-USD",60000).isEmpty());
        EXPECT_EQ(s.tradeMinNotional,100000);
        ini.sync();
    }
    QSettings ini(path,QSettings::IniFormat);
    heatmap::HeatmapSettingsStore store(ini);
    const auto s=store.load("one",{});
    EXPECT_TRUE(s.showTrades); EXPECT_EQ(s.tradeMinNotional,1000);
    EXPECT_FALSE(store.load("two",{}).showTrades);
    EXPECT_EQ(heatmap::settingsJson(s)["tradeMinNotional"].toDouble(),1000);
    auto invalid=s;
    EXPECT_FALSE(heatmap::applySettingsPatch(invalid,{{"showTrades","yes"}}).isEmpty());
    EXPECT_EQ(invalid,s);
}
