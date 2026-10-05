#include "models/DomModel.hpp"
#include "mainwindow/AgentApiSnapshots.hpp"
#include "themes/DarkTheme.hpp"
#include "widgets/OrderBookDock.hpp"
#include "datasources/IGridDataSource.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QMainWindow>
#include <QWheelEvent>
#include <QAbstractItemModelTester>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <gtest/gtest.h>
#include <algorithm>
#include <iostream>
#include <numeric>

namespace {
Trade trade(double price, AggressorSide side, double size = 1) {
    return {std::chrono::system_clock::now(), "BTC-USD", "", side, price, size};
}
void apply(LiveOrderBook& book, std::initializer_list<BookLevelUpdate> updates, qint64 receiveMs = 0) {
    const auto at = receiveMs ? std::chrono::system_clock::time_point(std::chrono::milliseconds(receiveMs)) : std::chrono::system_clock::now();
    book.applyUpdates(std::span(updates.begin(), updates.size()), at, nullptr);
}
int rowAt(DomModel& model, qint64 key) {
    for (int r = 0; r < model.rowCount(); ++r)
        if (model.index(r, DomModel::Price).data(DomModel::BucketRole).toLongLong() == key) return r;
    return -1;
}
int count(DomModel& model, int row, int col) { return model.index(row, col).data().toInt(); }

TEST(DomModel, SameBucketUniqueBestAskStableEmptyRowsAndAlignment) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200, 7}, {false, 200.2, 2}});
    DomModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.clear("BTC-USD");
    DomTradeWindow trades;
    model.publish(book, trades, true);
    ASSERT_EQ(model.rowCount(), DomModel::Rows);
    const int row = rowAt(model, 2000);
    ASSERT_GE(row, 0);
    EXPECT_DOUBLE_EQ(model.index(row, DomModel::Bid).data(DomModel::QuantityRole).toDouble(), 4);
    EXPECT_DOUBLE_EQ(model.index(row, DomModel::Ask).data(DomModel::QuantityRole).toDouble(), 7);
    int bestAsks = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        bestAsks += model.index(r, 0).data(DomModel::BestSideRole).toString().contains("Ask");
        if (r) EXPECT_EQ(model.index(r - 1, 0).data(DomModel::BucketRole).toLongLong() - 1,
                         model.index(r, 0).data(DomModel::BucketRole).toLongLong());
        EXPECT_TRUE(model.index(r, DomModel::Price).data(Qt::TextAlignmentRole).toInt() & Qt::AlignRight);
    }
    EXPECT_EQ(bestAsks, 1);
    EXPECT_EQ(model.index(row - 5, DomModel::Ask).data(DomModel::QuantityRole).toDouble(), 0);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    apply(book, {{false, 200.2, 0}, {false, 202, 8}});
    model.publish(book, trades, false);
    EXPECT_EQ(rowAt(model, 2000), row);
    EXPECT_EQ(reset.count(), 0);
    EXPECT_TRUE(model.summary().contains("Bucketed spread 0.0 USD"));
    EXPECT_TRUE(model.aggregation().contains("0.1 USD"));
    EXPECT_TRUE(model.headerData(DomModel::Bid, Qt::Horizontal, Qt::ToolTipRole).toString().contains("BTC"));
}

TEST(DomModel, EmptyBookAndUnchangedPublicationDoNotInventRowsOrChurn) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    DomModel model;
    DomTradeWindow trades;
    trades.ingest(trade(200, AggressorSide::Buy));
    model.publish(book, trades, true);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_TRUE(model.aggregation().contains("0.1"));
    EXPECT_TRUE(model.executionSummary().contains("(1 received)"));
    apply(book, {{true, 200, 1}, {false, 201, 1}});
    model.publish(book, trades, true);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.publish(book, trades, true);
    EXPECT_EQ(changed.count(), 0);
    apply(book, {{true, 200, 0}, {false, 201, 0}});
    model.publish(book, trades, false);
    EXPECT_EQ(model.rowCount(), DomModel::Rows);
    EXPECT_EQ(model.index(rowAt(model, 2000), DomModel::Bid).data(DomModel::QuantityRole).toDouble(), 0);
    EXPECT_EQ(count(model, rowAt(model, 2000), DomModel::Buys), 1);
    EXPECT_TRUE(model.summary().contains("Bid — · Ask —"));
}

