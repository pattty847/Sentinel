#include "marketdata/analytics/BookDiffClassifier.hpp"
#include "marketdata/analytics/BookHistoryRing.hpp"
#include "marketdata/analytics/LevelTracker.hpp"

#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace sentinel::book;

struct Input {
    std::vector<LevelSizes> rows{{10, 20}};
    double first = 100;
    double tick = 1;
    std::uint64_t symbol = 1;
    Snapshot at(Milliseconds now) const { return {symbol, tick, first, rows, now}; }
};

void expectEvent(const BookEvent& event, Side side, EventKind kind, double size, double price = 100)
{
    EXPECT_EQ(event.side, side);
    EXPECT_EQ(event.kind, kind);
    EXPECT_DOUBLE_EQ(event.size, size);
    EXPECT_DOUBLE_EQ(event.price, price);
}

TEST(BookDiff, PureAddPullAndEat)
{
    BookDiffClassifier classifier;
    Input input;
    EXPECT_EQ(classifier.update(input.at(0)), UpdateStatus::Accepted);
    EXPECT_TRUE(classifier.events().empty());
    input.rows[0] = {15, 12};
    EXPECT_EQ(classifier.update(input.at(100)), UpdateStatus::Accepted);
    ASSERT_EQ(classifier.events().size(), 2u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Added, 5);
    expectEvent(classifier.events()[1], Side::Ask, EventKind::Pulled, 8);
    input.rows[0].bid = 8;
    const std::array trades{Trade{100, 7, Aggressor::Sell, 150}};
    classifier.update(input.at(200), trades);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Eaten, 7);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].added, 5);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].eaten, 7);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, 8);
}

TEST(BookDiff, SplitsDropAndDoesNotCarryUnspentTrades)
{
    BookDiffClassifier classifier;
    Input input;
    classifier.update(input.at(10));
    input.rows[0].ask = 10;
    const std::array trades{Trade{100, 3, Aggressor::Buy, 20}, Trade{100, 2, Aggressor::Buy, 25}};
    classifier.update(input.at(30), trades);
    ASSERT_EQ(classifier.events().size(), 2u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Eaten, 5);
    expectEvent(classifier.events()[1], Side::Ask, EventKind::Pulled, 5);
    input.rows[0].ask = 8;
    const std::array excess{Trade{100, 50, Aggressor::Buy, 35}};
    classifier.update(input.at(40), excess);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Eaten, 2);
    input.rows[0].ask = 6;
    classifier.update(input.at(50));
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Pulled, 2);
}

TEST(BookDiff, RefilledTradesWithoutNetDropAreNotEaten)
{
    BookDiffClassifier classifier;
    Input input;
    classifier.update(input.at(0));
    const std::array trades{Trade{100, 30, Aggressor::Buy, 10}};
    classifier.update(input.at(20), trades);
    EXPECT_TRUE(classifier.events().empty());
    input.rows[0].ask = 25;
    const std::array more{Trade{100, 30, Aggressor::Buy, 30}};
    classifier.update(input.at(40), more);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Added, 5);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].eaten, 0);
}

TEST(BookDiff, ShiftAndResizeCompareOnlyOverlappingPrices)
{
    BookDiffClassifier classifier;
    Input input{{{10, 0}, {20, 0}, {30, 0}}};
    classifier.update(input.at(0));
    input.first = 101;
    input.rows = {{15, 0}, {30, 0}, {900, 0}, {800, 0}};
    const std::array trades{Trade{101, 2, Aggressor::Sell, 50}};
    classifier.update(input.at(100), trades);
    ASSERT_EQ(classifier.events().size(), 2u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Eaten, 2, 101);
    expectEvent(classifier.events()[1], Side::Bid, EventKind::Pulled, 3, 101);
    EXPECT_EQ(classifier.events()[0].row, 0u);
    input.first = 100;
    input.rows = {{500, 0}, {15, 0}};
    classifier.update(input.at(200));
    EXPECT_TRUE(classifier.events().empty());
    input.first = 1000;
    classifier.update(input.at(300));
    EXPECT_TRUE(classifier.events().empty());
}

