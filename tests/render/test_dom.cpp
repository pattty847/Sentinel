#include "models/DomModel.hpp"
#include "themes/DarkTheme.hpp"
#include "widgets/OrderBookDock.hpp"
#include "datasources/IGridDataSource.hpp"
#include <QAbstractItemModelTester>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
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
    EXPECT_TRUE(model.headerData(DomModel::Bid, Qt::Horizontal, Qt::DisplayRole).toString().contains("BTC"));
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
        EXPECT_TRUE(model.headerData(c, Qt::Horizontal, Qt::DisplayRole).toString().contains("count"));
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
    DomModel model;
    model.publish(book, trades, true);
    EXPECT_EQ(count(model, rowAt(model, 2000), DomModel::Buys), 1);
    EXPECT_EQ(count(model, rowAt(model, 2001), DomModel::Sells), 1);
    book.initialize(100, 300, 0.2);
    apply(book, {{true, 200, 4}, {false, 200.4, 7}});
    model.publish(book, trades, false);
    EXPECT_EQ(count(model, rowAt(model, 1000), DomModel::Buys), 1);
    EXPECT_EQ(count(model, rowAt(model, 1000), DomModel::Sells), 1);
}

TEST(DomModel, LowPricePrecisionAndInvalidTradeSafety) {
    LiveOrderBook book;
    book.initialize(0, 0.000004, 0.000000001);
    apply(book, {{true, 0.000002, 1}, {false, 0.000002001, 1}});
    DomModel model;
    DomTradeWindow trades;
    trades.ingest(trade(std::numeric_limits<double>::infinity(), AggressorSide::Sell));
    trades.ingest(trade(std::numeric_limits<double>::quiet_NaN(), AggressorSide::Buy));
    model.publish(book, trades, true);
    EXPECT_EQ(model.index(rowAt(model, 2000), DomModel::Price).data().toString(), "0.000002000");
    EXPECT_EQ(count(model, rowAt(model, 2000), DomModel::Buys), 0);
    EXPECT_EQ(DomModel::priceText(0.0000000001, 0.0000000001), "0.0000000001");
}

TEST(DomFreshness, ReceiveAgeStaleDisconnectedAndReconnect) {
    DomFreshness state;
    EXPECT_TRUE(state.text(10000).startsWith("Waiting"));
    state.receiveMs = 10000;
    EXPECT_TRUE(state.text(10001).startsWith("Recent"));
    EXPECT_TRUE(state.text(13000).startsWith("Stale"));
    state.connected = false;
    EXPECT_TRUE(state.text(10001).startsWith("Disconnected"));
    state.connected = true;
    state.awaitingBook = true;
    EXPECT_TRUE(state.text(10001).startsWith("Waiting"));
    state.awaitingBook = false;
    state.snapshotStale = true;
    EXPECT_TRUE(state.text(10001).startsWith("Stale snapshot"));
}

class Source : public IGridDataSource {
public:
    LiveOrderBook book;
    mutable int reads = 0;
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
    QString label(const char* name) { return dock.findChild<QLabel*>(name)->text(); }
};

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
    EXPECT_TRUE(label("domExecutions").contains("unknown: 100"));
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
    table->verticalScrollBar()->setValue(table->verticalScrollBar()->value() - 220);
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

TEST_F(DomDock, VisibleUpdatesCoalesceAtFifteenHzAndFreshnessUsesReceiveTime) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
    for (int batch = 0; batch < 25; ++batch) {
        for (int i = 0; i < 100; ++i) {
            source.send(trade(200, AggressorSide::Buy));
            source.update({{true, 200, double(batch * 100 + i + 1)}});
        }
        QTest::qWait(10);
    }
    EXPECT_GT(changed.count(), 0);
    EXPECT_LT(changed.count(), 12);
    QTest::qWait(90);
    EXPECT_EQ(count(*model, rowAt(*model, 2000), DomModel::Buys), 1000);
    source.update({{true, 200, 2}}, QDateTime::currentMSecsSinceEpoch() - 5000);
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Stale"));
    source.send(trade(200, AggressorSide::Buy));
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Stale"));
    emit source.connectionStatusChanged(false);
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Disconnected"));
    emit source.connectionStatusChanged(true);
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Waiting"));
    source.update({{true, 200, 3}});
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Recent"));
    emit source.bookSnapshotStaleChanged("BTC-USD", true);
    QTest::qWait(90);
    EXPECT_TRUE(label("domFreshness").startsWith("Stale snapshot"));
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
    EXPECT_TRUE(model->headerData(DomModel::Bid, Qt::Horizontal, Qt::DisplayRole).toString().contains("ETH"));
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