TEST(DomModel, RingCountsEveryEventUnknownNeverSellsAndCountsAreNotVolume) {
    DomTradeWindow trades;
    for (int i = 0; i < 1250; ++i) trades.ingest(trade(200, AggressorSide::Buy, 100000));
    for (int i = 0; i < 300; ++i) trades.ingest(trade(200, AggressorSide::Sell, 0.0001));
    for (int i = 0; i < 50; ++i) trades.ingest(trade(200, AggressorSide::Unknown));
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.publish(book, trades, true);
    const int r = rowAt(model, 2000);
    EXPECT_EQ(trades.size(), 1000);
    EXPECT_EQ(count(model, r, DomModel::Buys), 650);
    EXPECT_EQ(count(model, r, DomModel::Sells), 300);
    EXPECT_EQ(count(model, r, DomModel::Delta), 350);
    EXPECT_TRUE(model.executionSummary().contains("last 1,000 trades (1000 received)"));
    EXPECT_TRUE(model.executionSummary().contains("unknown: 50"));
    for (int c = DomModel::Buys; c < DomModel::Columns; ++c) {
        EXPECT_TRUE(model.headerData(c, Qt::Horizontal, Qt::ToolTipRole).toString().contains("counts"));
        EXPECT_FALSE(model.headerData(c, Qt::Horizontal, Qt::DisplayRole).toString().contains("volume"));
    }
}

TEST(DomModel, ManualAnchorAndRecenterAcrossRapidBookMoves) {
    LiveOrderBook book;
    book.initialize(0, 500, 0.1);
    apply(book, {{true, 200, 1}, {false, 200.1, 1}});
    DomModel model;
    DomTradeWindow trades;
    model.publish(book, trades, true);
    const auto top = model.index(0, 0).data(DomModel::BucketRole);
    for (int i = 0; i < 100; ++i) {
        apply(book, {{true, 200 + i * 0.1, 0}, {false, 200.1 + i * 0.1, 0},
                     {true, 200.1 + i * 0.1, 1}, {false, 200.2 + i * 0.1, 1}});
        model.publish(book, trades, false);
        EXPECT_EQ(model.index(0, 0).data(DomModel::BucketRole), top);
    }
    model.publish(book, trades, true);
    EXPECT_NE(model.index(0, 0).data(DomModel::BucketRole), top);
    EXPECT_EQ(model.centerRow(), DomModel::Rows / 2);
}

TEST(DomModel, ExecutionsUseContainingBucketAndGridChangesRebucketTheRawRing) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.3, 7}});
    DomTradeWindow trades;
    trades.ingest(trade(200.09, AggressorSide::Buy));
    trades.ingest(trade(200.19, AggressorSide::Sell));
    trades.ingest(trade(std::numeric_limits<double>::infinity(), AggressorSide::Sell));
    trades.ingest(trade(std::numeric_limits<double>::quiet_NaN(), AggressorSide::Buy));
    DomModel model;
    model.publish(book, trades, true);
    EXPECT_EQ(count(model, rowAt(model, 2000), DomModel::Buys), 1);
    EXPECT_EQ(count(model, rowAt(model, 2001), DomModel::Sells), 1);
    book.initialize(100, 300, 0.2);
    apply(book, {{true, 200, 4}, {false, 200.4, 7}});
    model.publish(book, trades, false);
    EXPECT_EQ(count(model, rowAt(model, 1000), DomModel::Buys), 1);
    EXPECT_EQ(count(model, rowAt(model, 1000), DomModel::Sells), 1);
    int buyCount = 0, sellCount = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        buyCount += count(model, r, DomModel::Buys);
        sellCount += count(model, r, DomModel::Sells);
    }
    EXPECT_EQ(buyCount, 1);
    EXPECT_EQ(sellCount, 1);
}