TEST(BookDiff, SideCrossingClassifiesEachSideIndependently)
{
    BookDiffClassifier classifier;
    Input input{{{10, 0}}};
    classifier.update(input.at(0));
    input.rows[0] = {0, 12};
    const std::array sell{Trade{100, 4, Aggressor::Sell, 1}};
    classifier.update(input.at(2), sell);
    ASSERT_EQ(classifier.events().size(), 3u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Eaten, 4);
    expectEvent(classifier.events()[1], Side::Bid, EventKind::Pulled, 6);
    expectEvent(classifier.events()[2], Side::Ask, EventKind::Added, 12);
    input.rows[0] = {8, 0};
    const std::array buy{Trade{100, 12, Aggressor::Buy, 3}};
    classifier.update(input.at(4), buy);
    ASSERT_EQ(classifier.events().size(), 2u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Added, 8);
    expectEvent(classifier.events()[1], Side::Ask, EventKind::Eaten, 12);
}

TEST(BookDiff, TradeClockAndAggressorFiltering)
{
    BookDiffClassifier classifier;
    Input input;
    classifier.update(input.at(10));
    input.rows[0] = {0, 0};
    const std::array trades{
        Trade{100, 100, Aggressor::Buy, 10}, Trade{100, 100, Aggressor::Sell, 31},
        Trade{100, 100, Aggressor::Unknown, 20}, Trade{101, 100, Aggressor::Buy, 20},
        Trade{100, 3, Aggressor::Sell, 30}, Trade{100, 4, Aggressor::Buy, 30}};
    classifier.update(input.at(30), trades);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].eaten, 3);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 7);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].eaten, 4);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, 16);
}

TEST(BookDiff, DecimalGridAndRawTradeBucket)
{
    BookDiffClassifier classifier;
    Input input{{{10, 0}, {20, 0}}, 0.3, 0.1};
    classifier.update(input.at(0));
    input.first = 0.4;
    input.rows = {{15, 0}};
    const std::array trades{Trade{0.49, 2, Aggressor::Sell, 1}};
    classifier.update(input.at(2), trades);
    ASSERT_EQ(classifier.events().size(), 2u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Eaten, 2, 0.4);
    expectEvent(classifier.events()[1], Side::Bid, EventKind::Pulled, 3, 0.4);
}

TEST(BookDiff, ThresholdFiltersSplitComponentsAndTotals)
{
    BookDiffClassifier classifier({.minimumSize = 3});
    Input input;
    classifier.update(input.at(0));
    input.rows[0] = {12, 15};
    const std::array trades{Trade{100, 2, Aggressor::Buy, 1}};
    classifier.update(input.at(2), trades);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Pulled, 3);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].added, 0);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].eaten, 0);
}

TEST(BookDiff, RollingExpiryAndLargestPullAcrossSides)
{
    BookDiffClassifier classifier({.windowMs = 100});
    Input input;
    classifier.update(input.at(0));
    input.rows[0] = {3, 12};
    classifier.update(input.at(10));
    input.rows[0] = {0, 10};
    classifier.update(input.at(20));
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 10);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].largestPull, 7);
    ASSERT_TRUE(classifier.totals().largestPull);
    EXPECT_EQ(classifier.totals().largestPull->side, Side::Ask);
    EXPECT_DOUBLE_EQ(classifier.totals().largestPull->size, 8);
    classifier.update(input.at(109));
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, 10);
    classifier.update(input.at(110));
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 3);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, 2);
    ASSERT_TRUE(classifier.totals().largestPull);
    EXPECT_DOUBLE_EQ(classifier.totals().largestPull->size, 3);
    classifier.update(input.at(120));
    EXPECT_FALSE(classifier.totals().largestPull);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 0);
}

