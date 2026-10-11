#include <gtest/gtest.h>

#include "marketdata/model/TradeData.h"

#include <array>

namespace {
constexpr auto timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1234));

void seed(LiveOrderBook& book) {
    book.initialize(100.0, 106.0, 1.0);
    const std::array<BookLevelUpdate, 5> updates{{
        {true, 101.0, 2.0}, {true, 104.0, 3.0},
        {false, 102.0, 5.0}, {false, 105.0, 7.0}, {false, 106.0, 11.0},
    }};
    book.applyUpdates(updates, timestamp, nullptr);
}
}

TEST(LiveOrderBookTest, SnapshotAndDenseViewPreserveSideOrderAndLimits) {
    LiveOrderBook book("BTC-USD");
    seed(book);
    const auto snapshot = book.snapshotLevels();
    EXPECT_DOUBLE_EQ(snapshot.tickSize, 1.0);
    EXPECT_EQ(snapshot.version, book.stateVersion());
    ASSERT_EQ(snapshot.bids.size(), 2u);
    ASSERT_EQ(snapshot.asks.size(), 3u);
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 101.0);
    EXPECT_DOUBLE_EQ(snapshot.bids[1].price, 104.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 102.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[2].price, 106.0);

    std::vector<std::pair<uint32_t, double>> bids, asks;
    auto view = book.captureDenseNonZero(bids, asks, 2);
    ASSERT_EQ(view.bidLevels.size(), 2u);
    ASSERT_EQ(view.askLevels.size(), 2u);
    EXPECT_EQ(view.bidLevels[0], (std::pair<uint32_t, double>{4, 3.0}));
    EXPECT_EQ(view.bidLevels[1], (std::pair<uint32_t, double>{1, 2.0}));
    EXPECT_EQ(view.askLevels[0], (std::pair<uint32_t, double>{2, 5.0}));
    EXPECT_EQ(view.askLevels[1], (std::pair<uint32_t, double>{5, 7.0}));
    EXPECT_FALSE(view.scanLimited);
    EXPECT_EQ(view.timestamp, timestamp);
    EXPECT_DOUBLE_EQ(view.minPrice, 100.0);
    EXPECT_DOUBLE_EQ(view.maxPrice, 106.0);

    view = book.captureDenseNonZero(bids, asks, 3, 0);
    EXPECT_TRUE(view.bidLevels.empty());
    EXPECT_TRUE(view.askLevels.empty());
    EXPECT_TRUE(view.scanLimited);

    LiveOrderBook wide;
    wide.initialize(0.0, 130.0, 1.0);
    const std::array<BookLevelUpdate, 4> wideUpdates{{
        {true, 1.0, 2.0}, {true, 129.0, 3.0},
        {false, 2.0, 5.0}, {false, 128.0, 7.0},
    }};
    wide.applyUpdates(wideUpdates, timestamp, nullptr);
    view = wide.captureDenseNonZero(bids, asks, 4, 1);
    ASSERT_EQ(view.bidLevels.size(), 1u);
    ASSERT_EQ(view.askLevels.size(), 1u);
    EXPECT_EQ(view.bidLevels[0], (std::pair<uint32_t, double>{129, 3.0}));
    EXPECT_EQ(view.askLevels[0], (std::pair<uint32_t, double>{2, 5.0}));
    EXPECT_TRUE(view.scanLimited);
}

TEST(LiveOrderBookTest, RangeAndSplitClampBothEndsAndKeepBestPrices) {
    LiveOrderBook book;
    seed(book);
    std::vector<double> combined(9, -1.0), bids(9, -1.0), asks(9, -1.0);
    double bestBid = -1.0, bestAsk = -1.0;
    book.accumulateRange(99.0, 107.0, 1.0, combined, &bestBid, &bestAsk);
    EXPECT_EQ(combined, (std::vector<double>{0, 11, 7, 3, 0, 5, 2, 0, 0}));
    EXPECT_DOUBLE_EQ(bestBid, 104.0);
    EXPECT_DOUBLE_EQ(bestAsk, 102.0);
    book.accumulateRangeSplit(99.0, 107.0, 1.0, bids, asks, &bestBid, &bestAsk);
    EXPECT_EQ(bids, (std::vector<double>{0, 0, 0, 3, 0, 0, 2, 0, 0}));
    EXPECT_EQ(asks, (std::vector<double>{0, 11, 7, 0, 0, 5, 0, 0, 0}));
    EXPECT_DOUBLE_EQ(bestBid, 104.0);
    EXPECT_DOUBLE_EQ(bestAsk, 102.0);

    std::vector<double> edge(3, -1.0);
    book.accumulateRange(104.0, 106.0, 1.0, edge, &bestBid, &bestAsk);
    EXPECT_EQ(edge, (std::vector<double>{11, 7, 3}));
    EXPECT_DOUBLE_EQ(bestBid, 104.0);
    EXPECT_DOUBLE_EQ(bestAsk, 105.0);
}