TEST(DomModel, ServerQuantisedSnapshotsNeverAdvertiseFalsePrecision) {
    RemoteGridDataSource source{"127.0.0.1", "1"}; // No network: real production snapshot/delta slots.
    OrderBookDock dock(nullptr, &source);
    dock.resize(650, 600);
    dock.show();
    auto* timer = dock.findChild<QTimer*>("domPublishTimer");
    ASSERT_NE(timer, nullptr);
    timer->stop();
    QSignalSpy updates(&source, &IGridDataSource::liveOrderBookUpdated);
    auto* model = static_cast<DomModel*>(dock.findChild<QTableView*>()->model());
    QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    struct Case { const char* symbol; double bid, ask, tick; bool coarse; };
    // Wire prices after the server's 0.1 aggregation, including its zero-price PEPE levels.
    // Alternate valid/coarse products to prove old rows and spread disappear and recover.
    for (const auto& c : {Case{"BTC-USD", 84000, 84001, .1, false},
                          Case{"DOGE-USD", .1, .1, .1, true},
                          Case{"ETH-USD", 2500, 2500.5, .1, false},
                          Case{"FARTCOIN-USD", .7, .8, .1, true},
                          Case{"PEPE-USD", 0, 0, .1, true},
                          Case{"ETH-USD", 6000, 6001, .1, false},
                          Case{"DOGE-USD", .2, .2, .1, true}}) {
        SCOPED_TRACE(c.symbol);
        const QString symbol = c.symbol;
        source.subscribe(symbol);
        dock.onSymbolChanged(symbol);
        updates.clear();
        emit source.streamClient()->snapshotReceived(symbol, {{c.bid, 7}}, {{c.ask, 9}},
            c.tick, source.streamClient()->bookDeliveryGeneration(c.symbol));
        QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        const auto& book = source.getDirectLiveOrderBook(c.symbol);
        EXPECT_DOUBLE_EQ(book.getTickSize(), c.tick);
        // Other consumers still receive a usable replica and nonempty deltas, even for PEPE.
        ASSERT_EQ(updates.count(), 1);
        EXPECT_FALSE(qvariant_cast<std::vector<BookDelta>>(updates[0][1]).empty());
        EXPECT_FALSE(source.isBookSnapshotStale(symbol));
        EXPECT_EQ(book.getBidCount(), 1u);
        EXPECT_EQ(book.getAskCount(), 1u);
        EXPECT_DOUBLE_EQ(book.getBidVolume(), 7);
        EXPECT_DOUBLE_EQ(book.getAskVolume(), 9);
        ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        const auto aggregation = dock.findChild<QLabel*>("domAggregation")->toolTip().section('\n', 0, 0);
        const auto summary = dock.findChild<QLabel*>("domBookSummary")->toolTip().section('\n', 0, 0);
        if (c.coarse) {
            EXPECT_EQ(model->rowCount(), 0);
            EXPECT_EQ(aggregation, QString("Server aggregation 0.1 USD is too coarse for %1").arg(symbol));
            EXPECT_TRUE(summary.isEmpty());
            EXPECT_FALSE(dock.findChild<QLabel*>("domFreshness")->text().contains("Waiting for book"));
        } else {
            EXPECT_EQ(model->rowCount(), DomModel::Rows);
            EXPECT_TRUE(aggregation.startsWith("Aggregation:"));
            EXPECT_TRUE(summary.contains("Bucketed spread"));
        }
        // A subsequent level update keeps using the same replica, without a new snapshot/retry.
        emit source.streamClient()->l2UpdateReceived(symbol, {{true, c.bid, 11}},
            c.tick, source.streamClient()->bookDeliveryGeneration(c.symbol));
        QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        EXPECT_DOUBLE_EQ(book.getBidVolume(), 11);
        EXPECT_DOUBLE_EQ(book.getAskVolume(), 9);
        EXPECT_DOUBLE_EQ(book.getTickSize(), c.tick);
        source.unsubscribe(symbol);
    }
}

TEST(DomModel, CoarseOrUnavailableAggregationExplainsWhyThereIsNoLadder) {
    LiveOrderBook book;
    DomModel model;
    model.clear("TEST-USD");
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    DomTradeWindow trades;
    book.initialize(0, 200, 1);
    apply(book, {{true, 100, 7}, {false, 100, 9}});
    model.publish(book, trades, true);
    EXPECT_EQ(model.rowCount(), DomModel::Rows); // Exactly 1% remains permitted.
    apply(book, {{true, 100, 0}, {false, 100, 0}, {true, 99, 7}, {false, 99, 9}});
    model.publish(book, trades, true);
    EXPECT_EQ(model.rowCount(), 0); // Same product loses rows when its tick becomes too coarse.
    EXPECT_TRUE(model.aggregation().contains("too coarse for TEST-USD"));
    EXPECT_TRUE(model.summary().isEmpty());
    book.initialize(0, .1, .1);
    apply(book, {{true, 0, 7}, {false, 0, 9}});
    model.publish(book, trades, true);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_TRUE(model.aggregation().contains("Server aggregation 0.1 USD"));
    book.clear();
    model.publish(book, trades, true);
    EXPECT_TRUE(model.aggregation().contains("unavailable"));
    EXPECT_TRUE(model.summary().isEmpty());
    book.initialize(0, 200, 1);
    apply(book, {{true, 100, 7}, {false, 101, 9}});
    model.publish(book, trades, true);
    EXPECT_EQ(model.rowCount(), DomModel::Rows);
    EXPECT_TRUE(model.summary().contains("Bucketed spread"));
}