TEST(BookDiff, LargestPullTiesKeepOldestEventUntilItExpires)
{
    BookDiffClassifier classifier({.windowMs = 100});
    Input input;
    classifier.update(input.at(0));
    input.rows[0].bid = 5;
    classifier.update(input.at(10));
    input.rows[0].ask = 15;
    classifier.update(input.at(20));
    ASSERT_TRUE(classifier.totals().largestPull);
    EXPECT_EQ(classifier.totals().largestPull->timestampMs, 10);
    EXPECT_EQ(classifier.totals().largestPull->side, Side::Bid);
    classifier.update(input.at(110));
    ASSERT_TRUE(classifier.totals().largestPull);
    EXPECT_EQ(classifier.totals().largestPull->timestampMs, 20);
    EXPECT_EQ(classifier.totals().largestPull->side, Side::Ask);
}

TEST(BookDiff, CountsTradesOutsideSnapshotClockInterval)
{
    BookDiffClassifier classifier;
    Input input;
    classifier.update(input.at(100));
    input.rows[0].ask = 10;
    const std::array trades{
        Trade{100, 50, Aggressor::Buy, 99}, Trade{100, 50, Aggressor::Buy, 100},
        Trade{100, 50, Aggressor::Buy, 201}, Trade{100, 50, Aggressor::Buy, 1700000000000},
        Trade{100, 2, Aggressor::Buy, 199}, Trade{100, 3, Aggressor::Buy, 200},
        Trade{100, -1, Aggressor::Buy, 150}};
    classifier.update(input.at(200), trades);
    EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 4u);
    EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 4u);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].eaten, 5);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, 5);
    const std::array stale{Trade{100, 5, Aggressor::Buy, 200}};
    classifier.update(input.at(300), stale);
    EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 1u);
    EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 5u);
    classifier.update(input.at(400));
    EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 0u);
    EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 5u);
    EXPECT_EQ(classifier.update(input.at(399), stale), UpdateStatus::InvalidInput);
    EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 0u);
    EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 5u);
}

TEST(BookDiff, RejectedTradeCountsResetWithSeriesAndExcludeRejectedUpdates)
{
    BookDiffClassifier classifier({.maxSteps = 1});
    Input input;
    for (int series = 0; series < 4; ++series) {
        const Milliseconds now = series * 100;
        const std::array stale{Trade{100, 1, Aggressor::Buy, now}};
        classifier.update(input.at(now), stale); // Fresh baseline has no interval.
        EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 0u);
        EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 0u);
        input.rows[0].bid += 1;
        classifier.update(input.at(now + 10), stale);
        EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 1u);
        EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 1u);
        input.rows[0].bid += 1;
        EXPECT_EQ(classifier.update(input.at(now + 20), stale), UpdateStatus::CapacityExceeded);
        EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 0u);
        EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 1u);
        if (series == 0) ++input.symbol;
        else if (series == 1) input.tick = 0.5;
        else classifier.update({});
    }
    classifier.reset();
    EXPECT_EQ(classifier.outOfIntervalTradesThisUpdate(), 0u);
    EXPECT_EQ(classifier.outOfIntervalTradesTotal(), 0u);
}

TEST(BookDiff, RingCapacityRejectsAtomicallyAndReusesExpiredSlots)
{
    BookDiffClassifier classifier({.maxSteps = 1, .windowMs = 100});
    Input input;
    classifier.update(input.at(0));
    input.rows[0].bid = 8;
    classifier.update(input.at(1));
    input.rows[0].bid = 3;
    EXPECT_EQ(classifier.update(input.at(2)), UpdateStatus::CapacityExceeded);
    EXPECT_TRUE(classifier.events().empty());
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 2);
    EXPECT_EQ(classifier.update(input.at(101)), UpdateStatus::Accepted);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Pulled, 5);
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, 5);
}

