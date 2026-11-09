#include "chart/core/BubblePlacement.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <new>
#include <vector>

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
using namespace chart::bubbles;

TEST(BubblePlacement, AutoUsesFortyPixelBoundaryWithHysteresisAndShortTimeframeOverride) {
    PlacementMode mode;
    EXPECT_EQ(mode.select(Mode::Auto, 39, 60000), Mode::Slot);
    for (double px : {40., 41., 39., 41.}) EXPECT_EQ(mode.select(Mode::Auto, px, 60000), Mode::Slot);
    EXPECT_EQ(mode.select(Mode::Auto, 42, 60000), Mode::Individual);
    for (double px : {41., 40., 39.}) EXPECT_EQ(mode.select(Mode::Auto, px, 60000), Mode::Individual);
    EXPECT_EQ(mode.select(Mode::Auto, 38, 60000), Mode::Slot);
    EXPECT_EQ(mode.select(Mode::Auto, 1, 1000), Mode::Individual);
    EXPECT_EQ(mode.select(Mode::Auto, 1, 100), Mode::Individual);
    EXPECT_EQ(mode.select(Mode::Slot, 100, 1000), Mode::Slot);
    EXPECT_EQ(mode.select(Mode::Individual, 1, 60000), Mode::Individual);
    EXPECT_EQ(mode.select(Mode::Auto, 40, 60000), Mode::Individual);
}
TEST(BubblePlacement, SlotsHaveSideVwapSummedNotionalAndThreeLargestAtBarCentre) {
    BubblePlacement p;
    const std::array rows{Execution{61000,100,1,0}, Execution{62000,200,3,0},
                          Execution{63000,150,2,1}, Execution{64000,110,.1,0},
                          Execution{121000,120,1,0}};
    const auto marks = p.build(rows, {Mode::Slot, {}, 1}, 60000);
    ASSERT_EQ(marks.size(), 6); // singleton sides have only the aggregate
    EXPECT_TRUE(marks[0].aggregate);
    EXPECT_DOUBLE_EQ(marks[0].timeMs, 90000);
    EXPECT_DOUBLE_EQ(marks[0].notional, 711);
    EXPECT_DOUBLE_EQ(marks[0].price, 711 / 4.1);
    EXPECT_DOUBLE_EQ(marks[1].price, 150);
    EXPECT_DOUBLE_EQ(marks[1].notional, 300);
    EXPECT_EQ(marks[1].side, 1);
    for (size_t i = 2; i < 5; ++i) {
        EXPECT_FALSE(marks[i].aggregate);
        EXPECT_DOUBLE_EQ(marks[i].timeMs, 90000);
    }
    EXPECT_DOUBLE_EQ(marks[2].notional, 600);
    EXPECT_DOUBLE_EQ(marks[3].notional, 100);
    EXPECT_DOUBLE_EQ(marks[4].notional, 11);
    EXPECT_DOUBLE_EQ(marks[5].timeMs, 150000);
}
TEST(BubblePlacement, SweepsRequireExactTimeSideAndConsecutiveNumericIdsAndCountDistinctPrices) {
    BubblePlacement p;
    const std::array rows{Execution{1500,100,1,0,"10",1500000001},
        Execution{1500,102,2,0,"11",1500000001}, Execution{1500,100,1,0,"12",1500000001},
        Execution{1500,100,1,0,"13",1500000002}, Execution{1500,100,1,1,"14",1500000002},
        Execution{1500,100,1,1,"16",1500000002}, Execution{1500,100,1,1,"x17",1500000002}};
    auto marks = p.build(rows, {Mode::Individual, {}, 1}, 60000, 100);
    ASSERT_EQ(marks.size(), 5);
    EXPECT_DOUBLE_EQ(marks[0].minPrice, 100);
    EXPECT_DOUBLE_EQ(marks[0].maxPrice, 102);
    EXPECT_DOUBLE_EQ(marks[0].notional, 404);
    EXPECT_DOUBLE_EQ(marks[0].price, 101);
    EXPECT_EQ(marks[0].sweepLevels, 2);
    EXPECT_TRUE(marks[0].moving);
    EXPECT_FALSE(marks[1].moving);
    marks = p.build(rows, {Mode::Slot, {}, 1}, 60000, 100);
    ASSERT_GE(marks.size(), 3);
    EXPECT_EQ(marks[2].sweepLevels, 2);
    EXPECT_EQ(marks[2].timeMs, 30000);
}
TEST(BubblePlacement, VisibleReferenceAndMedianChangeAndExplicitMinimumOverrides) {
    ReferenceStatistics stats;
    BubblePlacement p;
    const std::array rows{Execution{1000,100,1,0}, Execution{1100,100,2,0}, Execution{1200,100,10,0}};
    auto ref = stats.reference(rows);
    EXPECT_DOUBLE_EQ(ref.median, 200);
    EXPECT_DOUBLE_EQ(ref.p95, 1000);
    EXPECT_EQ(p.build(rows, {Mode::Individual, ref, -1}, 60000).size(), 3);
    EXPECT_EQ(p.build(rows, {Mode::Individual, ref, 0}, 60000).size(), 2);
    EXPECT_EQ(p.build(rows, {Mode::Individual, ref, 100}, 60000).size(), 3);
    EXPECT_EQ(p.build(rows, {Mode::Individual, ref, 201}, 60000).size(), 1);
    auto moved = stats.reference(std::span(rows).first(2));
    EXPECT_DOUBLE_EQ(moved.median, 100);
    EXPECT_DOUBLE_EQ(moved.p95, 200);
    EXPECT_GT(radius(200, moved.p95), radius(200, ref.p95));
    double previous = 0;
    for (double n : {1., 10., 100., 200., 500., 1000., 10000., 1e12}) {
        const double r = radius(n, ref.p95);
        EXPECT_GE(r, previous); EXPECT_GE(r, 2); EXPECT_LE(r, 18); previous = r;
    }
    EXPECT_DOUBLE_EQ(radius(1e12, 1000), 18);
    EXPECT_DOUBLE_EQ(radius(1000, 1000), 9);
}
TEST(BubblePlacement, SweepIsFilteredAfterMergeAndSlotsIncludeAllSideNotional) {
    BubblePlacement p;
    const std::array rows{Execution{1000,100,1,0,"1"}, Execution{1000,101,1,0,"2"}};
    EXPECT_EQ(p.build(rows, {Mode::Individual, {}, 200}, 60000).size(), 1);
    const auto marks = p.build(rows, {Mode::Slot, {}, 200}, 60000);
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks[0].notional, 201);
}
TEST(BubblePlacement, SlotAggregatesUseTheirOwnP95AndNotablesUseTradeP95) {
    ReferenceStatistics statistics;
    BubblePlacement placement;
    std::vector<Execution> rows;
    rows.reserve(120);
    for (int bar = 0; bar < 40; ++bar) {
        for (int trade = 0; trade < 3; ++trade)
            rows.push_back({1000. + 60000. * bar + trade, 100,
                            (bar % 10 == 0 ? 10. : 1.) * (1 + bar%4), 0});
    }
    const auto ref = statistics.update(rows, rows, {1,120,1,120,120,120},
                                       {1000,2401000,60000}, Mode::Slot, 1000);
    EXPECT_GT(ref.aggregateP95,ref.p95);
    auto marks = placement.build(rows,{Mode::Slot,ref,-1},60000);
    size_t aggregate = 0, capped = 0, uncapped = 0;
    double smallest = 18, largest = 0;
    for (const auto& mark : marks) if (mark.aggregate) {
        ++aggregate;
        smallest = std::min(smallest, radius(mark.notional,ref.aggregateP95));
        largest = std::max(largest, radius(mark.notional,ref.aggregateP95));
        if (radius(mark.notional,ref.aggregateP95) >= 18) ++capped;
        else ++uncapped;
    }
    EXPECT_EQ(aggregate,40);
    EXPECT_GT(uncapped,0);
    EXPECT_LT(capped,aggregate);
    EXPECT_GT(largest-smallest,4);
}
TEST(BubblePlacement, ReferencesThrottleBurstsAndHoldOneStepChanges) {
    ReferenceStatistics stats;
    std::vector<Execution> rows;
    rows.reserve(1200);
    for (int i=0; i<100; ++i) rows.push_back({1000.+i,100,1,0});
    const VisibleView view{1000,5000,60000};
    auto active = stats.update(rows,rows,{1,100,1,100,100,100},view,Mode::Individual,1000);
    const double initialRadius = radius(100,active.p95);
    const size_t initialVisible = BubblePlacement().build(rows,{Mode::Individual,active,0},60000).size();
    for (int i=0; i<1000; ++i) {
        rows.push_back({1100.+i,100,1,0});
        active = stats.update(rows,rows,{1,uint64_t(101+i),1,uint64_t(101+i),rows.size(),rows.size()},
                              view,Mode::Individual,1001+i/8);
    }
    EXPECT_EQ(stats.updates(),1);
    EXPECT_EQ(radius(100,active.p95),initialRadius);
    rows.pop_back();
    active = stats.update(rows,rows,{1,1099,1,1099,rows.size(),rows.size()},view,Mode::Individual,1250);
    EXPECT_EQ(stats.updates(),2);
    EXPECT_EQ(radius(100,active.p95),initialRadius);
    EXPECT_EQ(BubblePlacement().build(rows,{Mode::Individual,active,0},60000).size(),rows.size());
    EXPECT_EQ(initialVisible,100);
    for (auto& row : rows) row.size *= 10;
    active = stats.update(rows,rows,{2,1100,2,1100,rows.size(),rows.size()},view,Mode::Individual,1500);
    EXPECT_GT(active.p95,500);
    EXPECT_EQ(stats.updates(),3);
}
TEST(BubblePlacement, OneRowEnteringAndLeavingDoesNotRescaleOrHide) {
    ReferenceStatistics stats;
    std::vector<Execution> rows(100);
    for (int i=0;i<100;++i) rows[size_t(i)] = {1000.+i,100,1,0};
    const VisibleView view{1000,2000,60000};
    auto initial = stats.update(rows,rows,{1,100,1,100,100,100},view,Mode::Individual,1000);
    const double radiusBefore = radius(100,initial.p95);
    BubblePlacement placement;
    EXPECT_EQ(placement.build(rows,{Mode::Individual,initial,0},60000).size(),100);
    rows.push_back({1100,109,1,0});
    auto entered = stats.update(rows,rows,{1,101,1,101,101,101},view,Mode::Individual,1250);
    EXPECT_EQ(radius(100,entered.p95),radiusBefore);
    EXPECT_EQ(placement.build(rows,{Mode::Individual,entered,0},60000).size(),101);
    rows.pop_back();
    auto left = stats.update(rows,rows,{1,100,1,100,100,100},view,Mode::Individual,1500);
    EXPECT_EQ(radius(100,left.p95),radiusBefore);
    EXPECT_EQ(placement.build(rows,{Mode::Individual,left,0},60000).size(),100);
}
TEST(BubblePlacement, CapsuleRetainsAtLeastMinimumHalfWidth) {
    EXPECT_GE(capsuleHalfWidth(2,100),1.5);
    EXPECT_DOUBLE_EQ(capsuleHalfWidth(9,0),9);
}
TEST(BubblePlacement, FirstExecutionWithoutPredecessorIsPassive) {
    BubblePlacement p;
    const std::array rows{Execution{1500,100,1,0}, Execution{1501,101,1,0}};
    const auto marks = p.build(rows,{Mode::Individual,{},-1},60000);
    ASSERT_EQ(marks.size(),2);
    EXPECT_FALSE(marks[0].moving);
    EXPECT_TRUE(marks[1].moving);
}
TEST(BubblePlacement, ReferenceAndPlacementDoNotAllocateAfterConstruction) {
    BubblePlacement p;
    ReferenceStatistics stats;
    std::vector<Execution> rows(100000);
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = {1000. + double(i),100. + double(i % 31),1,int(i % 2)};
    allocations = 0;
    countAllocations = true;
    auto ref = stats.update(rows,rows,{1,100000,1,100000,100000,100000},
                            {1000,101000,60000,-1},Mode::Slot,1000);
    const auto individual = p.build(rows, {Mode::Individual, ref, -1}, 60000).size();
    const auto slot = p.build(rows, {Mode::Slot, ref, -1}, 60000).size();
    countAllocations = false;
    EXPECT_GT(individual, 0);
    EXPECT_GT(slot, 0);
    EXPECT_EQ(allocations, 0);
}
