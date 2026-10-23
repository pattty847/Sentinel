#include <gtest/gtest.h>
#include "servermodel/OverlayTradeSelection.hpp"
#include <random>

namespace {
struct Trade { int64_t timestampMs; int price; };
}

TEST(TradeOverlaySelection, NewestMatchesSortedReferenceWithBoundedStorage) {
    constexpr size_t limit = 250000;
    std::vector<Trade> tape;
    for (size_t i = 0; i < limit + 100; ++i)
        tape.push_back({static_cast<int64_t>(i + 1), static_cast<int>(i % 10)});
    std::mt19937 rng(42);
    std::shuffle(tape.begin(), tape.end(), rng);
    std::vector<Trade> selected;
    OverlayTradeSelection selection(1, limit + 101, limit, selected, OverlayTradeBudget::Newest);
    for (const auto& trade : tape) {
        ASSERT_TRUE(selection.add(trade));
        ASSERT_LE(selected.size(), limit);
    }
    selection.finish();
    ASSERT_TRUE(selection.budgetExceeded());
    EXPECT_EQ(selection.retainedFromMs(), 101);
    std::sort(tape.begin(), tape.end(), [](const auto& a, const auto& b) {
        return a.timestampMs < b.timestampMs;
    });
    ASSERT_EQ(selected.size(), limit);
    for (size_t i = 0; i < limit; ++i) {
        ASSERT_EQ(selected[i].timestampMs, tape[i + 100].timestampMs);
        ASSERT_EQ(selected[i].price, tape[i + 100].price);
    }
}

TEST(TradeOverlaySelection, StrictBudgetRejectsOverflowWithoutPartialSnapshot) {
    std::vector<Trade> selected;
    OverlayTradeSelection selection(10, 20, 2, selected, OverlayTradeBudget::Reject);
    ASSERT_TRUE(selection.add({9, 1}));
    ASSERT_TRUE(selection.add({20, 2}));
    ASSERT_TRUE(selection.add({19, 3}));
    ASSERT_TRUE(selection.add({10, 4}));
    EXPECT_FALSE(selection.add({15, 5}));
    EXPECT_TRUE(selected.empty());
}

TEST(TradeOverlaySelection, EmptyBudgetBackfillsThroughNewestOmittedTimestamp) {
    std::vector<Trade> selected;
    OverlayTradeSelection selection(10, 20, 0, selected, OverlayTradeBudget::Newest);
    ASSERT_TRUE(selection.add({19, 1}));
    ASSERT_TRUE(selection.add({10, 2}));
    selection.finish();
    EXPECT_TRUE(selected.empty());
    EXPECT_TRUE(selection.budgetExceeded());
    EXPECT_EQ(selection.retainedFromMs(), 20);
}
