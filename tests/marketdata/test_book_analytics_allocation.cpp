#include "marketdata/analytics/BookDiffClassifier.hpp"
#include "marketdata/analytics/BookHistoryRing.hpp"
#include "marketdata/analytics/LevelTracker.hpp"

#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <new>

namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
}

// Catch allocations from the actual library, including standard-container growth.
void* operator new(std::size_t size)
{
    if (countAllocations)
        ++allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

TEST(BookAnalytics, UpdatesQueriesAndResetsAllocateNothingAfterConstruction)
{
    using namespace sentinel::book;
    BookDiffClassifier classifier({.maxRows = 120, .maxSteps = 256});
    LevelTracker tracker({.maxRows = 120});
    BookHistoryRing history({.maxRows = 120, .columns = 8});
    std::array<LevelSizes, 120> rows{};
    std::array<Trade, 2> trades{};
    allocations = 0;
    countAllocations = true;
    bool accepted = true;
    for (int step = 0; step < 1000; ++step) {
        const double first = 100 + step % 20;
        const Milliseconds now = step * 67;
        const auto count = static_cast<std::size_t>(40 + step % 81);
        for (auto& row : rows)
            row = {step % 3 == 0 ? 0.0 : 20.0, step % 2 == 0 ? 10.0 : 30.0};
        trades = {Trade{first, 3, Aggressor::Sell, now - 100}, Trade{first, 4, Aggressor::Buy, now}};
        const Snapshot snapshot{static_cast<std::uint64_t>(step / 200), 1, first,
                                std::span<const LevelSizes>(rows.data(), count), now};
        accepted &= classifier.update(snapshot, trades) == UpdateStatus::Accepted;
        accepted &= tracker.update(snapshot) == UpdateStatus::Accepted;
        history.update({snapshot.symbolId, 1, first, snapshot.rows, step * 50000LL});
        (void)classifier.totals();
        (void)tracker.isLarge(0, Side::Bid);
        (void)tracker.level(0, Side::Ask);
        (void)history.cell(3, 0);
        if (step % 100 == 99) {
            classifier.update({});
            tracker.update({});
            history.update({});
        }
    }
    countAllocations = false;
    EXPECT_TRUE(accepted);
    EXPECT_EQ(allocations, 0u);
}