TEST(BookAnalytics, TickAndSymbolChangesResetEveryConsumer)
{
    BookDiffClassifier classifier;
    LevelTracker tracker;
    BookHistoryRing history;
    Input input;
    for (int series = 0; series < 3; ++series) {
        EXPECT_EQ(classifier.update(input.at(0)), UpdateStatus::Accepted);
        EXPECT_TRUE(classifier.events().empty());
        EXPECT_DOUBLE_EQ(classifier.totals().side[0].added, 0);
        tracker.update(input.at(0));
        EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 0);
        history.update(input.at(0));
        EXPECT_EQ(history.size(), 1u);
        input.rows[0].bid += 1;
        classifier.update(input.at(300));
        tracker.update(input.at(300));
        history.update(input.at(300));
        EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 300);
        EXPECT_EQ(history.size(), 2u);
        if (series == 0) input.tick = 0.5;
        else ++input.symbol;
    }
}

TEST(LevelTracker, NewSeriesClearsAgeAndFlickersDespiteOverlappingKeysAndLaterTime)
{
    for (const bool changeTick : {false, true}) {
        LevelTracker tracker;
        Input input{{{10, 20}, {10, 20}}, 1};
        tracker.update(input.at(100));
        input.rows = {{0, 0}, {0, 0}};
        tracker.update(input.at(110));
        input.rows = {{10, 20}, {10, 20}};
        tracker.update(input.at(120));
        tracker.update(input.at(130));
        for (const auto side : sides) {
            EXPECT_EQ(tracker.level(0, side).seenForMs, 10);
            EXPECT_EQ(tracker.level(0, side).flickers, 1u);
        }
        if (changeTick) {
            // Old prices 1,2; new prices 2,4. Price 2 and integer keys 1,2 overlap.
            input.tick = 2;
            input.first = 2;
        } else {
            ++input.symbol;
        }
        ASSERT_EQ(tracker.update(input.at(200)), UpdateStatus::Accepted);
        for (std::size_t row = 0; row < input.rows.size(); ++row) {
            for (const auto side : sides) {
                EXPECT_TRUE(tracker.level(row, side).present);
                EXPECT_EQ(tracker.level(row, side).seenForMs, 0);
                EXPECT_EQ(tracker.level(row, side).flickers, 0u);
            }
        }
    }
}

TEST(BookAnalytics, WithdrawalIsNotAPullOrFlickerAndRestartsHistory)
{
    BookDiffClassifier classifier;
    LevelTracker tracker;
    BookHistoryRing history;
    Input input;
    classifier.update(input.at(0));
    tracker.update(input.at(0));
    history.update(input.at(0));
    input.rows[0] = {10, 0};
    classifier.update(input.at(10));
    tracker.update(input.at(10));
    EXPECT_TRUE(classifier.totals().largestPull);
    EXPECT_EQ(classifier.update({}), UpdateStatus::Reset);
    EXPECT_EQ(tracker.update({}), UpdateStatus::Reset);
    EXPECT_EQ(history.update({}), UpdateStatus::Reset);
    EXPECT_TRUE(classifier.events().empty());
    EXPECT_FALSE(classifier.totals().largestPull);
    EXPECT_FALSE(tracker.level(0, Side::Bid).present);
    EXPECT_EQ(history.size(), 0u);
    input.rows[0] = {20, 20};
    classifier.update(input.at(20));
    tracker.update(input.at(20));
    history.update(input.at(20));
    EXPECT_TRUE(classifier.events().empty());
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 0u);
    EXPECT_EQ(tracker.level(0, Side::Ask).flickers, 0u);
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 0);
    EXPECT_EQ(history.size(), 1u);
}

