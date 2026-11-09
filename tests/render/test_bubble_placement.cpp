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
    ASSERT_EQ(marks.size(), 7); // first bar: two sides + top three; second: side + top one
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
    EXPECT_DOUBLE_EQ(marks[3].notional, 300);
    EXPECT_DOUBLE_EQ(marks[4].notional, 100);
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
    ASSERT_EQ(marks.size(), 5);
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
    ASSERT_EQ(marks.size(), 2);
    EXPECT_EQ(marks[0].notional, 201);
    EXPECT_EQ(marks[1].notional, 201);
}
TEST(BubblePlacement, ReferenceAndPlacementDoNotAllocateAfterConstruction) {
    BubblePlacement p;
    ReferenceStatistics stats;
    std::vector<Execution> rows(100000);
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = {1000. + double(i),100. + double(i % 31),1,int(i % 2)};
    allocations = 0;
    countAllocations = true;
    auto ref = stats.reference(rows);
    const auto individual = p.build(rows, {Mode::Individual, ref, 0}, 60000).size();
    const auto slot = p.build(rows, {Mode::Slot, ref, 0}, 60000).size();
    countAllocations = false;
    EXPECT_GT(individual, 0);
    EXPECT_GT(slot, 0);
    EXPECT_EQ(allocations, 0);
}