TEST(DomModel, EthReplicaPreservesAdjacentServerLevelsAndBookTop) {
    RemoteGridDataSource source{"127.0.0.1", "1"};
    // Adjacent 0.1 levels on each side must survive at both ETH price scales.
    // The chart book-top and Agent API consume this same dense replica view.
    for (const auto& prices : {std::array{2500.3, 2500.4, 2500.8, 2500.9},
                               std::array{6000.0, 6000.1, 6000.5, 6000.6}}) {
        SCOPED_TRACE(prices[1]);
        source.subscribe("ETH-USD");
        const auto generation = source.streamClient()->bookDeliveryGeneration("ETH-USD");
        emit source.streamClient()->snapshotReceived("ETH-USD",
            {{prices[0], 3}, {prices[1], 7}}, {{prices[2], 9}, {prices[3], 5}}, .1, generation);
        QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
        const auto& book = source.getDirectLiveOrderBook("ETH-USD");
        EXPECT_DOUBLE_EQ(book.getTickSize(), .1);
        std::vector<std::pair<uint32_t, double>> bids, asks;
        const auto view = book.captureDenseNonZero(bids, asks, 10);
        ASSERT_EQ(view.bidLevels.size(), 2u);
        ASSERT_EQ(view.askLevels.size(), 2u);
        const auto priceAt = [&](uint32_t index) { return view.minPrice + index * view.tickSize; };
        EXPECT_NEAR(priceAt(bids[0].first), prices[1], 1e-9);
        EXPECT_NEAR(priceAt(bids[1].first), prices[0], 1e-9);
        EXPECT_NEAR(priceAt(asks[0].first), prices[2], 1e-9);
        EXPECT_NEAR(priceAt(asks[1].first), prices[3], 1e-9);
        EXPECT_DOUBLE_EQ(bids[0].second, 7);
        EXPECT_DOUBLE_EQ(bids[1].second, 3);
        EXPECT_DOUBLE_EQ(asks[0].second, 9);
        EXPECT_DOUBLE_EQ(asks[1].second, 5);
        const auto api = AgentApi::captureBook(book, 10, 1000, {});
        ASSERT_EQ(api.bids.size(), 2u);
        ASSERT_EQ(api.asks.size(), 2u);
        EXPECT_EQ(api.bestBid, prices[1]);
        EXPECT_EQ(api.bestAsk, prices[2]);
        EXPECT_DOUBLE_EQ(api.bids[1].price, prices[0]);
        EXPECT_DOUBLE_EQ(api.asks[1].price, prices[3]);
        emit source.streamClient()->l2UpdateReceived("ETH-USD", {{true, prices[0], 4}}, .1, generation);
        QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
        EXPECT_EQ(book.getBidCount(), 2u);
        EXPECT_DOUBLE_EQ(book.getBidVolume(), 11);
        source.unsubscribe("ETH-USD");
    }
}

TEST(DomModel, BookWireVersionFencesOlderReadyAndUnavailableEvents) {
    RemoteGridDataSource source{"127.0.0.1", "1"};
    source.subscribe("DOGE-USD");
    const auto generation = source.streamClient()->bookDeliveryGeneration("DOGE-USD");
    auto snapshot = [&](double bid, double ask, double tick, uint64_t version,
                        const QString& status = QStringLiteral("ready")) {
        emit source.streamClient()->snapshotReceived("DOGE-USD", {{bid, 1}}, {{ask, 2}},
                                                     tick, generation, status, version);
        QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    };
    emit source.streamClient()->l2UpdateReceived("DOGE-USD", {{true, .20002, 3}},
                                                 .00002, generation, 8);
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    snapshot(.20000, .20006, .00002, 7); // Missing delta forbids an earlier snapshot.
    EXPECT_EQ(source.getDirectLiveOrderBook("DOGE-USD").getTickSize(), 0.0);
    snapshot(.20000, .20006, .00002, 8); // Complete snapshot at the fenced version recovers.
    EXPECT_DOUBLE_EQ(source.getDirectLiveOrderBook("DOGE-USD").getBidVolume(), 1.0);
    snapshot(.20000, .20006, .00002, 10);
    const auto& book = source.getDirectLiveOrderBook("DOGE-USD");
    EXPECT_DOUBLE_EQ(book.getTickSize(), .00002);
    DomTradeWindow trades;
    DomModel display;
    display.publish(book, trades, true);
    ASSERT_GT(display.rowCount(), 0);
    int withdrawals = 0;
    QObject::connect(&source, &IGridDataSource::liveOrderBookUpdated, &source,
        [&](const QString& symbol, const std::vector<BookDelta>& deltas) {
            if (symbol != "DOGE-USD") return;
            display.publish(book, trades, true);
            if (book.getTickSize() == 0.0) {
                EXPECT_TRUE(deltas.empty());
                EXPECT_EQ(display.rowCount(), 0);
                ++withdrawals;
            }
        });
    snapshot(.1, .2, .1, 9); // Older queued snapshot after the current subscribe snapshot.
    EXPECT_DOUBLE_EQ(book.getTickSize(), .00002);
    snapshot(.1, .2, .1, 10); // Duplicate version cannot replace an already ready book.
    EXPECT_DOUBLE_EQ(book.getTickSize(), .00002);
    emit source.streamClient()->l2UpdateReceived("DOGE-USD", {{true, .20002, 3}},
                                                 .00002, generation, 11);
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    EXPECT_EQ(book.getBidCount(), 2u);
    emit source.streamClient()->l2UpdateReceived("DOGE-USD", {{true, .20004, 4}},
                                                 .00002, generation, 10);
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    EXPECT_EQ(book.getBidCount(), 2u);
    snapshot(0, 0, 0, 12, QStringLiteral("invalidated"));
    EXPECT_EQ(book.getTickSize(), 0.0);
    EXPECT_EQ(withdrawals, 1);
    snapshot(.20000, .20006, .00002, 11); // Cannot repopulate after invalidation.
    EXPECT_EQ(book.getTickSize(), 0.0);
    snapshot(.20000, .20006, .00002, 13);
    EXPECT_DOUBLE_EQ(book.getTickSize(), .00002);
    emit source.streamClient()->l2UpdateReceived("DOGE-USD", {{true, .20002, 3}},
                                                 .1, generation, 14);
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    EXPECT_EQ(book.getTickSize(), 0.0);
    EXPECT_EQ(withdrawals, 2);
    snapshot(.20000, .20006, .00002, 13); // Older ready event cannot restore a cleared book.
    EXPECT_EQ(book.getTickSize(), 0.0);
    snapshot(.20000, .20006, .00002, 14); // Retry at the current version is complete.
    EXPECT_DOUBLE_EQ(book.getBidVolume(), 1.0);
    EXPECT_GT(display.rowCount(), 0);
    emit source.streamClient()->l2UpdateReceived("DOGE-USD", {{true, .20002, 3}},
                                                 .00002, generation, 15);
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    EXPECT_EQ(book.getBidCount(), 2u);
    source.unsubscribe("DOGE-USD");
}

