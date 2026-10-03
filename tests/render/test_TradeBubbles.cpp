#include <gtest/gtest.h>
#include "render/TradeBubbleNode.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include <QTemporaryDir>
#include <QSettings>
#include <cmath>
#include <memory>

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
TEST(TradeBubbles, ZoomedOutAggregationIsBoundedAndPreservesSideAndCentroid) {
    auto tape = std::make_unique<Tape>();
    // More trades than display capacity, filling all bins (both sides).
    for (int pass=0; pass<5; ++pass)
        for (int y=0; y<32; ++y) for (int x=0; x<128; ++x) for (int side=0; side<2; ++side)
            ASSERT_TRUE(tape->append({1000+x*7+3, 199.0-y*3, 1,
                                     side ? AggressorSide::Sell : AggressorSide::Buy}));
    Layout layout;
    auto b=layout.build(tape->samples(),mapping(),0);
    EXPECT_LE(b.size(),4096u);
    EXPECT_GT(b.size(),1000);
    auto small=layout.build(tape->samples(),mapping(6,6),0);
    ASSERT_EQ(small.size(),2);
    EXPECT_EQ(small[0].side,AggressorSide::Buy);
    EXPECT_EQ(small[1].side,AggressorSide::Sell);
    EXPECT_FLOAT_EQ(small[0].x,small[1].x);
    EXPECT_FLOAT_EQ(small[0].y,small[1].y);
    const Sample pair[]={{1500,150,1,AggressorSide::Buy},{1501,150,3,AggressorSide::Buy}};
    const auto centroid=layout.build(pair,mapping(6,6),0);
    ASSERT_EQ(centroid.size(),1);
    EXPECT_NEAR(centroid[0].x,13.0045,1e-5);
    EXPECT_NEAR(centroid[0].radius,Layout::radius(600),1e-6);
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
    const auto* storage=tape->samples().data();
    for (size_t i=0;i<Tape::Capacity+10;++i) tape->append({1500,150,1,AggressorSide::Buy});
    ASSERT_EQ(tape->samples().size(),Tape::Capacity);
    EXPECT_EQ(tape->samples().data(),storage);
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