TEST(LiveOrderBookTest, EmptySideUpdatesCountersDeltasAndClear) {
    LiveOrderBook book("BTC-USD");
    book.initialize(100.0, 102.0, 1.0);
    EXPECT_TRUE(book.isEmpty());
    EXPECT_DOUBLE_EQ(book.getMinPrice(), 100.0);
    EXPECT_DOUBLE_EQ(book.getMaxPrice(), 102.0);
    EXPECT_DOUBLE_EQ(book.getTickSize(), 1.0);
    EXPECT_EQ(book.getProductId(), "BTC-USD");
    book.setProductId("ETH-USD");
    EXPECT_EQ(book.getProductId(), "ETH-USD");
    EXPECT_EQ(LiveOrderBook::bucketIndex(101.0, 100.0, 1.0), 1u);
    EXPECT_DOUBLE_EQ(book.index_to_price(2), 102.0);
    const auto initialVersion = book.stateVersion();
    std::vector<BookDelta> deltas;
    const std::array<BookLevelUpdate, 1> updates{{{true, 101.0, 4.0}}};
    book.applyUpdates(updates, timestamp, &deltas);
    EXPECT_EQ(book.stateVersion(), initialVersion + 1);
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].idx, 1u);
    EXPECT_FLOAT_EQ(deltas[0].qty, 4.0f);
    EXPECT_TRUE(deltas[0].isBid);
    EXPECT_EQ(book.getLastUpdate(), timestamp);
    EXPECT_EQ(book.getBidCount(), 1u);
    EXPECT_EQ(book.getAskCount(), 0u);
    EXPECT_DOUBLE_EQ(book.getBidVolume(), 4.0);
    EXPECT_DOUBLE_EQ(book.getAskVolume(), 0.0);
    EXPECT_FALSE(book.isEmpty());

    std::vector<std::pair<uint32_t, double>> bids, asks;
    const auto view = book.captureDenseNonZero(bids, asks, 3);
    ASSERT_EQ(view.bidLevels.size(), 1u);
    EXPECT_TRUE(view.askLevels.empty());
    EXPECT_FALSE(view.scanLimited);
    std::vector<double> bidRows(3, -1.0), askRows(3, -1.0);
    double bestBid = -1.0, bestAsk = -1.0;
    book.accumulateRangeSplit(100.0, 102.0, 1.0, bidRows, askRows, &bestBid, &bestAsk);
    EXPECT_EQ(bidRows, (std::vector<double>{0, 4, 0}));
    EXPECT_EQ(askRows, (std::vector<double>{0, 0, 0}));
    EXPECT_DOUBLE_EQ(bestBid, 101.0);
    EXPECT_DOUBLE_EQ(bestAsk, 0.0);

    const std::array<BookLevelUpdate, 1> remove{{{true, 101.0, 0.0}}};
    book.applyUpdates(remove, timestamp, &deltas);
    EXPECT_EQ(book.getBidCount(), 0u);
    EXPECT_DOUBLE_EQ(book.getBidVolume(), 0.0);
    book.clear();
    EXPECT_TRUE(book.isEmpty());
    EXPECT_DOUBLE_EQ(book.getTickSize(), 0.0);
    EXPECT_TRUE(book.snapshotLevels().bids.empty());
    EXPECT_GT(book.stateVersion(), initialVersion);
    EXPECT_TRUE(book.getBids().empty());
    EXPECT_TRUE(book.getAsks().empty());
}