TEST(DomModel, ExecutionsMatchReplicaTruncationAtDecimalBoundariesWithNonzeroOrigin) {
    LiveOrderBook book;
    book.initialize(90.3, 130, .1);
    const double price = 100.1;
    apply(book, {{true, price, 7}, {false, 100.5, 9}});
    DomTradeWindow trades;
    trades.ingest(trade(price, AggressorSide::Buy));
    DomModel model;
    model.publish(book, trades, true);
    int hits = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.index(r, DomModel::Bid).data(DomModel::QuantityRole).toDouble() > 0) {
            ++hits;
            EXPECT_EQ(count(model, r, DomModel::Buys), 1);
        }
    }
    EXPECT_EQ(hits, 1);
}

TEST(DomModel, LockedAndCrossedBooksRetainUniqueSideRows) {
    for (const double ask : {200., 199.}) {
        LiveOrderBook book;
        book.initialize(0, 400, .1);
        apply(book, {{true, 200, 7}, {false, ask, 9}});
        DomModel model;
        model.publish(book, {}, true);
        int bids = 0, asks = 0;
        for (int r = 0; r < model.rowCount(); ++r) {
            const auto side = model.index(r, 0).data(DomModel::BestSideRole).toString();
            bids += side.contains("Bid"); asks += side.contains("Ask");
        }
        EXPECT_EQ(bids, 1); EXPECT_EQ(asks, 1);
        EXPECT_TRUE(model.summary().contains(ask == 200 ? "spread 0.0" : "spread -1.0"));
    }
}

TEST(DomFreshness, LiveQuietBookHasChangeAgeButDoesNotBecomeStale) {
    DomFreshness state;
    EXPECT_TRUE(state.text(10000).startsWith("Waiting"));
    state.lastChangeMs = 10000;
    state.connected = true;
    EXPECT_EQ(state.text(13400), "Connected · last change 3.4 s");
    EXPECT_TRUE(state.text(1000000).startsWith("Connected"));
    state.connected = false;
    EXPECT_TRUE(state.text(1000000).startsWith("Disconnected"));
    state.connected = true;
    state.awaitingBook = true;
    EXPECT_TRUE(state.text(1000000).startsWith("Waiting"));
    state.awaitingBook = false;
    state.snapshotStale = true;
    EXPECT_TRUE(state.text(1000000).startsWith("Stale snapshot"));
}

class Source : public IGridDataSource {
public:
    LiveOrderBook book;
    mutable int reads = 0;
    QString staleSymbol;
    bool isBookSnapshotStale(const QString& symbol) const override { return symbol == staleSymbol; }
    Source() { book.initialize(0, 500, 0.1); }
    void update(std::initializer_list<BookLevelUpdate> updates, qint64 at = 0) {
        apply(book, updates, at);
        emit liveOrderBookUpdated("BTC-USD", {});
    }
    void send(const Trade& value) { emit tradeReceived(value); }
    const LiveOrderBook& getDirectLiveOrderBook(const std::string&) const override { ++reads; return book; }
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
    void requestHeatmapHistory(const QString&, int64_t, int64_t, int) override {}
    void registerRecordingView(const recording::LiveView&) override {}
    void requestRecordingHeatmapHistory(const protocol::recordingwire::Request&) override {}
    void requestFootprintHistory(const QString&, int64_t, int64_t, int) override {}
    void requestTpoHistory(const QString&, int64_t, int, int64_t, int, const QString&) override {}
    void cancelTpoHistory(const QString&, const QString&) override {}
    void setCandleHistoryViewport(const QString&, int64_t, qint64, qint64) override {}
    void sendTradeCommand(const trading::TradeCommand&) override {}
    void sendAlgoCommand(const std::string&, const std::string&, const std::string&, const trading::AlgoParams&) override {}
};