TEST(BookAnalytics, InvalidSizesAreUnknownNotZeroAndOtherSideStillWorks)
{
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), -1.0}) {
        BookDiffClassifier classifier;
        LevelTracker tracker;
        BookHistoryRing history({.sampleIntervalMs = 1});
        Input input;
        classifier.update(input.at(0));
        tracker.update(input.at(0));
        input.rows[0] = {bad, 25};
        classifier.update(input.at(1));
        tracker.update(input.at(1));
        history.update(input.at(1));
        ASSERT_EQ(classifier.events().size(), 1u);
        expectEvent(classifier.events()[0], Side::Ask, EventKind::Added, 5);
        EXPECT_FALSE(tracker.level(0, Side::Bid).present);
        ASSERT_TRUE(history.cell(0, 0));
        EXPECT_TRUE(std::isnan(history.cell(0, 0)->bid));
        EXPECT_DOUBLE_EQ(history.cell(0, 0)->ask, 25);
        input.rows[0] = {20, 25};
        classifier.update(input.at(2));
        tracker.update(input.at(2));
        EXPECT_TRUE(classifier.events().empty());
        EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 0u);
        EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 0);
    }
}

TEST(BookDiff, InvalidTradesNeverExplainADecrease)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    BookDiffClassifier classifier;
    Input input;
    classifier.update(input.at(0));
    input.rows[0].ask = 0;
    const std::array trades{
        Trade{nan, 10, Aggressor::Buy, 1}, Trade{inf, 10, Aggressor::Buy, 1},
        Trade{-1, 10, Aggressor::Buy, 1}, Trade{100, nan, Aggressor::Buy, 1},
        Trade{100, inf, Aggressor::Buy, 1}, Trade{100, -1, Aggressor::Buy, 1}};
    classifier.update(input.at(2), trades);
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Ask, EventKind::Pulled, 20);
}

TEST(BookAnalytics, InvalidWindowAndOutOfOrderUpdatesPreserveState)
{
    BookDiffClassifier classifier;
    LevelTracker tracker;
    BookHistoryRing history;
    Input input;
    classifier.update(input.at(10));
    tracker.update(input.at(10));
    history.update(input.at(10));
    std::array<Snapshot, 7> invalid;
    invalid.fill(input.at(20));
    invalid[0].tickSize = 0;
    invalid[1].firstPrice = std::numeric_limits<double>::quiet_NaN();
    invalid[2].firstPrice = -1;
    invalid[3].firstPrice = 100.5;
    invalid[4].timestampMs = -1;
    invalid[5].timestampMs = 10;
    invalid[6].firstPrice = std::numeric_limits<double>::max();
    for (const auto& snapshot : invalid) {
        EXPECT_EQ(classifier.update(snapshot), UpdateStatus::InvalidInput);
        EXPECT_EQ(tracker.update(snapshot), UpdateStatus::InvalidInput);
        EXPECT_EQ(history.update(snapshot), UpdateStatus::InvalidInput);
    }
    input.rows[0].bid = 5;
    classifier.update(input.at(300));
    tracker.update(input.at(300));
    history.update(input.at(300));
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Pulled, 5);
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 290);
    EXPECT_EQ(history.size(), 2u);
}

TEST(BookAnalytics, OversizedWindowIsRejectedByEveryConsumer)
{
    BookDiffClassifier classifier({.maxRows = 1});
    LevelTracker tracker({.maxRows = 1});
    BookHistoryRing history({.maxRows = 1});
    Input input;
    classifier.update(input.at(0));
    tracker.update(input.at(0));
    history.update(input.at(0));
    input.rows.push_back({30, 40});
    EXPECT_EQ(classifier.update(input.at(1)), UpdateStatus::CapacityExceeded);
    EXPECT_EQ(tracker.update(input.at(1)), UpdateStatus::CapacityExceeded);
    EXPECT_EQ(history.update(input.at(1)), UpdateStatus::CapacityExceeded);
    EXPECT_EQ(history.size(), 1u);
    input.rows.resize(1);
    input.rows[0].bid = 8;
    classifier.update(input.at(2));
    ASSERT_EQ(classifier.events().size(), 1u);
    expectEvent(classifier.events()[0], Side::Bid, EventKind::Pulled, 2);
}

