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
        const auto* unused = static_cast<const float*>(node.geometry()->vertexData()) + node.usedVertexCount() * 9;
        for (int i = 0; i < 6 * 9; ++i) EXPECT_EQ(unused[i], 0);
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
    const size_t vertices = Layout::MaxBubbles * 6 * 9 * sizeof(float);
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
TEST(TradeBubbles, OverflowStartsWithFineBinsAndPreservesCentroids) {
    auto tape = std::make_unique<Tape>();
    auto m = mapping(1847,901);
    for (int i = 0; i < 5000; ++i)
        ASSERT_TRUE(tape->append({1500,m.screenYToPrice(22+(i%2)*7),1,AggressorSide::Buy}));
    Layout layout;
    const chart::bubbles::Options opts{chart::bubbles::Mode::Individual,{0,150},0};
    auto b = layout.build(tape->samples(),m,opts);
    ASSERT_EQ(b.size(),2);
    EXPECT_GE(layout.binSizePx(),6);
    EXPECT_LE(layout.binSizePx(),8);
    EXPECT_NEAR(std::min(b[0].y,b[1].y),22,1e-4);
    EXPECT_NEAR(std::max(b[0].y,b[1].y),29,1e-4);
    tape->clear();
    for (int i=0; i<5000; ++i)
        tape->append({1500+i%2,150,i%2 ? 3.0 : 1.0,AggressorSide::Buy});
    b = layout.build(tape->samples(),mapping(6,6),opts);
    ASSERT_EQ(b.size(),1);
    EXPECT_NEAR(b[0].x,13.0045,1e-5);
    EXPECT_DOUBLE_EQ(b[0].notional,1500000);
}
TEST(TradeBubbles, SpatialOverflowCoarsensAndPreservesNotional) {
    auto tape = std::make_unique<Tape>();
    const auto m = mapping(1847,901);
    for (int x=0; x<128; ++x) for (int y=0; y<64; ++y)
        tape->append({1000+x*7+3,199.0-y*1.5,0.001,AggressorSide::Buy});
    Layout layout;
    const auto b = layout.build(tape->samples(),m,{chart::bubbles::Mode::Individual,{0,1},0});
    EXPECT_LE(b.size(),Layout::MaxBubbles);
    EXPECT_GT(b.size(),1000);
    EXPECT_GT(layout.binSizePx(),8);
    double expected=0, actual=0;
    const auto rows=tape->samples();
    for (size_t i=0;i<rows.size();++i) expected+=rows[i].price*rows[i].size;
    for (const auto& circle:b) actual+=circle.notional;
    EXPECT_NEAR(actual,expected,0.001);
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
    EXPECT_GT(layout.binSizePx(),0);
    EXPECT_EQ(layout.scannedRows(),100000);
}
TEST(TradeBubbles, OverflowWorkIsBoundedAndUsesFirstFittingGrid) {
    auto m = mapping();
    m.viewEndMs = 1001000; m.srcRect.setWidth(10000);
    for (const auto rect : {QRectF(0,0,1847,901), QRectF(0,0,12000,6000)}) {
        m.drawRect = rect;
        std::vector<Sample> rows;
        uint32_t seed = 1234567;
        auto random = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        for (size_t i = 0; i < 100000; ++i)
            rows.push_back({1000+int64_t(random()%1000000),100.+(random()%1000000)/10000.,.01,
                            i%2 ? AggressorSide::Buy : AggressorSide::Sell});
        std::stable_sort(rows.begin(), rows.end(), [](const Sample& a, const Sample& b) { return a.timeMs < b.timeMs; });
        Layout layout;
        const chart::bubbles::Options options{chart::bubbles::Mode::Individual,{0,2},.0001};
        std::span<const Bubble> bubbles;
        std::array<double,9> timings{};
        for (int trial = 0; trial < (rect.width() < 2000 ? 11 : 1); ++trial) {
            const auto start = std::chrono::steady_clock::now();
            bubbles = layout.build({rows,{}},m,options);
            const auto end = std::chrono::steady_clock::now();
            if (trial >= 2) timings[size_t(trial-2)] = std::chrono::duration<double,std::milli>(end-start).count();
        }
        if (rect.width() < 2000) {
            std::sort(timings.begin(),timings.end());
            std::cout << "BUBBLE_LAYOUT rows=" << rows.size() << " median_ms=" << timings[4]
                      << " max_ms=" << timings.back() << " probes=" << layout.hashProbes()
                      << " cell_visits=" << layout.coarseCellVisits() << " bin_px=" << layout.binSizePx()
                      << " bubbles=" << bubbles.size() << '\n';
        }
        EXPECT_LE(layout.hashProbes(),32*rows.size());
        EXPECT_LE(layout.coarseCellVisits(),24*rows.size());
        EXPECT_EQ(layout.scannedRows(),rows.size());
        for (int width = 6; width <= int(layout.binSizePx()); width += 6) {
            std::unordered_set<uint64_t> occupied;
            for (const auto& row : rows) {
                const auto x = uint32_t((m.timeToScreenX(row.timeMs)-m.drawRect.x())/width);
                const auto y = uint32_t((m.priceToScreenY(row.price)-m.drawRect.y())/width);
                occupied.insert((uint64_t(x)<<33)|(uint64_t(y)<<1)|(row.side==AggressorSide::Sell));
            }
            if (width < int(layout.binSizePx())) EXPECT_GT(occupied.size(),Layout::MaxBubbles);
            else EXPECT_EQ(occupied.size(),bubbles.size());
        }
    }
}
TEST(TradeBubbles, PredecessorChangesInvalidatePassiveOpacity) {
    auto tape = std::make_unique<Tape>();
    tape->append({999,150,1,AggressorSide::Buy});
    tape->append({1500,150,1,AggressorSide::Buy});
    TradeBubbleNode node;
    chart::bubbles::Options options{chart::bubbles::Mode::Individual,{0,150},1};
    node.sync(*tape,mapping(),true,options,Qt::cyan,Qt::yellow);
    const auto* vertices = static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(vertices[7],.21f,1e-6);
    const auto builds = node.rebuildCount();
    tape->append({999,149,1,AggressorSide::Sell});
    node.sync(*tape,mapping(),true,options,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_FLOAT_EQ(vertices[7],.6f);
}
TEST(TradeBubbles, RebuildsInBothModesDoNotAllocateAndKeepQsgStorage) {
    auto tape = std::make_unique<Tape>();
    for (int i = 0; i < 100000; ++i)
        tape->append({1000+i/100,100.+(i%100),1,AggressorSide::Buy});
    TradeBubbleNode node;
    auto m = mapping(1847,901);
    chart::bubbles::Options options{chart::bubbles::Mode::Individual,{150,195},1};
    node.sync(*tape,m,true,options,Qt::cyan,Qt::yellow);
    auto* storage = node.geometry()->vertexData();
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 6; ++i) {
        options.mode = i % 2 ? chart::bubbles::Mode::Slot : chart::bubbles::Mode::Individual;
        m.drawRect.setWidth(1848+i);
        node.sync(*tape,m,true,options,Qt::cyan,Qt::yellow);
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
    node.sync(*tape,m,true,options,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.lastScanRows(), 2);
    EXPECT_EQ(node.usedVertexCount(), 18);
    const auto builds = node.rebuildCount();
    // Keep the time scale fixed, but change bar alignment/centre.
    m.appendMs = 2000; m.srcRect = {.25,0,.1,100};
    node.sync(*tape,m,true,options,Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(), builds + 1);
}
TEST(TradeBubbles, CapsulesKeepMinimumWidthAndPassiveFlowUsesReducedOpacity) {
    auto tape = std::make_unique<Tape>();
    tape->append({1500,150,1,AggressorSide::Buy,"1"});
    tape->append({1500,181.25,1,AggressorSide::Buy,"2"});
    tape->append({1600,181.25,1,AggressorSide::Buy,"3"});
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,{chart::bubbles::Mode::Individual,{0,100000},1},Qt::cyan,Qt::yellow);
    ASSERT_EQ(node.usedVertexCount(),12);
    const auto* v = static_cast<const float*>(node.geometry()->vertexData());
    const double radius = (v[9]-v[0])/2;
    constexpr double pi = 3.14159265358979323846;
    EXPECT_GE(radius,1.5);
    EXPECT_GT(pi*radius*radius+2*100*radius,pi*4);
    EXPECT_FLOAT_EQ(v[7],.6f);
    EXPECT_NEAR(v[6*9+7],.21f,1e-6);
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
    b=layout.build(tape->samples(),mapping(),options);
    ASSERT_EQ(b.size(),2);
    EXPECT_EQ(b[0].side,AggressorSide::Sell);
    EXPECT_GT(b[0].radius,b[1].radius);
    TradeBubbleNode node;
    node.sync(*tape,mapping(),true,options,Qt::cyan,Qt::yellow);
    const auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_FLOAT_EQ(v[4],0.21f);
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
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    ASSERT_EQ(node.lastScanRows(),3);
    const auto builds=node.rebuildCount();
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(),builds);
    EXPECT_EQ(node.lastScanRows(),0);
    const auto* vertices=static_cast<const float*>(node.geometry()->vertexData());
    const float x=vertices[0];
    for (int i=0;i<5;++i) {
        m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
        tape->append({100100+i,150,1,AggressorSide::Buy}); // Outside view; ring eviction also outside.
        node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
        EXPECT_EQ(node.rebuildCount(),builds);
        EXPECT_EQ(node.lastScanRows(),0);
        EXPECT_LE(node.rangeComparisons(),34);
        EXPECT_FLOAT_EQ(vertices[0],x);
        EXPECT_NEAR(node.translation()(0,3),-(i+1)*.1*640/3,0.001);
    }
    // Late insertion inside the view must invalidate the cached mesh.
    ASSERT_TRUE(tape->append({1501,151,1,AggressorSide::Sell}));
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.rebuildCount(),builds+1);
    EXPECT_EQ(node.lastScanRows(),4);
    EXPECT_EQ(node.usedVertexCount(),24);
    // Moving across a boundary drops a row and forces another rebuild.
    m.viewStartMs=1500.1; m.viewEndMs=1503.1; m.srcRect.translate(.005,0);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
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
        node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
        ASSERT_EQ(node.geometry()->vertexData(),vertices);
        ASSERT_EQ(node.usedVertexCount(),6);
        expectDrawCount(node);
    }
    // Real quad corners surround the centre at the capped radius.
    auto* v=static_cast<const float*>(node.geometry()->vertexData());
    EXPECT_NEAR(v[0],m.timeToScreenX(1500)-18,1e-4);
    EXPECT_NEAR(v[1],m.priceToScreenY(150)-18,1e-4);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.usedVertexCount(),0);
    expectDrawCount(node);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.usedVertexCount(),6);
    tape->clear();
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
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
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow); // off, empty: nothing to draw
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);  // on, empty: still nothing
    EXPECT_EQ(node.dirtyMarks(),0u);
    ASSERT_TRUE(tape->append({1500,150,1,AggressorSide::Buy}));
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow); // identical frame
    m.viewStartMs+=.1; m.viewEndMs+=.1; m.srcRect.translate(.001,0);
    node.sync(*tape,m,true,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow); // follow-live pan: translation only
    EXPECT_EQ(node.dirtyMarks(),1u);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow); // off: the drawn mesh is removed
    EXPECT_EQ(node.dirtyMarks(),2u);
    node.sync(*tape,m,false,{chart::bubbles::Mode::Individual,{0,150},1},Qt::cyan,Qt::yellow);
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
    f.buy = Qt::green; f.sell = Qt::red;
    TradeBubbleNode node; link.node = &node;
    chart::CvdPresentation pane; link.presentation = &pane;
    pane.dataTimeframeMs = 1000; pane.hasData = true;
    link.sync();
    ASSERT_GT(node.usedVertexCount(), 0);
    const QByteArray before(static_cast<const char*>(node.geometry()->vertexData()), node.usedVertexCount() * 9 * sizeof(float));
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
            covered |= vertices[i * 9 + 1] > node.clipRect().bottom();
        }
        EXPECT_TRUE(covered) << "the fixture must exercise the excluded part of the bubble";
    }
    pane.flowFooter = false; pane.stripPx = 0; link.sync();
    EXPECT_EQ(node.clipRect(), f.mapping.drawRect);
    EXPECT_EQ(std::memcmp(before.constData(), node.geometry()->vertexData(), size_t(before.size())), 0);
}