struct DomDock : testing::Test {
    Source source;
    OrderBookDock dock{nullptr, &source};
    QTableView* table = dock.findChild<QTableView*>("domLadder");
    DomModel* model = static_cast<DomModel*>(table->model());
    void SetUp() override { dock.resize(650, 600); dock.onSymbolChanged("BTC-USD"); }
    void show() { dock.show(); QTest::qWait(90); }
    void flushEvents() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
    QString label(const char* name) { auto* label = dock.findChild<QLabel*>(name); return QString(name) == "domFreshness" ? label->toolTip().section('\n', 0, 0) : label->text(); }
};

TEST_F(DomDock, NarrowLadderFitsWithoutScrollingAndRestoresExecutionColumns) {
    show();
    for (int width : {650, 260, 200, 180, 650}) {
        dock.resize(width, 600);
        QCoreApplication::processEvents();
        ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection));
        EXPECT_EQ(dock.width(), width);
        EXPECT_EQ(table->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        EXPECT_EQ(table->horizontalScrollBar()->maximum(), 0);
        EXPECT_EQ(table->horizontalScrollBar()->value(), 0);
        EXPECT_LE(table->horizontalHeader()->length(), table->viewport()->width());
        for (int col : {DomModel::Bid, DomModel::Price, DomModel::Ask}) {
            EXPECT_FALSE(table->isColumnHidden(col));
            EXPECT_GE(table->columnViewportPosition(col), 0);
            EXPECT_LE(table->columnViewportPosition(col) + table->columnWidth(col), table->viewport()->width());
        }
        EXPECT_EQ(table->isColumnHidden(DomModel::Buys), width < 300);
        EXPECT_EQ(table->isColumnHidden(DomModel::Delta), width <= 200);
        const auto* aggregation = dock.findChild<QLabel*>("domAggregation");
        const auto* summary = dock.findChild<QLabel*>("domBookSummary");
        const auto* freshness = dock.findChild<QLabel*>("domFreshness");
        EXPECT_FALSE(aggregation->wordWrap());
        EXPECT_EQ(summary->y(), freshness->y());
        EXPECT_LT(table->y(), 110); // heading + two metadata lines, not a seven-line paragraph.
        EXPECT_TRUE(aggregation->text().contains("USD"));
        EXPECT_TRUE(aggregation->text().contains("BTC"));
        EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("last 1,000 trades"));
        EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("unknown"));
    }
}

TEST(DomDelegate, CompactNumbersAreWholeValuesAndPricesKeepTheirPrecision) {
    QFont font = QApplication::font();
    QFontMetrics fm(font);
    const int width = fm.horizontalAdvance("0.0353");
    EXPECT_EQ(DomBarDelegate::fittedQuantity(.035294567, fm, width), "0.0353");
    EXPECT_EQ(DomBarDelegate::fittedQuantity(1234.567, fm, fm.horizontalAdvance("1.23k")), "1.23k");
    for (double value : {0.000000034567, 12.345678, 12345678.9}) {
        const auto text = DomBarDelegate::fittedQuantity(value, fm, width);
        EXPECT_FALSE(text.contains(QChar(0x2026)));
        EXPECT_LE(fm.horizontalAdvance(text), width);
        EXPECT_NE(text, "0");
    }
}

TEST_F(DomDock, HiddenIngestsAllTradesButDoesNoModelOrReplicaReadWork) {
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
    for (int i = 0; i < 1500; ++i) {
        source.send(trade(200, i < 1400 ? AggressorSide::Buy : AggressorSide::Unknown));
        source.update({{true, 200, double(i + 1)}, {false, 200.1, 1}});
    }
    QTest::qWait(160);
    EXPECT_EQ(source.reads, 0);
    EXPECT_EQ(changed.count() + reset.count() + inserted.count(), 0);
    show();
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Buys), 900);
    EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("unknown: 100"));
    const int reads = source.reads;
    dock.hide();
    const int notifications = changed.count() + reset.count() + inserted.count();
    source.send(trade(200, AggressorSide::Sell));
    source.update({{true, 200, 9999}});
    QTest::qWait(160);
    EXPECT_EQ(source.reads, reads);
    EXPECT_EQ(changed.count() + reset.count() + inserted.count(), notifications);
    show();
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Sells), 1);
    EXPECT_DOUBLE_EQ(model->index(rowAt(*model, 2000), DomModel::Bid).data(DomModel::QuantityRole).toDouble(), 9999);
}