TEST(LevelTracker, FlickersRequireAnObservedDisappearanceAndRestartAge)
{
    LevelTracker tracker({.minimumSize = 5, .flickerWindowMs = 100});
    Input input{{{0, 0}}};
    tracker.update(input.at(0));
    input.rows[0].bid = 10;
    tracker.update(input.at(10));
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 0u);
    tracker.update(input.at(20));
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 10);
    input.rows[0].bid = 4;
    tracker.update(input.at(30));
    tracker.update(input.at(50)); // Repeated absence does not move disappearance time.
    input.rows[0].bid = 5;
    tracker.update(input.at(130));
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 1u);
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 0);
    input.rows[0].bid = 0;
    tracker.update(input.at(140));
    input.rows[0].bid = 5;
    tracker.update(input.at(241));
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 1u);
    tracker.update(input.at(250));
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 9);
}

TEST(LevelTracker, ShiftPreservesOverlappingAgeButUnobservedReturnsStartFresh)
{
    LevelTracker tracker;
    Input input{{{10, 0}, {20, 0}, {30, 0}}};
    tracker.update(input.at(100));
    input.first = 101;
    input.rows = {{20, 0}, {30, 0}, {40, 0}, {50, 0}};
    tracker.update(input.at(500));
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 400);
    EXPECT_EQ(tracker.level(1, Side::Bid).seenForMs, 400);
    EXPECT_EQ(tracker.level(2, Side::Bid).seenForMs, 0);
    input.first = 100;
    input.rows = {{10, 0}, {20, 0}};
    tracker.update(input.at(600));
    EXPECT_EQ(tracker.level(0, Side::Bid).seenForMs, 0);
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 0u);
    EXPECT_EQ(tracker.level(1, Side::Bid).seenForMs, 500);
}

TEST(LevelTracker, SideCrossingDoesNotTransferAge)
{
    LevelTracker tracker;
    Input input{{{10, 0}}};
    tracker.update(input.at(0));
    input.rows[0] = {0, 10};
    tracker.update(input.at(100));
    EXPECT_FALSE(tracker.level(0, Side::Bid).present);
    EXPECT_EQ(tracker.level(0, Side::Ask).seenForMs, 0);
    EXPECT_EQ(tracker.level(0, Side::Ask).flickers, 0u);
    input.rows[0] = {10, 0};
    tracker.update(input.at(200));
    EXPECT_EQ(tracker.level(0, Side::Bid).flickers, 1u);
}

TEST(LevelTracker, LargeLevelsUsePerSideNonemptyMedianIncludingEvenCase)
{
    LevelTracker tracker;
    Input input{{{1, 10}, {2, 20}, {8, 80}, {0, 0}}};
    tracker.update(input.at(0));
    EXPECT_DOUBLE_EQ(tracker.median(Side::Bid), 2);
    EXPECT_DOUBLE_EQ(tracker.median(Side::Ask), 20);
    EXPECT_TRUE(tracker.isLarge(2, Side::Bid));
    EXPECT_TRUE(tracker.isLarge(2, Side::Ask));
    EXPECT_FALSE(tracker.isLarge(1, Side::Bid));
    EXPECT_FALSE(tracker.isLarge(3, Side::Ask));
    EXPECT_FALSE(tracker.isLarge(9, Side::Ask));
    EXPECT_FALSE(tracker.isLarge(2, Side::Bid, std::numeric_limits<double>::quiet_NaN()));
    input.rows = {{2, 0}, {4, 0}, {12, 0}, {0, 0}, {0.5, 0}};
    tracker.update(input.at(1));
    EXPECT_DOUBLE_EQ(tracker.median(Side::Bid), 3);
    EXPECT_TRUE(tracker.isLarge(2, Side::Bid));
    EXPECT_DOUBLE_EQ(tracker.median(Side::Ask), 0);
    EXPECT_FALSE(tracker.isLarge(0, Side::Ask));
}