TEST_F(DomDock, ScrollPinsPriceAndPositionThenButtonRestoresFollow) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    QWheelEvent wheel(QPointF(20, 20), table->viewport()->mapToGlobal(QPoint(20, 20)),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(table->viewport(), &wheel);
    const int scroll = table->verticalScrollBar()->value();
    const auto top = model->index(0, 0).data(DomModel::BucketRole);
    source.update({{true, 200, 0}, {false, 200.1, 0}, {true, 210, 1}, {false, 210.1, 1}});
    QTest::qWait(90);
    EXPECT_EQ(table->verticalScrollBar()->value(), scroll);
    EXPECT_EQ(model->index(0, 0).data(DomModel::BucketRole), top);
    dock.findChild<QPushButton*>("domRecenter")->click();
    EXPECT_NE(model->index(0, 0).data(DomModel::BucketRole), top);
    EXPECT_NEAR(table->visualRect(model->index(model->centerRow(), 0)).center().y(), table->viewport()->height() / 2, 22);
    EXPECT_TRUE(dock.findChild<QPushButton*>("domRecenter")->text().contains("Following"));
    const auto recenteredTop = model->index(0, 0).data(DomModel::BucketRole);
    source.update({{true, 210, 0}, {false, 210.1, 0}, {true, 220, 1}, {false, 220.1, 1}});
    QTest::qWait(90);
    EXPECT_NE(model->index(0, 0).data(DomModel::BucketRole), recenteredTop);
}

TEST_F(DomDock, VisibleUpdatesCoalesceAtFifteenHz) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    auto* timer = dock.findChild<QTimer*>("domPublishTimer");
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->interval(), 67);
    timer->stop(); // Deliver real timeout signals synchronously; no scheduler timing assertions.
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
    for (int batch = 0; batch < 25; ++batch) {
        const int before = changed.count();
        for (int i = 0; i < 100; ++i) {
            source.send(trade(200, AggressorSide::Buy));
            source.update({{true, 200, double(batch * 100 + i + 2)}});
        }
        flushEvents();
        EXPECT_EQ(changed.count(), before) << "No publication between timer ticks";
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        EXPECT_EQ(changed.count(), before + 1) << "One publication per timer tick";
    }
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Buys), 1000);
}

TEST_F(DomDock, QuietConnectedBookRetainsLastChangeAgeAndDetectsDisconnect) {
    // Connection signal predates symbol switch; the datasource retains its state.
    emit source.connectionStatusChanged(true);
    dock.onSymbolChanged("ETH-USD");
    dock.onSymbolChanged("BTC-USD");
    const qint64 changedAt = QDateTime::currentMSecsSinceEpoch() - 60000;
    source.update({{true, 200, 2}, {false, 200.1, 3}}, changedAt);
    show();
    EXPECT_TRUE(label("domFreshness").startsWith("Connected · last change "));
    EXPECT_GE(label("domFreshness").section("last change ", 1).section(" s", 0, 0).toDouble(), 60.0);
    auto* timer = dock.findChild<QTimer*>("domPublishTimer");
    timer->stop();
    for (int i = 0; i < 100; ++i) {
        // Like the real replica: identical updates advance getLastUpdate but
        // produce no deltas / liveOrderBookUpdated signal.
        apply(source.book, {{true, 200, 2}, {false, 200.1, 3}});
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    }
    EXPECT_TRUE(label("domFreshness").startsWith("Connected · last change "));
    EXPECT_GE(label("domFreshness").section("last change ", 1).section(" s", 0, 0).toDouble(), 60.0);
    emit source.connectionStatusChanged(false);
    flushEvents();
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    EXPECT_TRUE(label("domFreshness").startsWith("Disconnected"));
    emit source.connectionStatusChanged(true);
    flushEvents();
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    EXPECT_TRUE(label("domFreshness").startsWith("Waiting"));
    source.update({{true, 200, 3}});
    flushEvents();
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    EXPECT_TRUE(label("domFreshness").startsWith("Connected"));
}

TEST_F(DomDock, NonUserScrollDoesNotStopFollowingAndSymbolSwitchRereadsStaleSnapshot) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    table->verticalScrollBar()->setValue(table->verticalScrollBar()->value() - 220);
    dock.resize(650, 450);
    const auto top = model->index(0, 0).data(DomModel::BucketRole);
    source.update({{true, 200, 0}, {false, 200.1, 0}, {true, 210, 1}, {false, 210.1, 1}});
    flushEvents();
    QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection);
    EXPECT_NE(model->index(0, 0).data(DomModel::BucketRole), top);
    source.staleSymbol = "ETH-USD";
    dock.onSymbolChanged("ETH-USD");
    QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection);
    EXPECT_TRUE(label("domFreshness").startsWith("Stale snapshot"));
}

TEST(DomWindow, MinimizedMainWindowStopsTimerAndRetainsTradeIngestion) {
    Source source;
    QMainWindow window;
    auto* dock = new OrderBookDock(&window, &source);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    dock->onSymbolChanged("BTC-USD");
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    window.resize(650, 600);
    window.show();
    QTest::qWait(90);
    auto* timer = dock->findChild<QTimer*>("domPublishTimer");
    ASSERT_TRUE(timer->isActive());
    window.setWindowState(Qt::WindowMinimized);
    EXPECT_FALSE(timer->isActive());
    const int reads = source.reads;
    source.send(trade(200, AggressorSide::Sell));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QMetaObject::invokeMethod(dock, "refreshDisplay", Qt::DirectConnection);
    EXPECT_EQ(source.reads, reads);
    window.setWindowState(Qt::WindowNoState);
    window.show();
    QTest::qWait(90);
    EXPECT_TRUE(timer->isActive());
    auto* model = static_cast<DomModel*>(dock->findChild<QTableView*>("domLadder")->model());
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Sells), 1);
}

TEST_F(DomDock, TabVisibilityStopsPublicationWithoutLosingEvents) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    // QDockWidget emits this when another tab covers it (isVisible stays true).
    emit dock.visibilityChanged(false);
    const int reads = source.reads;
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
    source.send(trade(200, AggressorSide::Sell));
    source.update({{true, 200, 91}});
    QTest::qWait(160);
    EXPECT_EQ(source.reads, reads);
    EXPECT_EQ(changed.count(), 0);
    emit dock.visibilityChanged(true);
    QTest::qWait(90);
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Sells), 1);
}

TEST_F(DomDock, SymbolChangeWhileHiddenClearsOldRowsAndCountsOnShow) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    source.send(trade(200, AggressorSide::Buy));
    show();
    dock.hide();
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    dock.onSymbolChanged("ETH-USD");
    source.send(trade(200, AggressorSide::Buy));
    QTest::qWait(90);
    EXPECT_EQ(reset.count(), 0);
    show();
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Buys), 0);
    EXPECT_TRUE(model->headerData(DomModel::Bid, Qt::Horizontal, Qt::ToolTipRole).toString().contains("ETH"));
}

// Opt-in measurement: sustained wall-clock 10,000 books + 10,000 trades/s,
// queued through the real dock slots, ~15 Hz model + QWidget raster painting.
TEST_F(DomDock, DISABLED_SustainedUpdateAndRasterPaintBenchmark) {
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme{}.stylesheet());
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    QImage image(dock.size(), QImage::Format_ARGB32_Premultiplied);
    std::vector<double> timings;
    QElapsedTimer elapsed;
    elapsed.start();
    int events = 0;
    qint64 nextFrame = 67;
    while (elapsed.elapsed() < 5000) {
        const int target = int(elapsed.elapsed() * 10);
        while (events < target) {
            source.send(trade(200 + (events % 20) * 0.1, events % 3 == 0 ? AggressorSide::Sell : AggressorSide::Buy));
            source.update({{true, 200, double(events % 1000 + 1)}, {false, 200.1, double(events % 997 + 1)}});
            ++events;
        }
        flushEvents(); // ingests every queued event; no model publication here
        if (elapsed.elapsed() >= nextFrame) {
            QElapsedTimer cost;
            cost.start();
            QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection);
            QPainter painter(&image);
            dock.render(&painter);
            painter.end();
            timings.push_back(cost.nsecsElapsed() / 1e6);
            nextFrame += 67;
        }
        QThread::msleep(1);
    }
    int total = 0;
    for (int r = 0; r < model->rowCount(); ++r) total += count(*model, r, DomModel::Buys) + count(*model, r, DomModel::Sells);
    EXPECT_EQ(total, DomTradeWindow::Capacity);
    std::sort(timings.begin(), timings.end());
    ASSERT_GT(timings.size(), 60u);
    std::cout << "DOM_BENCH platform=" << QGuiApplication::platformName().toStdString()
              << " events_per_kind=" << events << " duration_ms=" << elapsed.elapsed()
              << " frames=" << timings.size() << " update_plus_raster_paint_ms mean="
              << std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size()
              << " p50=" << timings[timings.size()/2] << " p95=" << timings[timings.size()*95/100]
              << " max=" << timings.back() << '\n';
    qApp->setStyleSheet(previousStyle);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