TEST(BookHistory, AlignsColumnsToLatestWindowEvenBetweenSamples)
{
    BookHistoryRing history({.maxRows = 4, .columns = 3});
    Input input{{{10, 1}, {20, 2}, {30, 3}}};
    EXPECT_EQ(history.update(input.at(0)), UpdateStatus::Accepted);
    input.first = 101;
    input.rows = {{21, 4}, {31, 5}, {41, 6}, {51, 7}};
    EXPECT_EQ(history.update(input.at(100)), UpdateStatus::NotSampled);
    ASSERT_TRUE(history.cell(0, 0));
    EXPECT_DOUBLE_EQ(history.cell(0, 0)->bid, 20);
    EXPECT_DOUBLE_EQ(history.cell(0, 1)->ask, 3);
    EXPECT_FALSE(history.cell(0, 2));
    EXPECT_EQ(history.update(input.at(250)), UpdateStatus::Accepted);
    EXPECT_DOUBLE_EQ(history.cell(0, 0)->bid, 21);
    EXPECT_DOUBLE_EQ(history.cell(1, 0)->bid, 20);
    input.first = 100;
    input.rows = {{11, 8}, {22, 9}};
    history.update(input.at(500));
    EXPECT_FALSE(history.cell(1, 0));
    EXPECT_DOUBLE_EQ(history.cell(1, 1)->bid, 21);
    EXPECT_DOUBLE_EQ(history.cell(2, 0)->bid, 10);
    EXPECT_DOUBLE_EQ(history.cell(2, 1)->bid, 20);
    EXPECT_FALSE(history.cell(0, 2));
}

TEST(BookHistory, WrapsAtCapacityAndMarksMissedSlotsMissing)
{
    BookHistoryRing history({.columns = 2, .sampleIntervalMs = 5000});
    Input input;
    history.update(input.at(0));
    EXPECT_EQ(history.update(input.at(4999)), UpdateStatus::NotSampled);
    history.update(input.at(5000));
    EXPECT_EQ(history.size(), 2u);
    input.rows[0].bid = 30;
    history.update(input.at(20000));
    EXPECT_EQ(history.size(), 2u);
    EXPECT_EQ(history.timestamp(0), 20000);
    EXPECT_FALSE(history.timestamp(1));
    EXPECT_FALSE(history.timestamp(2));
    EXPECT_DOUBLE_EQ(history.cell(0, 0)->bid, 30);
    EXPECT_FALSE(history.cell(1, 0));
    EXPECT_FALSE(history.cell(2, 0));
}

TEST(BookHistory, CallerCadenceDoesNotDriftSamplingGrid)
{
    BookHistoryRing history({.columns = 16});
    Input input;
    constexpr Milliseconds anchor = 17;
    history.update(input.at(anchor));
    Milliseconds previousSlot = 0;
    for (Milliseconds elapsed = 67; elapsed <= 2010; elapsed += 67) {
        const auto slot = elapsed / 250;
        const auto result = history.update(input.at(anchor + elapsed));
        EXPECT_EQ(result, slot == previousSlot ? UpdateStatus::NotSampled : UpdateStatus::Accepted);
        if (slot != previousSlot)
            EXPECT_EQ(history.timestamp(0), anchor + elapsed);
        previousSlot = slot;
    }
    ASSERT_EQ(history.size(), 9u);
    EXPECT_EQ(history.timestamp(0), anchor + 2010);
    EXPECT_EQ(history.timestamp(1), anchor + 1809);
    EXPECT_EQ(history.timestamp(4), anchor + 1005); // Not the drifting 1072 ms sample.
    EXPECT_EQ(history.timestamp(8), anchor);
}

TEST(BookHistory, MissingSlotsRemainDistinctAndAlignedByPrice)
{
    BookHistoryRing history({.maxRows = 2, .columns = 8});
    Input input{{{10, 1}, {20, 2}}};
    history.update(input.at(17));
    history.update(input.at(285)); // Slot due at 267.
    input.first = 101;
    input.rows = {{30, 3}, {40, 4}};
    history.update(input.at(1089)); // Current slot 1017; 517 and 767 were missed.
    ASSERT_EQ(history.size(), 5u);
    EXPECT_EQ(history.timestamp(0), 1089);
    EXPECT_DOUBLE_EQ(history.cell(0, 0)->bid, 30);
    for (std::size_t slot = 1; slot <= 2; ++slot) {
        EXPECT_FALSE(history.timestamp(slot));
        EXPECT_FALSE(history.cell(slot, 0));
    }
    EXPECT_EQ(history.timestamp(3), 285);
    ASSERT_TRUE(history.cell(3, 0));
    EXPECT_DOUBLE_EQ(history.cell(3, 0)->bid, 20);
    EXPECT_FALSE(history.cell(3, 1));
    EXPECT_EQ(history.update(input.at(1266)), UpdateStatus::NotSampled);
    EXPECT_EQ(history.update(input.at(1267)), UpdateStatus::Accepted);
    EXPECT_FALSE(history.timestamp(2));
    EXPECT_EQ(history.timestamp(4), 285);
}

TEST(BookHistory, HugeGapsAreBoundedAndTimestampLimitDoesNotOverflow)
{
    BookHistoryRing history({.columns = 3, .sampleIntervalMs = 1});
    Input input;
    history.update(input.at(0));
    const auto last = std::numeric_limits<Milliseconds>::max();
    EXPECT_EQ(history.update(input.at(last - 1)), UpdateStatus::Accepted);
    ASSERT_EQ(history.size(), 3u);
    EXPECT_EQ(history.timestamp(0), last - 1);
    EXPECT_FALSE(history.timestamp(1));
    EXPECT_FALSE(history.timestamp(2));
    EXPECT_FALSE(history.cell(2, 0));
    EXPECT_EQ(history.update(input.at(last)), UpdateStatus::Accepted);
    EXPECT_EQ(history.timestamp(0), last);
    EXPECT_EQ(history.timestamp(1), last - 1);
    EXPECT_FALSE(history.timestamp(2));
    history.update({});
    history.update(input.at(100));
    EXPECT_EQ(history.size(), 1u);
    EXPECT_EQ(history.timestamp(0), 100);
}

TEST(BookAnalytics, InvalidConfigurationIsRejectedAtConstruction)
{
    EXPECT_THROW((BookDiffClassifier{DiffConfig{.maxRows = 0}}), std::invalid_argument);
    EXPECT_THROW((BookDiffClassifier{DiffConfig{.windowMs = 0}}), std::invalid_argument);
    EXPECT_THROW((BookDiffClassifier{DiffConfig{.minimumSize = -1}}), std::invalid_argument);
    EXPECT_THROW((LevelTracker{TrackerConfig{.flickerWindowMs = 0}}), std::invalid_argument);
    EXPECT_THROW((BookHistoryRing{HistoryConfig{.columns = 0}}), std::invalid_argument);
    EXPECT_THROW((BookHistoryRing{HistoryConfig{.sampleIntervalMs = -1}}), std::invalid_argument);
}

TEST(BookDiff, FiniteExtremeSizesDoNotOverflowTotals)
{
    BookDiffClassifier classifier;
    const double huge = std::numeric_limits<double>::max();
    Input input{{{huge, huge}, {huge, huge}}};
    classifier.update(input.at(0));
    input.rows = {{0, 0}, {0, 0}};
    classifier.update(input.at(1));
    EXPECT_DOUBLE_EQ(classifier.totals().side[0].pulled, huge);
    EXPECT_DOUBLE_EQ(classifier.totals().side[1].pulled, huge);
}
} // namespace
