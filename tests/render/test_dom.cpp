#include "models/DomModel.hpp"
#include "models/DomGrouping.hpp"
#include "mainwindow/AgentApiSnapshots.hpp"
#include "themes/DarkTheme.hpp"
#include "widgets/OrderBookDock.hpp"
#include "widgets/LeanGaugeWidget.hpp"
#include "render/heatmap/HeatmapPalette.hpp"
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
#include <QMenu>
#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTableView>
#include <QToolButton>
#include <QTest>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>

// Allocation counter for the pulse publish/step assertions (test binary only).
namespace {
std::atomic<bool> gCounting{false};
std::atomic<size_t> gAllocations{0};
void* counted(std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
} // namespace
void* operator new(std::size_t n) { return counted(n); }
void* operator new[](std::size_t n) { return counted(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { if (gCounting) gAllocations++; return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { if (gCounting) gAllocations++; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// Qt containers (QString, QArrayData) allocate through malloc, which the operator new counter
// cannot see. On macOS libmalloc calls the exported malloc_logger hook (the one malloc stack
// logging uses) for every allocation and reallocation in the process, whichever image or zone made
// it: Qt frameworks, libc++ and this executable alike. Counted on the main thread only. It does
// not see stack memory or memory a library obtains with mmap/vm_allocate instead of malloc.
#if defined(__APPLE__)
#include <pthread.h>
using MallocLogger = void(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t result, uint32_t);
extern "C" MallocLogger* malloc_logger;
namespace {
std::atomic<size_t> gMallocs{0};
void countMallocs(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t) {
    constexpr uint32_t allocate = 2; // MALLOC_LOG_TYPE_ALLOCATE, also set on realloc
    if ((type & allocate) && gCounting.load(std::memory_order_relaxed) && pthread_main_np())
        gMallocs.fetch_add(1, std::memory_order_relaxed);
}
struct MallocHook {
    MallocHook() { malloc_logger = &countMallocs; }
} gMallocHook;
} // namespace
constexpr bool kMallocCounted = true;
#else
namespace { std::atomic<size_t> gMallocs{0}; }
constexpr bool kMallocCounted = false;
#endif

namespace {
Trade trade(double price, AggressorSide side, double size = 1) {
    return {std::chrono::system_clock::now(), "BTC-USD", "", side, price, size};
}
Trade tradeAt(qint64 timeMs, double price, AggressorSide side, double size) {
    return {std::chrono::system_clock::time_point(std::chrono::milliseconds(timeMs)), "BTC-USD", "", side, price, size};
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
// Full traded volume (base units) behind a Buy/Sell/Delta cell.
double vol(DomModel& model, int row, int col) { return model.index(row, col).data(DomModel::QuantityRole).toDouble(); }

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
    EXPECT_TRUE(model.executionSummary().contains("1 trades"));
    apply(book, {{true, 200, 1}, {false, 201, 1}});
    model.publish(book, trades, true);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.publish(book, trades, true);
    EXPECT_EQ(changed.count(), 0);
    apply(book, {{true, 200, 0}, {false, 201, 0}});
    model.publish(book, trades, false);
    EXPECT_EQ(model.rowCount(), DomModel::Rows);
    EXPECT_EQ(model.index(rowAt(model, 2000), DomModel::Bid).data(DomModel::QuantityRole).toDouble(), 0);
    EXPECT_EQ(vol(model, rowAt(model, 2000), DomModel::Buys), 1);
    EXPECT_TRUE(model.summary().contains("Bid — · Ask —"));
}

TEST(DomModel, TradesAreVolumeOverTheTrailingMinuteByTradeTimeUnknownNeverSells) {
    const qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    for (int i = 0; i < 3; ++i) trades.ingest(tradeAt(now - 10000, 200, AggressorSide::Buy, .5));
    trades.ingest(tradeAt(now - 59000, 200, AggressorSide::Sell, .25));
    trades.ingest(tradeAt(now - 61000, 200, AggressorSide::Sell, 100)); // outside the minute
    trades.ingest(tradeAt(now - 1000, 200, AggressorSide::Unknown, 7));
    trades.ingest(tradeAt(0, 200, AggressorSide::Buy, 9)); // no exchange time: never windowed
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.clear("BTC-USD");
    model.publish(book, trades, true, now);
    const int r = rowAt(model, 2000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Buys), 1.5);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Sells), .25);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Delta), 1.25);
    EXPECT_EQ(model.index(r, DomModel::Buys).data().toString(), "1.5");
    EXPECT_EQ(model.index(r, DomModel::Sells).data().toString(), ".25");
    EXPECT_EQ(model.index(r, DomModel::Delta).data().toString(), "+1.25");
    const auto summary = model.executionSummary();
    EXPECT_TRUE(summary.contains("Traded volume in BTC · last 60 s by trade time · 5 trades")) << summary.toStdString();
    EXPECT_TRUE(summary.contains("unknown side: 1"));
    EXPECT_TRUE(summary.contains("1 without trade time"));
    EXPECT_FALSE(summary.contains("ring-limited"));
    for (int c = DomModel::Buys; c < DomModel::Columns; ++c) {
        EXPECT_TRUE(model.headerData(c, Qt::Horizontal, Qt::ToolTipRole).toString().contains("Traded volume"));
        EXPECT_TRUE(model.headerData(c, Qt::Horizontal, Qt::DisplayRole).toString().contains("vol"));
    }
    // The sell at now-59 s leaves the window first; the model asks to republish then.
    EXPECT_FALSE(model.needsRepublish(now + 900));
    EXPECT_TRUE(model.needsRepublish(now + 1000));
    model.publish(book, trades, true, now + 1000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Sells), 0);
    EXPECT_EQ(model.index(r, DomModel::Delta).data().toString(), "+1.5");
    model.publish(book, trades, true, now + 51000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Buys), 0);
    EXPECT_TRUE(model.index(r, DomModel::Delta).data().toString().isEmpty());
}

TEST(DomModel, RingEvictsOldestAndLabelsAWindowItCannotCover) {
    const qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    for (int i = 0; i < 100; ++i) trades.ingest(tradeAt(now - 5000, 200, AggressorSide::Buy, 1000));
    for (int i = 0; i < DomTradeWindow::Capacity; ++i) trades.ingest(tradeAt(now - 4000, 200, AggressorSide::Sell, .001));
    EXPECT_EQ(trades.size(), DomTradeWindow::Capacity);
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.publish(book, trades, true, now);
    const int r = rowAt(model, 2000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Buys), 0);
    EXPECT_NEAR(vol(model, r, DomModel::Sells), DomTradeWindow::Capacity * .001, 1e-9);
    const auto summary = model.executionSummary();
    EXPECT_TRUE(summary.contains("ring-limited: the last 8,192 trades cover only 5.0 s")) << summary.toStdString();
    EXPECT_FALSE(summary.contains("without trade time"));
}

TEST(DomModel, FutureStampedTradesWaitForTheirTimeAndCountOnce) {
    const qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    trades.ingest(tradeAt(now + 500, 200, AggressorSide::Buy, 1));        // within the skew tolerance
    trades.ingest(tradeAt(now + 300000, 200, AggressorSide::Sell, 4));    // five minutes ahead
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.clear("BTC-USD");
    model.publish(book, trades, true, now);
    const int r = rowAt(model, 2000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Buys), 1);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Sells), 0);
    EXPECT_TRUE(model.executionSummary().contains("1 trades"));
    EXPECT_TRUE(model.executionSummary().contains("1 stamped more than 1 s ahead"));
    EXPECT_TRUE(model.needsRepublish(now + 60500)); // the near-future buy leaves 60 s after its stamp
    model.publish(book, trades, true, now + 60500);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Buys), 0);
    EXPECT_FALSE(model.needsRepublish(now + 298000));
    EXPECT_TRUE(model.needsRepublish(now + 299000)); // the far one enters at its stamp minus the tolerance
    model.publish(book, trades, true, now + 299000);
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Sells), 4);
    model.publish(book, trades, true, now + 300000 + DomTradeWindow::WindowMs); // and leaves 60 s after its stamp
    EXPECT_DOUBLE_EQ(vol(model, r, DomModel::Sells), 0);
    EXPECT_TRUE(DomTradeWindow::inWindow(now + DomTradeWindow::SkewMs, now));
    EXPECT_FALSE(DomTradeWindow::inWindow(now + DomTradeWindow::SkewMs + 1, now));
    EXPECT_FALSE(DomTradeWindow::inWindow(now - DomTradeWindow::WindowMs, now));
}

TEST(DomModel, RingCoverageWarningSurvivesUntimedAndOutOfOrderHeads) {
    const qint64 now = 1'800'000'000'000;
    for (const qint64 headTime : {qint64(0), now - 3'600'000}) { // untimed head, then an hour-old late arrival
        SCOPED_TRACE(headTime);
        DomTradeWindow trades;
        for (int i = 0; i < 10; ++i) trades.ingest(tradeAt(now - 2000, 200, AggressorSide::Sell, 100)); // evicted below
        trades.ingest(tradeAt(headTime, 200, AggressorSide::Buy, 1));
        for (int i = 0; i < DomTradeWindow::Capacity - 1; ++i) trades.ingest(tradeAt(now - 1000, 200, AggressorSide::Buy, .001));
        // The retained head is now that untimed or late entry; the evicted sells were inside the minute.
        LiveOrderBook book;
        book.initialize(0, 400, 0.1);
        apply(book, {{true, 200, 4}, {false, 200.1, 7}});
        DomModel model;
        model.publish(book, trades, true, now);
        EXPECT_DOUBLE_EQ(vol(model, rowAt(model, 2000), DomModel::Sells), 0); // the evicted sells are gone...
        // ...and the summary says the minute is incomplete back to the newest evicted stamp.
        EXPECT_TRUE(model.executionSummary().contains("ring-limited: the last 8,192 trades cover only 2.0 s"))
            << model.executionSummary().toStdString();
    }
    DomTradeWindow untimedOnly; // evicting only untimed entries loses nothing in the window
    for (int i = 0; i < DomTradeWindow::Capacity + 5; ++i) untimedOnly.ingest(tradeAt(i < 5 ? 0 : now - 1000, 200, AggressorSide::Buy, 1));
    EXPECT_EQ(untimedOnly.evictedMaxMs(), 0);
}

TEST(DomModel, CoverageWarningEndsWhenTheNewestEvictedTradeLeavesTheMinute) {
    const qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    trades.ingest(tradeAt(now - 2000, 200, AggressorSide::Sell, 100)); // evicted next
    for (int i = 0; i < DomTradeWindow::Capacity; ++i) trades.ingest(tradeAt(0, 200, AggressorSide::Buy, 1)); // untimed only
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.publish(book, trades, true, now);
    EXPECT_TRUE(model.executionSummary().contains("ring-limited"));
    const qint64 expiry = now - 2000 + DomTradeWindow::WindowMs;
    EXPECT_FALSE(model.needsRepublish(expiry - 1));
    EXPECT_TRUE(model.needsRepublish(expiry));
    model.publish(book, trades, true, expiry);
    EXPECT_FALSE(model.executionSummary().contains("ring-limited"));
    EXPECT_FALSE(model.needsRepublish(expiry + 600000)); // settled
}

TEST(DomModel, PublicationRequestsSettleWhenTheLadderGoesAway) {
    const qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 16}, {true, 195, 400}, {false, 200.1, 9}});
    DomModel model;
    model.clear("TEST-USD");
    const auto decaying = [&] {
        model.publish(book, trades, true, now);
        const int touch = rowAt(model, 2000);
        model.setVisibleRows(touch, touch + 60);
        model.publish(book, trades, true, now + 67);
        model.setVisibleRows(touch - 20, touch + 20);
        model.publish(book, trades, true, now + 134);
        ASSERT_TRUE(model.needsRepublish(now + 134)); // mid-decay
    };
    // 1. Withdrawal (INV-130 empty event clears the replica), then the view reports no rows.
    decaying();
    book.clear();
    model.publish(book, trades, true, now + 201);
    EXPECT_EQ(model.rowCount(), 0);
    model.setVisibleRows(-1, -1);
    EXPECT_TRUE(model.needsRepublish(now + 268));
    model.publish(book, trades, true, now + 268);
    EXPECT_FALSE(model.needsRepublish(now + 335));
    // 2. A coarse product replaces a decaying ladder.
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 16}, {true, 195, 400}, {false, 200.1, 9}});
    model.setVisibleRows(-1, -1);
    decaying();
    book.initialize(0, 200, 5);
    apply(book, {{true, 100, 7}, {false, 105, 9}});
    model.publish(book, trades, true, now + 201);
    EXPECT_EQ(model.rowCount(), 0);
    model.setVisibleRows(-1, -1);
    model.publish(book, trades, true, now + 268);
    EXPECT_FALSE(model.needsRepublish(now + 335));
    // 3. A symbol switch clears the model during decay.
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 16}, {true, 195, 400}, {false, 200.1, 9}});
    model.publish(book, trades, true, now + 400);
    decaying();
    model.clear("OTHER-USD");
    EXPECT_FALSE(model.needsRepublish(now + 201));
}

TEST(DomModel, PinnedCumulativeReadsOnlyTheReplicaAndReusesItsSum) {
    LiveOrderBook book;
    book.initialize(0, 500, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    DomTradeWindow trades;
    model.publish(book, trades, true);
    const auto top = model.index(0, 0).data(DomModel::BucketRole).toLongLong();
    // A replacement band far above the pinned window: keys 10,000,000+ against a top near 3,000.
    book.initialize(1'000'000, 1'000'050, 0.1);
    apply(book, {{true, 1'000'020, 5}, {true, 1'000'010, 3}, {false, 1'000'020.1, 6}});
    model.publish(book, trades, false);
    ASSERT_EQ(model.index(0, 0).data(DomModel::BucketRole).toLongLong(), top);
    EXPECT_DOUBLE_EQ(model.index(0, DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 8);
    EXPECT_GT(model.lastDepthSteps(), 0);
    EXPECT_LE(model.lastDepthSteps(), static_cast<qint64>(book.getBids().size())); // never the price distance
    model.publish(book, trades, false); // trade-only or decay publication: no rescan
    EXPECT_EQ(model.lastDepthSteps(), 0);
    EXPECT_DOUBLE_EQ(model.index(0, DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 8);
    apply(book, {{true, 1'000'030, 2}}); // book change: one bounded rescan
    model.publish(book, trades, false);
    EXPECT_DOUBLE_EQ(model.index(0, DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 10);
    EXPECT_LE(model.lastDepthSteps(), static_cast<qint64>(book.getBids().size()));
    // A tick change rescales the pinned window and still sums only replica levels.
    book.initialize(999'990, 1'000'050, 0.01);
    apply(book, {{true, 1'000'020, 5}, {false, 1'000'020.01, 6}});
    model.publish(book, trades, false);
    EXPECT_DOUBLE_EQ(model.tick(), 0.01);
    EXPECT_DOUBLE_EQ(model.index(0, DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 5);
    EXPECT_LE(model.lastDepthSteps(), static_cast<qint64>(book.getBids().size()));
}

TEST(DomModel, DisplayStringsAreFormattedOnceUntilTheirValueChanges) {
    const qint64 now = 1'800'000'000'000;
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4.5}, {true, 195, 400}, {false, 200.1, 7}});
    DomModel model;
    DomTradeWindow trades;
    model.publish(book, trades, true, now);
    const int r = rowAt(model, 2000);
    const auto text = [&](int col) { return model.index(r, col).data().toString(); };
    const QString bid = text(DomModel::Bid), price = text(DomModel::Price);
    const QString neighbour = model.index(rowAt(model, 2010), DomModel::Price).data().toString();
    const QString deepPrice = model.index(rowAt(model, 1950), DomModel::Price).data().toString();
    const QString deepSize = model.index(rowAt(model, 1950), DomModel::Bid).data().toString();
    EXPECT_EQ(deepSize, "400");
    EXPECT_EQ(bid, "4.5");
    EXPECT_EQ(text(DomModel::Bid).constData(), bid.constData()); // repaint: the same shared string
    // A scale-only publication (the wall scrolls out: decay) repaints without reformatting.
    model.setVisibleRows(r - 20, r + 20);
    model.publish(book, trades, true, now + 67);
    model.setVisibleRows(r, r + 60);
    model.publish(book, trades, true, now + 134);
    model.setVisibleRows(r - 20, r + 20);
    model.publish(book, trades, true, now + 201);
    ASSERT_TRUE(model.needsRepublish(now + 201));
    EXPECT_EQ(text(DomModel::Bid).constData(), bid.constData());
    EXPECT_EQ(text(DomModel::Price).constData(), price.constData());
    apply(book, {{true, 200, 5.5}});
    model.publish(book, trades, true, now + 268);
    EXPECT_EQ(text(DomModel::Bid), "5.5");
    EXPECT_EQ(model.index(rowAt(model, 2010), DomModel::Price).data().toString().constData(), neighbour.constData()); // unchanged rows keep theirs
    // Rows below the touch change only cumulative depth: their price and normal-mode size stay.
    EXPECT_EQ(model.index(rowAt(model, 1950), DomModel::Price).data().toString().constData(), deepPrice.constData());
    EXPECT_EQ(model.index(rowAt(model, 1950), DomModel::Bid).data().toString().constData(), deepSize.constData());
    EXPECT_EQ(text(DomModel::Price).constData(), price.constData()); // touch row: its price did not change either
    model.setCumulative(true);
    EXPECT_EQ(text(DomModel::Bid), "5.5");
    EXPECT_EQ(model.index(rowAt(model, 1950), DomModel::Bid).data().toString(), "406");
}

TEST(DomModel, BarsScaleToVisibleRowsWithSquareRootAndSmoothedMaximum) {
    const qint64 now = 1'800'000'000'000;
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    // A 400 wall 50 ticks below the touch; 16 at the touch, 4 one tick down.
    apply(book, {{true, 200, 16}, {true, 199.9, 4}, {true, 195, 400}, {false, 200.1, 9}});
    DomModel model;
    DomTradeWindow trades;
    model.publish(book, trades, true, now);
    EXPECT_DOUBLE_EQ(model.scaleMax(true, false), 400); // no view range yet: every row
    const int touch = rowAt(model, 2000);
    model.setVisibleRows(touch - 20, touch + 20);
    EXPECT_TRUE(model.needsRepublish(now));
    model.publish(book, trades, true, now + 67);
    EXPECT_DOUBLE_EQ(model.scaleMax(true, false), 16); // first reported range applies at once
    EXPECT_DOUBLE_EQ(model.barFraction(16, true, false), 1);
    EXPECT_DOUBLE_EQ(model.barFraction(4, true, false), .5); // square root of 1/4
    EXPECT_DOUBLE_EQ(model.scaleMax(false, false), 9);
    EXPECT_DOUBLE_EQ(model.scaleMax(true, true), 20); // cumulative at the lowest visible row
    EXPECT_FALSE(model.needsRepublish(now + 67));
    model.setVisibleRows(touch, touch + 60); // the wall scrolls into view: jump up
    model.publish(book, trades, true, now + 134);
    EXPECT_DOUBLE_EQ(model.scaleMax(true, false), 400);
    model.setVisibleRows(touch - 20, touch + 20); // and out again: decay, no jump
    qint64 t = now + 134;
    model.publish(book, trades, true, t += 67);
    const double afterOneTick = model.scaleMax(true, false);
    EXPECT_GT(afterOneTick, 16);
    EXPECT_LT(afterOneTick, 400);
    EXPECT_TRUE(model.needsRepublish(t));
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    while (model.needsRepublish(t) && t < now + 5000) model.publish(book, trades, true, t += 67);
    EXPECT_DOUBLE_EQ(model.scaleMax(true, false), 16);
    EXPECT_GE(t - (now + 134), 300); // settles over roughly 300-500 ms of 67 ms ticks
    EXPECT_LE(t - (now + 134), 600);
    EXPECT_GE(changed.count(), 4);
}

TEST(DomModel, CumulativeSumsFromTheTouchOutwardIncludingLevelsBeyondAPinnedWindow) {
    LiveOrderBook book;
    book.initialize(0, 500, 0.1);
    apply(book, {{true, 200, 4}, {true, 199.9, 3}, {true, 199.5, 2}, {false, 200.1, 7}, {false, 200.3, 1}});
    DomModel model;
    DomTradeWindow trades;
    model.publish(book, trades, true);
    const auto cum = [&](qint64 key, int col) { return model.index(rowAt(model, key), col).data(DomModel::CumulativeRole).toDouble(); };
    EXPECT_DOUBLE_EQ(cum(2001, DomModel::Bid), 0);
    EXPECT_DOUBLE_EQ(cum(2000, DomModel::Bid), 4);
    EXPECT_DOUBLE_EQ(cum(1999, DomModel::Bid), 7);
    EXPECT_DOUBLE_EQ(cum(1995, DomModel::Bid), 9);
    EXPECT_DOUBLE_EQ(cum(1500, DomModel::Bid), 9);
    EXPECT_DOUBLE_EQ(cum(2000, DomModel::Ask), 0);
    EXPECT_DOUBLE_EQ(cum(2001, DomModel::Ask), 7);
    EXPECT_DOUBLE_EQ(cum(2003, DomModel::Ask), 8);
    EXPECT_DOUBLE_EQ(cum(2500, DomModel::Ask), 8);
    const int row = rowAt(model, 1995);
    EXPECT_EQ(model.index(row, DomModel::Bid).data().toString(), "2");
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setCumulative(true);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(model.index(row, DomModel::Bid).data().toString(), "9");
    EXPECT_DOUBLE_EQ(model.index(row, DomModel::Bid).data(DomModel::QuantityRole).toDouble(), 2); // data keeps per-row size
    EXPECT_EQ(model.headerData(DomModel::Bid, Qt::Horizontal, Qt::DisplayRole).toString(), "Σ Bid");
    // The market leaves the pinned window: levels between the touch and the window still count.
    apply(book, {{false, 200.1, 0}, {false, 200.3, 0}, {true, 350, 5}, {true, 340, 1}, {false, 350.1, 6}});
    const auto top = model.index(0, 0).data(DomModel::BucketRole);
    model.publish(book, trades, false);
    EXPECT_EQ(model.index(0, 0).data(DomModel::BucketRole), top); // window pinned below the touch
    EXPECT_DOUBLE_EQ(cum(2000, DomModel::Bid), 10);
    EXPECT_DOUBLE_EQ(cum(1995, DomModel::Bid), 15);
}

TEST(DomModel, SizesShowThreeSignificantDigitsWithoutFloatNoise) {
    EXPECT_EQ(DomModel::sizeText(.634), ".634");
    EXPECT_EQ(DomModel::sizeText(5.8712), "5.87");
    EXPECT_EQ(DomModel::sizeText(13.64), "13.6");
    EXPECT_EQ(DomModel::sizeText(136.4), "136");
    EXPECT_EQ(DomModel::sizeText(999.6), "1k");
    EXPECT_EQ(DomModel::sizeText(1364.2), "1.36k");
    EXPECT_EQ(DomModel::sizeText(12345678.9), "12.3m");
    EXPECT_EQ(DomModel::sizeText(.0634), ".0634");
    EXPECT_EQ(DomModel::sizeText(-.25), "-.25");
    EXPECT_EQ(DomModel::sizeText(.000284), ".000284");
    EXPECT_EQ(DomModel::sizeText(2.92e-6), "<.0001"); // dust, never e-notation
    EXPECT_EQ(DomModel::sizeText(1e-4), ".0001");
    // Signed (Δ) values: never "-<.0001"; dust and exact cancellation read as about zero.
    EXPECT_EQ(DomModel::signedSizeText(1.25), "+1.25");
    EXPECT_EQ(DomModel::signedSizeText(-.5), "-.5");
    EXPECT_EQ(DomModel::signedSizeText(1e-4), "+.0001");
    EXPECT_EQ(DomModel::signedSizeText(-1e-4), "-.0001");
    EXPECT_EQ(DomModel::signedSizeText(9.99e-5), "≈0");
    EXPECT_EQ(DomModel::signedSizeText(-1e-7), "≈0");
    EXPECT_EQ(DomModel::signedSizeText(0), "≈0");
    // The server sends float sizes widened to double: 0.0032f is 0.0031999999191612.
    const double noisy = double(0.0032f);
    EXPECT_EQ(DomModel::sizeText(noisy), ".0032");
    EXPECT_EQ(DomModel::fullSizeText(noisy), "0.0032");
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, noisy}, {false, 200.1, double(1.2345678f)}});
    DomModel model;
    model.clear("BTC-USD");
    model.publish(book, {}, true);
    const auto bid = model.index(rowAt(model, 2000), DomModel::Bid);
    EXPECT_EQ(bid.data().toString(), ".0032");
    EXPECT_DOUBLE_EQ(bid.data(DomModel::QuantityRole).toDouble(), noisy); // full value stays in data
    const auto tip = bid.data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(tip.startsWith("0.0032 BTC resting bid")) << tip.toStdString();
    EXPECT_FALSE(tip.contains("0.00319999"));
    EXPECT_TRUE(model.index(rowAt(model, 2001), DomModel::Ask).data(Qt::ToolTipRole).toString().startsWith("1.234568 BTC"));
    EXPECT_EQ(model.index(rowAt(model, 2001), DomModel::Ask).data().toString(), "1.23");
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
    EXPECT_EQ(vol(model, rowAt(model, 2000), DomModel::Buys), 1);
    EXPECT_EQ(vol(model, rowAt(model, 2001), DomModel::Sells), 1);
    book.initialize(100, 300, 0.2);
    apply(book, {{true, 200, 4}, {false, 200.4, 7}});
    model.publish(book, trades, false);
    EXPECT_EQ(vol(model, rowAt(model, 1000), DomModel::Buys), 1);
    EXPECT_EQ(vol(model, rowAt(model, 1000), DomModel::Sells), 1);
    int buyCount = 0, sellCount = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        buyCount += vol(model, r, DomModel::Buys);
        sellCount += vol(model, r, DomModel::Sells);
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
    trades.ingest(trade(price + .09, AggressorSide::Buy)); // same containing bucket, never rounded up
    DomModel model;
    model.publish(book, trades, true);
    int hits = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        if (model.index(r, DomModel::Bid).data(DomModel::QuantityRole).toDouble() > 0) {
            ++hits;
            EXPECT_EQ(vol(model, r, DomModel::Buys), 2);
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

namespace sb = sentinel::book;
// Book, model and the pulse's monotonic clock, stepped like the dock's 67 ms timer.
struct PulseRig {
    LiveOrderBook book;
    DomModel model;
    DomTradeWindow trades;
    qint64 wall = 1'800'000'000'000, t = 1000;
    PulseRig() {
        book.initialize(0, 400, 0.1);
        model.clear("BTC-USD");
        apply(book, {{true, 200, 10}, {true, 199.9, 4}, {true, 199.8, 4}, {true, 199.7, 4},
                     {false, 200.1, 10}, {false, 200.2, 4}, {false, 200.3, 4}, {false, 200.4, 4}});
    }
    // A trade received after the last step: inside the next step's interval.
    sb::Trade received(double price, double size, sb::Aggressor side) const { return {price, size, side, t + 1}; }
    void step(std::span<const sb::Trade> received = {}, bool follow = true) {
        wall += 67;
        t += 67;
        model.publish(book, trades, follow, wall);
        model.pulseStep(t, received);
    }
    int row(qint64 key) { return rowAt(model, key); }
    DomPulseCell cell(qint64 key, bool bid) { return model.pulse(row(key), bid); }
};

TEST(DomPulse, AddedPulledAndEatenFlashOnTheirRowsWithTagsAndSmallChangesHide) {
    PulseRig rig;
    rig.step();
    ASSERT_EQ(rig.model.lastPulseEvents(), 0u); // first step only observes
    const std::array trades{rig.received(200.1, 2.5, sb::Aggressor::Buy)};
    apply(rig.book, {{true, 199.9, 6}, {false, 200.2, 1}, {false, 200.1, 7.5}, {true, 199.8, 4.1}});
    rig.step(trades);
    EXPECT_EQ(rig.model.lastPulseEvents(), 4u); // the classifier sees the 0.1 change too
    const auto added = rig.cell(1999, true), pulled = rig.cell(2002, false), eaten = rig.cell(2001, false);
    EXPECT_DOUBLE_EQ(added.change, 2);
    EXPECT_DOUBLE_EQ(added.changeAlpha, 1);
    EXPECT_LT(added.grow, 1); // the segment grows outward over the first ticks
    EXPECT_EQ(rig.model.pulseTag(rig.row(1999), true, false), "+2");
    EXPECT_DOUBLE_EQ(pulled.change, -3);
    EXPECT_EQ(rig.model.pulseTag(rig.row(2002), false, false), "-3");
    EXPECT_DOUBLE_EQ(eaten.eaten, 2.5);
    EXPECT_DOUBLE_EQ(eaten.change, 0); // the drop was traded, not pulled
    EXPECT_EQ(rig.model.pulseTag(rig.row(2001), false, true), "×2.5");
    // 0.1 is under 5% of the largest visible bid (10): no flash.
    EXPECT_DOUBLE_EQ(rig.model.pulseThreshold(true), 0.5);
    EXPECT_DOUBLE_EQ(rig.cell(1998, true).change, 0);
    for (qint64 key : {1997, 2000, 2003}) {
        EXPECT_EQ(rig.cell(key, true).change + rig.cell(key, false).change, 0) << key;
        EXPECT_EQ(rig.cell(key, true).eaten + rig.cell(key, false).eaten, 0) << key;
    }
}

TEST(DomPulse, FlashesFadeWithinTheFadeTimeAndRepublishSettles) {
    PulseRig rig;
    rig.step();
    apply(rig.book, {{false, 200.2, 1}});
    rig.step();
    const int row = rig.row(2002);
    ASSERT_DOUBLE_EQ(rig.cell(2002, false).change, -3);
    ASSERT_EQ(rig.model.pulseTag(row, false, false), "-3"); // cached while live
    QSignalSpy changed(&rig.model, &QAbstractItemModel::dataChanged);
    double alpha = 1;
    // Unchanged book: the fading row still repaints every tick.
    constexpr qint64 fadeMs = 1200; // owner spec, not the constant under test
    for (qint64 age = 67; age < fadeMs; age += 67) {
        ASSERT_TRUE(rig.model.needsRepublish(rig.wall + 67)) << age;
        const int before = changed.count();
        rig.step();
        ASSERT_EQ(changed.count(), before + 1) << age;
        EXPECT_LE(changed.last().at(0).value<QModelIndex>().row(), row);
        EXPECT_GE(changed.last().at(1).value<QModelIndex>().row(), row);
        const double now = rig.cell(2002, false).changeAlpha;
        EXPECT_LT(now, alpha) << age;
        EXPECT_GT(now, 0) << age;
        alpha = now;
    }
    rig.step(); // 1,206 ms: gone
    EXPECT_DOUBLE_EQ(rig.cell(2002, false).change, 0);
    EXPECT_TRUE(rig.model.pulseTag(row, false, false).isEmpty());
    ASSERT_TRUE(rig.model.needsRepublish(rig.wall + 67)); // repaint the cleared row once
    const int before = changed.count();
    rig.step();
    EXPECT_EQ(changed.count(), before + 1);
    EXPECT_FALSE(rig.model.needsRepublish(rig.wall + 67));
    rig.step();
    EXPECT_FALSE(rig.model.needsRepublish(rig.wall + 67));
}

TEST(DomPulse, ScrollFollowShiftSymbolSwitchAndWithdrawalProduceNoEvents) {
    PulseRig rig;
    apply(rig.book, {{true, 199.6, 4}, {true, 199.5, 4}, {true, 199.4, 4}, {false, 200.5, 4}, {false, 200.6, 4}});
    const auto flashes = [&] {
        int live = 0;
        for (int r = 0; r < rig.model.rowCount(); ++r)
            for (bool bid : {true, false}) live += rig.model.pulse(r, bid).change != 0 || rig.model.pulse(r, bid).eaten != 0;
        return live;
    };
    rig.step();
    const int center = rig.model.centerRow();
    // Scroll: the visible window moves by 12 rows, the book does not.
    rig.model.setVisibleRows(center - 30, center - 6);
    rig.step({}, false);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    // Follow: the mid falls three ticks; only the five pulled bids are events.
    rig.model.setVisibleRows(center - 12, center + 12);
    rig.step();
    const auto top = rig.model.index(0, 0).data(DomModel::BucketRole).toLongLong();
    apply(rig.book, {{true, 200, 0}, {true, 199.9, 0}, {true, 199.8, 0}, {true, 199.7, 0}, {true, 199.6, 0}});
    rig.step();
    ASSERT_NE(rig.model.index(0, 0).data(DomModel::BucketRole).toLongLong(), top);
    EXPECT_EQ(rig.model.lastPulseEvents(), 5u);
    EXPECT_EQ(flashes(), 5);
    for (qint64 key : {2000, 1999, 1998, 1997, 1996}) EXPECT_LT(rig.cell(key, true).change, 0) << key;
    // Pinned, then Recenter with an unchanged book: a window move alone.
    rig.step({}, false);
    apply(rig.book, {{false, 200.1, 0}, {false, 200.2, 0}, {false, 200.3, 0}});
    rig.step({}, false);
    EXPECT_EQ(rig.model.lastPulseEvents(), 3u);
    const auto pinned = rig.model.index(0, 0).data(DomModel::BucketRole).toLongLong();
    rig.step({}, true);
    ASSERT_NE(rig.model.index(0, 0).data(DomModel::BucketRole).toLongLong(), pinned);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    // Symbol switch: another product's book at the same prices is a fresh baseline.
    rig.model.clear("ETH-USD");
    EXPECT_EQ(flashes(), 0);
    apply(rig.book, {{true, 199.5, 40}, {true, 199.4, 1}, {false, 200.5, 30}, {false, 200.6, 1}});
    rig.step();
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    EXPECT_EQ(flashes(), 0);
    // Withdrawal (INV-130 empty update clears the replica), then different sizes return.
    rig.book.clear();
    rig.step();
    EXPECT_EQ(rig.model.rowCount(), 0);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    rig.book.initialize(0, 400, 0.1);
    apply(rig.book, {{true, 199.5, 2}, {true, 199.4, 9}, {false, 200.5, 1}, {false, 200.6, 9}});
    rig.step();
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    EXPECT_EQ(flashes(), 0);
    EXPECT_FALSE(rig.model.needsRepublish(rig.wall + 67));
}

TEST(DomPulse, FooterSumsEachKindPerSideAndNamesTheBiggestPull) {
    PulseRig rig;
    rig.step();
    EXPECT_EQ(rig.model.pulseSummary(), "Asks pulled 0 eaten 0 · Bids pulled 0 eaten 0");
    const std::array first{rig.received(200.1, 1.1, sb::Aggressor::Buy), rig.received(200, 2, sb::Aggressor::Sell)};
    apply(rig.book, {{false, 200.1, 8.9}, {true, 200, 8}, {false, 200.3, 0}, {false, 200.4, 1.2}});
    rig.step(first);
    apply(rig.book, {{true, 199.7, 3.6}, {false, 200.2, 4.5}});
    rig.step();
    // Asks pulled 4 + 2.8, bids 0.4; the largest single pull is the 4 at 200.3.
    EXPECT_EQ(rig.model.pulseSummary(), "Asks pulled 6.8 eaten 1.1 · Bids pulled .4 eaten 2 · biggest pull 4 @ 200.3");
    EXPECT_EQ(rig.model.pulseSummary(DomModel::PulseText::Compact), "Ask pull 6.8 eat 1.1 · Bid pull .4 eat 2 · max 4 @ 200.3");
    EXPECT_DOUBLE_EQ(rig.model.pulseTotals().side[1].added, 0.5);
    const auto version = rig.model.pulseTotalsVersion();
    rig.step();
    EXPECT_EQ(rig.model.pulseTotalsVersion(), version); // unchanged numbers: no footer rebuild
}

TEST(DomPulse, PublishAndStepAllocateNothingAfterWarmUp) {
    PulseRig rig;
    std::array<sb::Trade, 4> received{};
    int tags = 0;
    const auto cycle = [&](int i) {
        received[0] = rig.received(200.1, 0.5, sb::Aggressor::Buy);
        rig.step(std::span(received.data(), 1), i % 7 != 0);
        for (int r = rig.model.centerRow() - 6; r <= rig.model.centerRow() + 6; ++r)
            for (bool bid : {true, false}) {
                const auto cell = rig.model.pulse(r, bid);
                if (cell.change) tags += !rig.model.pulseTag(r, bid, false).isEmpty();
                if (cell.eaten) tags += !rig.model.pulseTag(r, bid, true).isEmpty();
            }
        (void)rig.model.needsRepublish(rig.wall);
        (void)rig.model.pulseTotals();
    };
    for (int i = 0; i < 40; ++i) cycle(i);
    std::vector<std::array<BookLevelUpdate, 2>> changes;
    for (int i = 0; i < 60; ++i) changes.push_back({BookLevelUpdate{false, 200.1, 10.0 - i % 3}, BookLevelUpdate{true, 199.9, 4.0 + i % 5}});
    size_t allocations = 0;
    for (int i = 0; i < 60; ++i) {
        rig.book.applyUpdates(std::span(changes[i]), std::chrono::system_clock::now(), nullptr); // replica work, not counted
        gAllocations = 0;
        gCounting = true;
        cycle(i);
        // Fading frames repaint flashes and tags without new events.
        cycle(i);
        gCounting = false;
        allocations += gAllocations;
    }
    EXPECT_EQ(allocations, 0u); // operator new: classifier, model and std containers
    EXPECT_GT(tags, 200); // the counted cycles read live tags
    // Qt strings allocate through malloc, outside that counter: a fading tag is the same
    // shared string on every repaint, formatted once when its flash starts.
    apply(rig.book, {{true, 199.8, 9}});
    rig.step();
    const int row = rig.row(1998);
    const QChar* tag = rig.model.pulseTag(row, true, false).constData();
    ASSERT_EQ(rig.model.pulseTag(row, true, false), "+5");
    const auto formats = rig.model.pulseTagFormats();
    for (int i = 0; i < 10; ++i) {
        rig.step();
        ASSERT_GT(rig.model.pulse(row, true).changeAlpha, 0);
        EXPECT_EQ(rig.model.pulseTag(row, true, false).constData(), tag) << i;
    }
    EXPECT_EQ(rig.model.pulseTagFormats(), formats); // formatted once, when the flash started
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
        const auto timestamp = at ? std::chrono::system_clock::time_point(std::chrono::milliseconds(at))
                                  : std::chrono::system_clock::now();
        std::vector<BookDelta> deltas;
        book.applyUpdates(std::span(updates.begin(), updates.size()), timestamp, &deltas);
        if (!deltas.empty()) emit liveOrderBookUpdated("BTC-USD", deltas);
    }
    void send(const Trade& value) { emit tradeReceived(value); }
    const LiveOrderBook& getDirectLiveOrderBook(const std::string&) const override { ++reads; return book; }
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
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
    // The scripted books are a few levels: native rows unless a test picks a grouping.
    void SetUp() override { dock.setGrouping(OrderBookDock::Grouping::Fixed, 0.1); dock.resize(650, 600); dock.onSymbolChanged("BTC-USD"); }
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
        EXPECT_LT(table->y() - (dock.findChild<QWidget*>("domLeanStrip")->isVisible() ? dock.findChild<QWidget*>("domLeanStrip")->height() + 2 : 0), 110); // heading + two metadata lines, not a seven-line paragraph.
        EXPECT_TRUE(aggregation->text().contains("USD"));
        EXPECT_TRUE(aggregation->text().contains("BTC"));
        EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("last 60 s by trade time"));
        EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("unknown"));
    }
}

TEST(DomDelegate, HeatmapRampPriceDigitsAndSizeTextTint) {
    // Samples of the heatmap's own default palette image (Electric): no invented colours.
    const auto texels = heatmap::gpu::paletteTexels(heatmap::gpu::gradientsFor(heatmap::HeatmapChartSettings{}));
    const auto texel = [&](int i) { return QColor(texels[i * 4], texels[i * 4 + 1], texels[i * 4 + 2]); };
    EXPECT_EQ(DomBarDelegate::barColor(true, 1), texel(255));
    EXPECT_EQ(DomBarDelegate::barColor(false, 1), texel(511));
    EXPECT_EQ(DomBarDelegate::barColor(true, 0), texel(140));
    EXPECT_EQ(DomBarDelegate::barColor(false, 0), texel(396));
    EXPECT_EQ(DomBarDelegate::barColor(false, 1), QColor(255, 200, 0));
    const QColor lowBid = DomBarDelegate::barColor(true, 0), midBid = DomBarDelegate::barColor(true, .5);
    EXPECT_EQ(lowBid.red(), 0);
    EXPECT_GT(lowBid.blue(), lowBid.green()); // navy
    EXPECT_GT(lowBid.blue(), 0);
    EXPECT_LT(lowBid.blue(), midBid.blue());
    EXPECT_LT(midBid.green(), 255);
    const QColor lowAsk = DomBarDelegate::barColor(false, 0);
    EXPECT_GT(lowAsk.red(), lowAsk.green());
    EXPECT_LT(lowAsk.red(), DomBarDelegate::barColor(false, .5).red());
    EXPECT_LT(DomBarDelegate::sizeTextColor(true, .1).alpha(), DomBarDelegate::sizeTextColor(true, .9).alpha());
    EXPECT_GT(DomBarDelegate::sizeTextColor(true, .5).red(), DomBarDelegate::barColor(true, .5).red()); // lighter tint
    EXPECT_EQ(DomBarDelegate::dimPrefixLength("84123.4"), 3);   // 841 | 23.4
    EXPECT_EQ(DomBarDelegate::dimPrefixLength("84123.45"), 4);  // 8412 | 3.45
    EXPECT_EQ(DomBarDelegate::dimPrefixLength("0.20002"), 4);   // 0.20 | 002
    EXPECT_EQ(DomBarDelegate::dimPrefixLength("12"), 0);
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
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Buys), 1400);
    EXPECT_TRUE(table->horizontalHeader()->toolTip().contains("unknown side: 100"));
    const int reads = source.reads;
    dock.hide();
    const int notifications = changed.count() + reset.count() + inserted.count();
    source.send(trade(200, AggressorSide::Sell));
    source.update({{true, 200, 9999}});
    QTest::qWait(160);
    EXPECT_EQ(source.reads, reads);
    EXPECT_EQ(changed.count() + reset.count() + inserted.count(), notifications);
    show();
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Sells), 1);
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
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Buys), 2500);
}

TEST_F(DomDock, QuietConnectedBookRetainsLastChangeAgeAndDetectsDisconnect) {
    // Connection signal predates symbol switch; the datasource retains its state.
    emit source.connectionStatusChanged(true);
    dock.onSymbolChanged("ETH-USD");
    dock.onSymbolChanged("BTC-USD");
    const qint64 changedAt = QDateTime::currentMSecsSinceEpoch() - 60000;
    source.update({{true, 200, 2}, {false, 200.1, 3}}, changedAt);
    show();
    EXPECT_TRUE(label("domFreshness").startsWith("Live · last change "));
    EXPECT_GE(label("domFreshness").section("last change ", 1).section(" s", 0, 0).toDouble(), 60.0);
    auto* timer = dock.findChild<QTimer*>("domPublishTimer");
    timer->stop();
    for (int i = 0; i < 100; ++i) {
        // Like the real replica: identical updates advance getLastUpdate but
        // produce no deltas / liveOrderBookUpdated signal.
        apply(source.book, {{true, 200, 2}, {false, 200.1, 3}});
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    }
    EXPECT_TRUE(label("domFreshness").startsWith("Live · last change "));
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
    EXPECT_TRUE(label("domFreshness").startsWith("Live"));
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
    EXPECT_TRUE(label("domFreshness").startsWith("Stale"));
}

TEST(DomWindow, MinimizedMainWindowStopsTimerAndRetainsTradeIngestion) {
    Source source;
    QMainWindow window;
    auto* dock = new OrderBookDock(&window, &source);
    dock->setGrouping(OrderBookDock::Grouping::Fixed, 0.1); // a two-level book: native rows
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
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Sells), 1);
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
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Sells), 1);
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
    EXPECT_EQ(vol(*model, rowAt(*model, 2000), DomModel::Buys), 0);
    EXPECT_TRUE(model->headerData(DomModel::Bid, Qt::Horizontal, Qt::ToolTipRole).toString().contains("ETH"));
}

TEST_F(DomDock, CumulativeToggleSwitchesTheSizeColumnsAndPersists) {
    source.update({{true, 200, 1}, {true, 199.9, 2}, {false, 200.1, 1}});
    show();
    auto* toggle = dock.findChild<QToolButton*>("domCumulative");
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isCheckable());
    EXPECT_FALSE(toggle->isChecked());
    const int row = rowAt(*model, 1999);
    EXPECT_EQ(model->index(row, DomModel::Bid).data().toString(), "2");
    toggle->click();
    EXPECT_TRUE(model->cumulative());
    EXPECT_EQ(model->index(row, DomModel::Bid).data().toString(), "3");
    {
        Source other;
        OrderBookDock reopened{nullptr, &other};
        EXPECT_TRUE(reopened.findChild<QToolButton*>("domCumulative")->isChecked());
        auto* reopenedModel = static_cast<DomModel*>(reopened.findChild<QTableView*>("domLadder")->model());
        EXPECT_TRUE(reopenedModel->cumulative());
    }
    toggle->click();
    EXPECT_FALSE(model->cumulative());
    OrderBookDock reopened{nullptr, &source};
    EXPECT_FALSE(reopened.findChild<QToolButton*>("domCumulative")->isChecked());
}

TEST_F(DomDock, BestCellsFillInTheSideColourAndBarsUseTheRamp) {
    // An 800 wall 500 ticks down, far outside the viewport, must not shrink the visible bars.
    source.update({{true, 200, 8}, {true, 199.9, 2}, {true, 150, 800}, {false, 200.1, 5}});
    show();
    for (int i = 0; i < 2; ++i) QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection);
    const QImage image = table->viewport()->grab().toImage();
    const auto pixel = [&](int row, int col, double x) {
        const QRect cell = table->visualRect(model->index(row, col));
        return image.pixelColor(cell.left() + int(cell.width() * x), cell.center().y());
    };
    const int best = rowAt(*model, 2000), next = rowAt(*model, 1999), ask = rowAt(*model, 2001);
    EXPECT_DOUBLE_EQ(model->scaleMax(true, false), 8);
    EXPECT_EQ(pixel(best, DomModel::Bid, .1), DomBarDelegate::bestFill(true));
    EXPECT_EQ(pixel(ask, DomModel::Ask, .1), DomBarDelegate::bestFill(false));
    // A 2 next to an 8 on the visible scale: half-length bar (square root), drawn from the price side.
    const QColor near = pixel(next, DomModel::Bid, .8), far = pixel(next, DomModel::Bid, .2);
    EXPECT_GT(near.blue(), near.red() + 40); // ramp colour, not the neutral cell background
    EXPECT_LT(std::abs(far.blue() - far.red()), 10);
    EXPECT_NE(pixel(next, DomModel::Bid, .45), pixel(next, DomModel::Bid, .55));
}

TEST_F(DomDock, TradesLeaveTheWindowWithoutAnyNewEvent) {
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    source.send(tradeAt(QDateTime::currentMSecsSinceEpoch() - DomTradeWindow::WindowMs + 300, 200, AggressorSide::Buy, 2));
    show();
    EXPECT_DOUBLE_EQ(vol(*model, rowAt(*model, 2000), DomModel::Buys), 2);
    QTest::qWait(600); // the dock's own 67 ms timer, no book or trade event
    EXPECT_DOUBLE_EQ(vol(*model, rowAt(*model, 2000), DomModel::Buys), 0);
}

struct DomPulseDock : DomDock {
    qint64 clock = 5000;
    QTimer* timer = nullptr;
    void SetUp() override {
        DomDock::SetUp();
        dock.setPulseClock([this] { return clock; });
        source.update({{true, 200, 10}, {true, 199.9, 5}, {false, 200.1, 10}, {false, 200.2, 4}});
        show();
        timer = dock.findChild<QTimer*>("domPublishTimer");
        timer->stop();
        tick();
    }
    void tick(qint64 ms = 67) {
        flushEvents();
        clock += ms;
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
    }
    QToolButton* toggle() { return dock.findChild<QToolButton*>("domPulse"); }
    DomPulseCell cell(qint64 key, bool bid) { return model->pulse(rowAt(*model, key), bid); }
};

TEST_F(DomPulseDock, QuietBookTotalsExpireOnScheduleFedEveryTick) {
    ASSERT_TRUE(toggle()->isChecked()); // default on
    source.update({{true, 199.9, 0}});
    tick();
    const auto footer = dock.findChild<QLabel*>("domPulseFooter");
    ASSERT_TRUE(footer->isVisible());
    EXPECT_EQ(footer->text(), "Asks pulled 0 eaten 0 · Bids pulled 5 eaten 0 · biggest pull 5 @ 199.9");
    EXPECT_TRUE(footer->toolTip().contains("last 10 s"));
    // The bar scale decays in wall time: let it settle so only the pulse clock moves.
    QTest::qWait(700);
    // No book change for ten seconds: the totals still move, from the timer alone.
    for (int i = 1; i < 149; ++i) tick();
    EXPECT_EQ(footer->text(), "Asks pulled 0 eaten 0 · Bids pulled 5 eaten 0 · biggest pull 5 @ 199.9");
    tick(); // 9,983 ms after the pull
    EXPECT_TRUE(footer->text().contains("Bids pulled 5"));
    tick(); // 10,050 ms
    EXPECT_EQ(footer->text(), "Asks pulled 0 eaten 0 · Bids pulled 0 eaten 0");
}

TEST_F(DomPulseDock, TradesAreStampedOnReceiptAndEatTheNextSnapshot) {
    // Received in the same millisecond as the last snapshot, carrying an old exchange time.
    source.send(tradeAt(1000, 200.1, AggressorSide::Buy, 2));
    source.update({{false, 200.1, 8}});
    tick();
    EXPECT_DOUBLE_EQ(cell(2001, false).eaten, 2);
    EXPECT_DOUBLE_EQ(cell(2001, false).change, 0);
    for (int i = 0; i < 20; ++i) {
        source.send(trade(200, AggressorSide::Sell, 0.5));
        source.update({{true, 200, 10.0 - 0.5 * (i + 1)}});
        tick(i % 3 ? 67 : 0); // also a tick in the same millisecond
        EXPECT_DOUBLE_EQ(cell(2000, true).eaten, 0.5) << i;
    }
    EXPECT_EQ(model->pulseOutOfIntervalTotal(), 0u);
}

TEST_F(DomPulseDock, HiddenOrDisabledPulseDoesNoWorkAndStartsFromAFreshBaseline) {
    const auto noFlashes = [&] {
        for (qint64 key : {1999, 2000, 2001, 2002})
            for (bool bid : {true, false}) EXPECT_EQ(cell(key, bid).change + cell(key, bid).eaten, 0) << key << bid;
    };
    dock.hide();
    const auto steps = model->pulseSteps();
    source.update({{true, 199.9, 0}, {false, 200.2, 40}});
    source.send(trade(200.1, AggressorSide::Buy, 3));
    source.update({{false, 200.1, 7}});
    tick();
    QTest::qWait(160);
    EXPECT_EQ(model->pulseSteps(), steps);
    show();
    timer->stop();
    tick();
    EXPECT_EQ(model->lastPulseEvents(), 0u); // no burst of what changed while hidden
    noFlashes();
    toggle()->click();
    EXPECT_FALSE(dock.findChild<QLabel*>("domPulseFooter")->isVisible());
    const auto off = model->pulseSteps();
    source.update({{true, 199.9, 6}, {false, 200.2, 1}});
    for (int i = 0; i < 5; ++i) tick();
    EXPECT_EQ(model->pulseSteps(), off);
    noFlashes();
    toggle()->click();
    tick();
    EXPECT_EQ(model->lastPulseEvents(), 0u);
    noFlashes();
    source.update({{true, 199.9, 9}});
    tick();
    EXPECT_DOUBLE_EQ(cell(1999, true).change, 3);
}

TEST_F(DomPulseDock, PulseTogglePersistsAndRestores) {
    ASSERT_TRUE(toggle()->isChecked());
    toggle()->click();
    {
        Source other;
        OrderBookDock reopened{nullptr, &other};
        EXPECT_FALSE(reopened.findChild<QToolButton*>("domPulse")->isChecked());
    }
    toggle()->click();
    Source other;
    OrderBookDock reopened{nullptr, &other};
    EXPECT_TRUE(reopened.findChild<QToolButton*>("domPulse")->isChecked());
}

TEST_F(DomPulseDock, FlashesPaintInTheirColours) {
    dock.resize(650, 700); QCoreApplication::processEvents();
    source.send(trade(200.1, AggressorSide::Buy, 3));
    source.update({{false, 200.1, 7}, {true, 199.9, 0}});
    tick();
    const QImage image = table->viewport()->grab().toImage();
    const auto pixel = [&](qint64 key, int col, double x, double y = 0.5) {
        const QRect cell = table->visualRect(model->index(rowAt(*model, key), col));
        return image.pixelColor(cell.left() + int(cell.width() * x), cell.top() + int(cell.height() * y));
    };
    // Eaten ask row: a buyer's flash (bid colour) across the row, the price cell included.
    const QColor eaten = pixel(2001, DomModel::Price, 0.1);
    EXPECT_GT(eaten.blue(), eaten.red() + 40);
    // Pulled bid (5 of a visible 10): a grey dashed outline where the bar was, nothing inside.
    const QColor inside = pixel(1999, DomModel::Bid, 0.85);
    EXPECT_LT(std::abs(inside.blue() - inside.red()), 12);
    // Its top edge is dashed grey; the same pixels of an untouched row are plain background.
    const QRect cell = table->visualRect(model->index(rowAt(*model, 1999), DomModel::Bid));
    const QRect plain = table->visualRect(model->index(rowAt(*model, 1990), DomModel::Bid));
    ASSERT_TRUE(image.rect().contains(plain));
    int dashes = 0;
    for (int x = cell.right() - 6; x > cell.right() - 60; --x) {
        const QColor c = image.pixelColor(x, cell.top() + 2);
        dashes += c != image.pixelColor(x, plain.top() + 2) && std::abs(c.red() - c.blue()) < 20 && c.red() > 90;
    }
    EXPECT_GT(dashes, 15);
    EXPECT_LT(dashes, 45);
    // Its "-5" tag sits at the cell's outer (left) edge in grey glyphs from the tag strip.
    int ink = 0;
    for (int x = cell.left() + 3; x < cell.left() + 30; ++x)
        for (int y = cell.top() + 4; y < cell.bottom() - 4; ++y) {
            const QColor c = image.pixelColor(x, y);
            ink += c != image.pixelColor(x, plain.top() + (y - cell.top())) && std::abs(c.red() - c.blue()) < 20;
        }
    EXPECT_GT(ink, 8);
}

TEST(DomPulse, InPlaceTagDigitsMatchTheLadderSizeText) {
    for (double v : {0.0, 0.00005, 0.0001, 0.000123, 0.0032, 0.4, 0.634, 1.0, 2.5, 2.8, 6.8, 13.6, 100.0, 999.4,
                     999.5, 1360.0, 12345.0, 999'500.0, 2.5e9, 7.1e12}) {
        char buffer[DomModel::PulseTagCapacity];
        const int n = DomModel::formatSize(buffer, DomModel::PulseTagCapacity, v);
        EXPECT_EQ(QString::fromLatin1(buffer, n), DomModel::sizeText(v)) << v;
    }
}

TEST(DomPulse, TagAlphabetCoversEverythingFormatSizeEmits) {
    const std::string alphabet = DomModel::TagAlphabet;
    std::vector<double> values{0, 1e-5, 0.0001, 7.1e12, -7.1e12, 9.99e14, 1e300, -2.5};
    for (double v = 1e-4; v < 1e18; v *= 1.37) values.push_back(v);
    for (double v : values) {
        char buffer[DomModel::PulseTagCapacity];
        const int n = DomModel::formatSize(buffer, DomModel::PulseTagCapacity - 1, v);
        ASSERT_GT(n + (v == 0 ? 1 : 0), 0) << v;
        for (int i = 0; i < n; ++i) EXPECT_NE(alphabet.find(buffer[i]), std::string::npos) << v << " '" << buffer[i] << "'";
    }
    for (char prefix : {'+', '-', '\xD7'}) EXPECT_NE(alphabet.find(prefix), std::string::npos);
}

TEST_F(DomPulseDock, ExponentTagsRenderEveryGlyph) {
    dock.resize(650, 700); QCoreApplication::processEvents();
    source.update({{true, 199.9, 7.1e12}});
    for (int i = 0; i < 20; ++i) tick(); // the add fades
    source.update({{true, 199.9, 0}});
    tick();
    const int row = rowAt(*model, 1999);
    ASSERT_EQ(model->pulseTag(row, true, false), "-7.1e+03b");
    const QImage image = table->viewport()->grab().toImage();
    const QRect cell = table->visualRect(model->index(row, DomModel::Bid));
    const QRect plain = table->visualRect(model->index(rowAt(*model, 1990), DomModel::Bid));
    ASSERT_TRUE(image.rect().contains(plain));
    const int expected = QFontMetrics(table->font()).horizontalAdvance(model->pulseTag(row, true, false));
    int right = -1;
    for (int x = cell.left() + 3; x < std::min(cell.right(), cell.left() + 3 + expected + 12); ++x)
        for (int y = cell.top() + 4; y < cell.bottom() - 4; ++y)
            if (image.pixelColor(x, y) != image.pixelColor(x, plain.top() + (y - cell.top()))) right = x;
    const int glyph = QFontMetrics(table->font()).horizontalAdvance(QLatin1Char('e'));
    // Inked to within half a glyph of the full text: a dropped character would end a glyph short.
    EXPECT_GT(right - (cell.left() + 3) + 1, expected - glyph / 2);
}

TEST_F(DomPulseDock, FreshFlashesAndTheirPaintAllocateNothingAfterWarmUp) {
    // The dock's own model and delegate, driven directly (the dock tick also refreshes labels).
    // Every frame changes four rows. Pulse frames classify them (fresh added, pulled and eaten
    // flashes with new tags each frame); plain frames paint the same book cycle with no pulse.
    QImage image(table->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    auto* delegate = table->itemDelegate();
    QStyleOptionViewItem option;
    option.initFrom(table->viewport());
    option.font = table->font();
    option.fontMetrics = QFontMetrics(option.font);
    option.widget = table;
    DomTradeWindow none;
    qint64 wall = QDateTime::currentMSecsSinceEpoch(), t = 100000;
    std::array<sb::Trade, 1> received{};
    std::vector<QModelIndex> cells;
    std::vector<QRect> rects;
    for (int r = rowAt(*model, 2004); r <= rowAt(*model, 1995); ++r)
        for (int c : {int(DomModel::Bid), int(DomModel::Price), int(DomModel::Ask)}) {
            cells.push_back(model->index(r, c));
            rects.push_back(table->visualRect(cells.back()));
            if (c != DomModel::Price) ASSERT_GT(rects.back().width(), 90); // room for a tag beside the size
        }
    struct Count { size_t news = 0, mallocs = 0; };
    std::array<Count, 2> paint{}; // [plain, pulse]
    Count model_{};
    size_t freshTags = 0;
    const auto frame = [&](int i, bool counted, bool pulse) {
        const double k = 1 + i % 4;
        source.update({{true, 200, 10 + k}, {true, 199.9, 5 - k}, {false, 200.1, 10 - k}, {false, 200.2, 4 + k}});
        flushEvents();
        received[0] = {200.1, 0.5, sb::Aggressor::Buy, t + 1};
        wall += 67;
        t += 67;
        gAllocations = 0;
        gMallocs = 0;
        gCounting = counted;
        model->publish(source.book, none, true, wall);
        if (pulse) model->pulseStep(t, received);
        model_.news += gAllocations;
        model_.mallocs += gMallocs;
        gAllocations = 0;
        gMallocs = 0;
        for (size_t c = 0; c < cells.size(); ++c) {
            option.rect = rects[c];
            delegate->paint(&painter, option, cells[c]);
        }
        gCounting = false;
        paint[pulse].news += gAllocations;
        paint[pulse].mallocs += gMallocs;
        if (pulse && counted)
            for (const auto& cell : cells)
                for (bool bid : {true, false})
                    freshTags += cell.column() == DomModel::Price && model->pulse(cell.row(), bid).changeAlpha == 1;
    };
    for (int i = 0; i < 64; ++i) frame(i, false, true);
    if (kMallocCounted) { // the instrument sees Qt's own string allocations
        gMallocs = 0;
        gCounting = true;
        const QString probe = QString::number(1.25);
        gCounting = false;
        ASSERT_GT(gMallocs.load(), 0u);
    } else {
        std::cout << "malloc hook unavailable on this platform: Qt allocations unchecked\n";
    }
    model_ = {};
    paint = {};
    for (int i = 64; i < 96; ++i) frame(i, true, true);
    EXPECT_GE(freshTags, 32u * 3); // three or more fresh flashes in each pulse frame
    model->resetPulse();
    for (int i = 96; i < 128; ++i) frame(i, false, false); // warm the plain path too
    for (int i = 128; i < 160; ++i) frame(i, true, false);
    EXPECT_EQ(model_.news, 0u);
    EXPECT_EQ(model_.mallocs, 0u);
    // Delegate painting allocates inside Qt's style code on every frame (OB0 path); the pulse's
    // fills, outlines and tags must add nothing to it.
    EXPECT_EQ(paint[1].news, paint[0].news);
    EXPECT_EQ(paint[1].mallocs, paint[0].mallocs);
    std::cout << "PULSE_ALLOC per 32 frames: plain paint news=" << paint[0].news << " mallocs=" << paint[0].mallocs
              << " · pulse paint news=" << paint[1].news << " mallocs=" << paint[1].mallocs << '\n';
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
    double total = 0;
    for (int r = 0; r < model->rowCount(); ++r) total += vol(*model, r, DomModel::Buys) + vol(*model, r, DomModel::Sells);
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

TEST_F(DomDock, LeanTogglePersistsAndCollapsesAt180) {
    auto* toggle = dock.findChild<QToolButton*>("domLean");
    auto* strip = dock.findChild<QWidget*>("domLeanStrip");
    ASSERT_TRUE(toggle->isChecked());
    show(); EXPECT_TRUE(strip->isVisible()); EXPECT_EQ(strip->height(), strip->sizeHint().height());
    toggle->setChecked(false); EXPECT_TRUE(strip->isHidden());
    { OrderBookDock reopened(nullptr, &source); EXPECT_FALSE(reopened.findChild<QToolButton*>("domLean")->isChecked()); }
    toggle->setChecked(true);
    { OrderBookDock reopened(nullptr, &source); EXPECT_TRUE(reopened.findChild<QToolButton*>("domLean")->isChecked()); }
    dock.resize(180, 600); QCoreApplication::processEvents();
    EXPECT_EQ(dock.width(), 180); EXPECT_TRUE(strip->isHidden());
    const auto narrowSteps = dock.leanSteps(); QTest::qWait(90); EXPECT_EQ(dock.leanSteps(), narrowSteps);
    EXPECT_EQ(table->horizontalScrollBar()->maximum(), 0);
    for (const char* name : {"domLean", "domPulse", "domCumulative", "domRecenter"}) {
        auto* button = dock.findChild<QWidget*>(name);
        EXPECT_GE(button->mapTo(&dock, QPoint()).x(), 0);
        EXPECT_LE(button->mapTo(&dock, QPoint(button->width(), 0)).x(), 180);
    }
    EXPECT_TRUE(dock.findChild<QLabel*>("domSymbol")->isVisible());
    dock.resize(650, 600); QCoreApplication::processEvents(); EXPECT_TRUE(strip->isVisible());
    for (const char* name : {"domLean", "domPulse", "domCumulative", "domRecenter"})
        EXPECT_TRUE(dock.findChild<QWidget*>(name)->styleSheet().isEmpty());
    EXPECT_GT(dock.findChild<QLabel*>("domSymbol")->parentWidget()->layout()->spacing(), 0);
}

TEST_F(DomDock, LeanDarkThemeLayoutAndOptionalSnapshots) {
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme{}.stylesheet());
    source.update({{true, 200, 3}, {false, 200, 1}, {true, 195, 5}, {false, 300, 50}});
    dock.resize(400, 600); show();
    auto* strip = dock.findChild<QWidget*>("domLeanStrip");
    EXPECT_TRUE(strip->isVisible()); EXPECT_EQ(dock.width(), 400);
    // Optional diagnostic artifacts stay outside the checkout's tracked sources.
    const QString directory = qEnvironmentVariable("DOM_LEAN_SHOT_DIR");
    if (!directory.isEmpty()) EXPECT_TRUE(dock.grab().save(directory + "/lean-wide-synthetic.png"));
    EXPECT_EQ(dock.leanGauge().state(), sentinel::book::LeanGauge::State::TwoSided);
    source.update({{true, 200.1, 7}}); flushEvents();
    QMetaObject::invokeMethod(dock.findChild<QTimer*>("domPublishTimer"), "timeout", Qt::DirectConnection);
    EXPECT_EQ(dock.leanGauge().state(), sentinel::book::LeanGauge::State::Crossed);
    if (!directory.isEmpty()) EXPECT_TRUE(dock.grab().save(directory + "/lean-crossed-synthetic.png"));
    dock.resize(180, 600); QCoreApplication::processEvents();
    EXPECT_EQ(dock.width(), 180); EXPECT_TRUE(strip->isHidden());
    EXPECT_EQ(table->horizontalScrollBar()->maximum(), 0);
    if (!directory.isEmpty()) EXPECT_TRUE(dock.grab().save(directory + "/lean-narrow-synthetic.png"));
    dock.resize(600, 600);
    source.update({{true, 200.1, 0}}); flushEvents();
    const QFont previousFont = qApp->font();
    QFont large = previousFont; large.setPixelSize(18); qApp->setFont(large);
    qApp->setStyleSheet(qApp->styleSheet()); // FontManager refreshes the stylesheet after changing the app font.
    QCoreApplication::processEvents();
    QMetaObject::invokeMethod(dock.findChild<QTimer*>("domPublishTimer"), "timeout", Qt::DirectConnection);
    EXPECT_EQ(strip->font().pixelSize(), 18);
    if (!directory.isEmpty()) EXPECT_TRUE(dock.grab().save(directory + "/lean-large-synthetic.png"));
    qApp->setFont(previousFont);
    qApp->setStyleSheet(previousStyle);
}

TEST_F(DomDock, LeanOnlyWorksOnExposedTicksAndResets) {
    source.update({{true, 199, 3}, {false, 201, 1}});
    flushEvents(); EXPECT_EQ(dock.leanSteps(), 0u);
    show(); ASSERT_GT(dock.leanSteps(), 0u);
    EXPECT_NEAR(dock.leanGauge().bands()[0].imbalance, .5, 1e-12);
    auto steps = dock.leanSteps();
    dock.findChild<QPushButton*>("domRecenter")->click();
    EXPECT_EQ(dock.leanSteps(), steps); // Recenter is not a Lean sample tick.
    source.update({{true, 199, 7}}); flushEvents(); EXPECT_EQ(dock.leanSteps(), steps);
    dock.hide(); QTest::qWait(100);
    QMetaObject::invokeMethod(dock.findChild<QTimer*>("domPublishTimer"), "timeout", Qt::DirectConnection);
    EXPECT_EQ(dock.leanSteps(), steps);
    EXPECT_EQ(dock.leanGauge().historySize(), 0u);
    show(); auto* toggle = dock.findChild<QToolButton*>("domLean"); toggle->setChecked(false);
    steps = dock.leanSteps(); QTest::qWait(100); EXPECT_EQ(dock.leanSteps(), steps);
    toggle->setChecked(true); QTest::qWait(90); EXPECT_GT(dock.leanSteps(), steps);
    dock.onSymbolChanged("ETH-USD"); EXPECT_EQ(dock.leanGauge().historySize(), 0u);
    dock.onSymbolChanged("BTC-USD"); QTest::qWait(90);
    source.book.clear(); emit source.liveOrderBookUpdated("BTC-USD", {}); flushEvents();
    EXPECT_EQ(dock.leanGauge().historySize(), 0u);
    QTest::qWait(90); EXPECT_EQ(dock.leanGauge().state(), sentinel::book::LeanGauge::State::Empty);
}

TEST_F(DomDock, LeanIgnoresPinnedLadderAndUsesNativeTick) {
    source.update({{true, 199, 3}, {false, 201, 1}, {true, 195, 5}, {false, 300, 50}});
    show();
    QWheelEvent wheel(QPointF(20, 20), table->viewport()->mapToGlobal(QPoint(20, 20)),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(table->viewport(), &wheel);
    table->verticalScrollBar()->setValue(0);
    const auto steps = dock.leanSteps();
    source.update({{true, 195, 11}, {false, 300, 80}});
    flushEvents();
    QMetaObject::invokeMethod(dock.findChild<QTimer*>("domPublishTimer"), "timeout", Qt::DirectConnection);
    EXPECT_EQ(dock.leanSteps(), steps + 1);
    EXPECT_DOUBLE_EQ(dock.leanGauge().bands()[0].bid, 14);
    EXPECT_DOUBLE_EQ(dock.leanGauge().bands()[3].ask, 81);
    EXPECT_TRUE(dock.leanGauge().disagreement());
}

TEST(DomLean, ChangingDataAndPaintAllocateNothingAfterWarmup) {
    std::array<double, 401> bids{}, asks{};
    sentinel::book::LeanGauge gauge;
    LeanGaugeWidget widget(gauge); widget.resize(400, widget.sizeHint().height());
    const QFont originalFont = widget.font();
    for (int pixelSize : {0, 20}) {
        QFont selected = originalFont;
        if (pixelSize) selected.setPixelSize(pixelSize);
        widget.setFont(selected);
        QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        const auto frame = [&](int i) {
            bids[199] = 1 + i % 37; asks[201] = 1 + i % 11;
            bids[100] = 30; asks[300] = 40;
            gauge.update({bids, asks, 0, 1, 199, 201}, 0, i * 500);
            widget.paint(painter);
        };
        for (int i = 0; i < 300; ++i) frame(i);
        gAllocations = 0; gMallocs = 0; gCounting = true;
        for (int i = 300; i < 332; ++i) frame(i);
        gCounting = false;
        EXPECT_EQ(gAllocations, 0u); EXPECT_EQ(gMallocs, 0u);
        std::cout << "LEAN_ALLOC 32 changed frames: new=" << gAllocations << " malloc=" << gMallocs << '\n';
    }
    if (kMallocCounted) {
        gMallocs = 0; gCounting = true;
        QString probe(1000, QLatin1Char('x'));
        gCounting = false; EXPECT_GT(gMallocs, 0u); EXPECT_EQ(probe.size(), 1000);
    }
}

TEST(DomLean, InheritsLargeFontAndMarksCrossedValuesStale) {
    std::array<double, 401> bids{}, asks{};
    bids[199] = 3; asks[201] = 1; asks[300] = 10;
    sentinel::book::LeanGauge gauge;
    gauge.update({bids, asks, 0, 1, 199, 201}, 0, 0);
    QWidget parent;
    QFont large = parent.font(); large.setPixelSize(20); parent.setFont(large);
    LeanGaugeWidget widget(gauge, &parent); widget.resize(500, widget.sizeHint().height());
    EXPECT_EQ(widget.font(), parent.font());
    const auto render = [&] {
        QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::black);
        QPainter painter(&image); widget.paint(painter); return image;
    };
    const QImage fresh = render();
    const QFontMetrics fm(large);
    const int left = 250 - fm.horizontalAdvance("-100%") - 4;
    int first = fm.height(), last = -1;
    for (int y = 0; y < fm.height(); ++y)
        for (int x = left; x < 250; ++x)
            if (fresh.pixelColor(x, y).lightness() > 150) { first = std::min(first, y); last = y; }
    EXPECT_GE(last - first + 1, fm.capHeight() - 2);
    EXPECT_GT(widget.height(), 78);
    gauge.update({bids, asks, 0, 1, 202, 201}, 0, 500);
    ASSERT_EQ(gauge.state(), sentinel::book::LeanGauge::State::Crossed);
    const QImage stale = render();
    const QRect status(2, 2 * std::max(14, fm.height()) + 14, 230, fm.height());
    EXPECT_NE(fresh.copy(status), stale.copy(status)); // stale must override near/far disagreement
    EXPECT_EQ(fresh.copy(0, 0, 500, 2 * fm.height()), stale.copy(0, 0, 500, 2 * fm.height()));
}

TEST(DomLean, PaintMirrorsBidLeftAskRightAndDrawsHistory) {
    std::array<double, 401> bids{}, asks{};
    bids[199] = 3; asks[201] = 1; asks[300] = 10;
    sentinel::book::LeanGauge gauge;
    for (int i = 0; i < 241; ++i) gauge.update({bids, asks, 0, 1, 199, 201}, 0, i * 500);
    LeanGaugeWidget widget(gauge); widget.resize(400, widget.sizeHint().height());
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::black);
    QPainter painter(&image); widget.paint(painter); painter.end();
    const QColor bid = DomBarDelegate::barColor(true, 1), ask = DomBarDelegate::barColor(false, 1);
    int brightDigits = 0;
    for (int y = 0; y < 13; ++y)
        for (int x = 163; x < 199; ++x) brightDigits += image.pixelColor(x, y).lightness() > 150;
    EXPECT_GT(brightDigits, 20);
    const QFontMetrics fm(widget.font());
    const int row = std::max(14, fm.height());
    const int label = fm.horizontalAdvance("±100") + 4, value = fm.horizontalAdvance("-100%") + 4;
    const int center = label + (200 - label - value) / 2;
    EXPECT_EQ(image.pixelColor(center - 10, row / 2), bid);
    EXPECT_EQ(image.pixelColor(center + 210, row + row / 2), ask);
    const auto countColour = [&](const QRect& area, const QColor& colour) {
        int count = 0;
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) count += image.pixelColor(x, y) == colour;
        return count;
    };
    EXPECT_GT(countColour(QRect(0, 2 * row, 200, 14), bid), 100);
    EXPECT_EQ(countColour(QRect(200, 2 * row, 200, 14), bid), 0);
    EXPECT_GT(countColour(QRect(200, 2 * row, 200, 14), ask), 100);
    EXPECT_GT(countColour(QRect(100, 3 * row + 14, 297, std::max(16, row)), bid), 200);
}

// ---- Row grouping (client side) ----
// Native 0.1 levels 199.9, 200.0, 200.9, 201.0 sit in $1 rows 199, 200, 200, 201:
// absolute bins [k, k+1), lower inclusive, never rounded across a boundary.
TEST(DomGroup, AbsoluteGridBinsVolumesCumulativeAndLabels) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 199.9, 1}, {true, 200.0, 2}, {true, 199.5, 4}, {false, 200.9, 8}, {false, 201.0, 16}, {false, 200.1, 32}});
    DomTradeWindow trades;
    trades.ingest(trade(200.05, AggressorSide::Buy));
    trades.ingest(trade(200.95, AggressorSide::Buy, 2));
    trades.ingest(trade(201.0, AggressorSide::Sell, 4));
    DomModel model;
    model.clear("BTC-USD");
    model.setGroup(10);
    model.publish(book, trades, true);
    ASSERT_EQ(model.group(), 10);
    EXPECT_DOUBLE_EQ(model.tick(), 1.0);
    const auto q = [&](qint64 key, int col) { return model.index(rowAt(model, key), col).data(DomModel::QuantityRole).toDouble(); };
    EXPECT_DOUBLE_EQ(q(199, DomModel::Bid), 5);  // 199.5 + 199.9
    EXPECT_DOUBLE_EQ(q(200, DomModel::Bid), 2);  // 200.0 opens row 200
    EXPECT_DOUBLE_EQ(q(200, DomModel::Ask), 40); // 200.1 + 200.9
    EXPECT_DOUBLE_EQ(q(201, DomModel::Ask), 16); // 201.0 opens row 201
    EXPECT_DOUBLE_EQ(q(200, DomModel::Buys), 3); // 200.05 and 200.95
    EXPECT_DOUBLE_EQ(q(201, DomModel::Sells), 4);
    EXPECT_DOUBLE_EQ(q(200, DomModel::Sells), 0);
    // Σ over grouped rows from the touch outward.
    const auto cum = [&](qint64 key, int col) { return model.index(rowAt(model, key), col).data(DomModel::CumulativeRole).toDouble(); };
    EXPECT_DOUBLE_EQ(cum(200, DomModel::Bid), 2);
    EXPECT_DOUBLE_EQ(cum(199, DomModel::Bid), 7);
    EXPECT_DOUBLE_EQ(cum(200, DomModel::Ask), 40);
    EXPECT_DOUBLE_EQ(cum(201, DomModel::Ask), 56);
    // The price column shows each row's lower edge at the row precision.
    EXPECT_EQ(model.index(rowAt(model, 200), DomModel::Price).data().toString(), "200");
    model.setGroup(25); // $2.5 rows: one decimal
    model.publish(book, trades, true);
    EXPECT_EQ(model.index(rowAt(model, 80), DomModel::Price).data().toString(), "200.0");
    EXPECT_DOUBLE_EQ(q(80, DomModel::Bid), 2);
    EXPECT_DOUBLE_EQ(q(79, DomModel::Bid), 5);
    // Best cells keep their filled marking in whichever row holds the touch.
    EXPECT_EQ(model.index(rowAt(model, 80), 0).data(DomModel::BestSideRole).toString(), "Bid / Ask");
}

TEST(DomGroup, PanningAnUnchangedBookKeepsTheBinsAndCostStaysNearTheView) {
    // Prices high enough that a $10 window does not touch zero (the window top is at least row 2000).
    LiveOrderBook book;
    book.initialize(19000, 21000, 0.1);
    for (int i = 1; i <= 300; ++i) apply(book, {{true, 20000 - i * 0.1, double(i)}, {false, 20000 + i * 0.1, double(i)}});
    DomModel model;
    DomTradeWindow trades;
    model.setGroup(10);
    model.publish(book, trades, true);
    const int touch = rowAt(model, 19999);
    model.setVisibleRows(touch - 20, touch + 20);
    model.publish(book, trades, false);
    std::vector<double> before;
    for (qint64 key = 19975; key <= 20025; ++key) before.push_back(model.index(rowAt(model, key), DomModel::Bid).data(DomModel::QuantityRole).toDouble()
                                                               + model.index(rowAt(model, key), DomModel::Ask).data(DomModel::QuantityRole).toDouble());
    model.setVisibleRows(touch - 16, touch + 24); // scroll four rows
    model.publish(book, trades, false);
    for (qint64 key = 19975; key <= 20025; ++key)
        EXPECT_DOUBLE_EQ(model.index(rowAt(model, key), DomModel::Bid).data(DomModel::QuantityRole).toDouble()
                         + model.index(rowAt(model, key), DomModel::Ask).data(DomModel::QuantityRole).toDouble(), before[size_t(key - 19975)]) << key;
    // $10 rows read the rows on screen plus a margin, not the 2,001-row window.
    model.setGroup(100);
    model.publish(book, trades, true);
    const int c = model.centerRow();
    model.setVisibleRows(c - 20, c + 20);
    model.publish(book, trades, true);
    EXPECT_LT(model.filledLast() - model.filledFirst() + 1, 200);
    EXPECT_LE(model.filledFirst(), c - 20 - 8);
    EXPECT_GE(model.filledLast(), c + 20 + 8);
    EXPECT_LE(model.lastGroupLevels(), qint64(model.filledLast() - model.filledFirst() + 1) * 100);
    EXPECT_LT(model.lastGroupLevels(), 20000);
}

TEST(DomGroup, PinnedLevelsAboveTheFilledRowsStillCountInTheSum) {
    LiveOrderBook book;
    book.initialize(0, 5000, 0.1);
    apply(book, {{true, 3000, 5}, {true, 2900, 3}, {true, 2000, 7}, {false, 3000.1, 1}});
    DomModel model;
    DomTradeWindow trades;
    model.setGroup(100);
    model.publish(book, trades, true);
    // Scroll 80 $10 rows below the touch: the filled rows no longer reach it.
    const int row = rowAt(model, 220);
    model.setVisibleRows(row - 10, row + 10);
    model.publish(book, trades, false);
    ASSERT_GT(rowAt(model, 300), -1);
    ASSERT_LT(rowAt(model, 300), model.filledFirst());
    EXPECT_DOUBLE_EQ(model.index(row, DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 8);
    EXPECT_DOUBLE_EQ(model.index(rowAt(model, 200), DomModel::Bid).data(DomModel::CumulativeRole).toDouble(), 15);
}

TEST(DomGroup, ChoicesAreTheHeatmapPresetsThatAreWholeMultiples) {
    const auto list = [](const dom::GroupChoices& c) { return std::vector<int64_t>(c.list().begin(), c.list().end()); };
    EXPECT_EQ(list(dom::groupChoices(0.1, 110000)), (std::vector<int64_t>{1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000}));
    EXPECT_EQ(list(dom::groupChoices(0.1, 200)), (std::vector<int64_t>{1, 2, 5, 10, 20})); // 1% of the mid
    EXPECT_EQ(list(dom::groupChoices(0.3, 1000)), (std::vector<int64_t>{1}));
    EXPECT_EQ(list(dom::groupChoices(0.01, 4000)), (std::vector<int64_t>{1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000}));
    const auto choices = dom::groupChoices(0.1, 110000);
    EXPECT_EQ(dom::linkGroup(1.0, 0.1, choices).group, 10);
    EXPECT_TRUE(dom::linkGroup(1.0, 0.1, choices).exact);
    EXPECT_EQ(dom::linkGroup(0.15, 0.1, choices).group, 2); // not a whole multiple: nearest
    EXPECT_FALSE(dom::linkGroup(0.15, 0.1, choices).exact);
    EXPECT_EQ(dom::linkGroup(0.3, 0.1, choices).group, 2); // a multiple the list does not offer
    EXPECT_FALSE(dom::linkGroup(0.3, 0.1, choices).exact);
    EXPECT_EQ(dom::linkGroup(0, 0.1, choices).group, 1);
}

TEST(DomGroup, AutoPicksTheSmallestFilledSizeOnScriptedBooks) {
    const auto choices = dom::groupChoices(0.1, 110000);
    std::vector<double> bids(20000), asks(20000);
    const auto target = [&](std::ptrdiff_t bidTop, std::ptrdiff_t askTop) {
        return dom::AutoGroupRule::target({bids, asks, 1'000'000, bidTop, askTop}, choices);
    };
    // Thick: every native level on both sides.
    for (int i = 0; i < 2000; ++i) { bids[10000 - i] = 1; asks[10001 + i] = 1; }
    EXPECT_EQ(target(10000, 10001), 1);
    // Thin: a level every 0.5 -> $0.5 rows are the first that are 60% full.
    std::fill(bids.begin(), bids.end(), 0); std::fill(asks.begin(), asks.end(), 0);
    for (int i = 0; i < 2000; i += 5) { bids[10000 - i] = 1; asks[10000 + i] = 1; }
    EXPECT_EQ(target(10000, 10000), 5);
    // Per side: thick bids, asks every $1 -> the asks decide ($1).
    for (int i = 0; i < 2000; ++i) bids[10000 - i] = 1;
    std::fill(asks.begin(), asks.end(), 0);
    for (int i = 0; i < 4000; i += 10) asks[10001 + i] = 1;
    EXPECT_EQ(target(10000, 10001), 10);
    // One-sided: only the present side counts.
    EXPECT_EQ(target(10000, -1), 1);
    // Exactly 12 of 20 rows is enough; 11 is not.
    std::fill(bids.begin(), bids.end(), 0);
    for (int j = 0; j < 12; ++j) bids[10000 - j] = 1;
    EXPECT_EQ(dom::AutoGroupRule::filledRows({bids, asks, 1'000'000, 10000, -1}, true, 1), 12);
    EXPECT_EQ(target(10000, -1), 1);
    bids[10000 - 11] = 0;
    EXPECT_GT(target(10000, -1), 1);
    // Hysteresis: 13 of every 20 levels (65%). From $0.2 rows the native size needs 14 (70%).
    std::fill(bids.begin(), bids.end(), 0);
    for (int j = 0; j < 2000; ++j) bids[10000 - j] = j % 20 < 13 ? 1 : 0;
    EXPECT_EQ(dom::AutoGroupRule::target({bids, asks, 1'000'000, 10000, -1}, choices, 1), 1);
    EXPECT_EQ(dom::AutoGroupRule::target({bids, asks, 1'000'000, 10000, -1}, choices, 2), 2);
    bids[10000 - 13] = 1;
    EXPECT_EQ(dom::AutoGroupRule::target({bids, asks, 1'000'000, 10000, -1}, choices, 2), 1);
    // Nothing anywhere near: the coarsest; an empty book has no target.
    std::fill(bids.begin(), bids.end(), 0);
    bids[100] = 1;
    EXPECT_EQ(target(100, -1), 1000);
    EXPECT_EQ(target(-1, -1), 0);
}

TEST(DomGroup, HoldStopsFlipFlopAndAppliesASteadyChange) {
    dom::GroupHold hold;
    EXPECT_EQ(hold.step(5, 0), 5); // first target at once
    for (qint64 t = 300; t <= 10000; t += 300) EXPECT_EQ(hold.step((t / 300) % 2 ? 10 : 5, t), 5) << t;
    EXPECT_EQ(hold.changes(), 0u);
    EXPECT_EQ(hold.step(5, 10100), 5);
    EXPECT_EQ(hold.step(10, 20000), 5);
    EXPECT_EQ(hold.step(10, 20000 + dom::GroupHold::HoldMs - 1), 5);
    EXPECT_EQ(hold.step(10, 20000 + dom::GroupHold::HoldMs), 10);
    EXPECT_EQ(hold.changes(), 1u);
    EXPECT_EQ(hold.step(0, 30000), 10); // no evidence keeps the size
}

TEST(DomGroup, GroupingChangeKeepsThePinnedPriceAndRebaselinesPulse) {
    PulseRig rig;
    rig.book.initialize(19000, 21000, 0.1); // a $1 window must not reach zero
    // Every level from 19990 to 20010 rests, with sizes that differ level to level.
    for (int i = 0; i < 100; ++i)
        apply(rig.book, {{true, 20000 - i * 0.1, 1.0 + i % 3}, {false, 20000.1 + i * 0.1, 1.0 + i % 4}});
    rig.step();
    const int center = rig.model.centerRow();
    rig.model.setVisibleRows(center - 40, center + 40); // pinned on the touch
    rig.step({}, false);
    const int middle = center;
    const double before = rig.model.index(middle, 0).data(DomModel::BucketRole).toLongLong() * rig.model.tick();
    // $0.2 rows: the grouped rows overlap the native prices, so a classifier fed them as
    // if nothing changed would see different sizes at the same prices.
    rig.model.setGroup(2);
    ASSERT_TRUE(rig.model.needsRepublish(rig.wall + 67));
    rig.step({}, false);
    ASSERT_EQ(rig.model.group(), 2);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u) << "a new grid is a fresh baseline";
    const double after = rig.model.index(middle, 0).data(DomModel::BucketRole).toLongLong() * rig.model.tick();
    EXPECT_LE(after, before + 1e-9);
    EXPECT_GT(after, before - 0.2 - 1e-9); // the same price, now its row's lower edge
    rig.model.setGroup(10);
    rig.step({}, false);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    const double ten = rig.model.index(middle, 0).data(DomModel::BucketRole).toLongLong() * rig.model.tick();
    EXPECT_LE(ten, before + 1e-9);
    EXPECT_GT(ten, before - 1.0);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u) << "a new grid is a fresh baseline";
    rig.step({}, false);
    EXPECT_EQ(rig.model.lastPulseEvents(), 0u);
    apply(rig.book, {{true, 19999.8, 9}}); // a real change on the grouped rows still classifies
    rig.step({}, false);
    EXPECT_EQ(rig.model.lastPulseEvents(), 1u);
}

TEST(DomGroup, HeaderSpreadIsTheNativeSpreadUnderGrouping) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    apply(book, {{true, 200, 4}, {false, 200.1, 7}});
    DomModel model;
    model.clear("BTC-USD");
    model.setGroup(100);
    model.publish(book, {}, true);
    EXPECT_TRUE(model.summary().contains("Bid 200.0 · Ask 200.1 · Bucketed spread 0.1 USD")) << model.summary().toStdString();
    EXPECT_EQ(model.summary(true), "Spr 0.1");
    EXPECT_TRUE(model.aggregation(true).startsWith("Rows 10 USD · tick 0.1")) << model.aggregation(true).toStdString();
}

TEST(DomGroup, GroupedPublishAndStepAllocateNothingAfterWarmUp) {
    PulseRig rig;
    std::array<sb::Trade, 1> received{};
    std::vector<std::pair<uint32_t, double>> topBids, topAsks;
    topBids.reserve(1); topAsks.reserve(1);
    dom::GroupHold hold;
    int64_t group = 10;
    const auto cycle = [&](int i) {
        const auto top = rig.book.captureDenseNonZero(topBids, topAsks, 1);
        const auto choices = dom::groupChoices(top.tickSize, 200);
        const dom::AutoBook view{rig.book.getBids(), rig.book.getAsks(), 0,
            topBids.empty() ? -1 : std::ptrdiff_t(topBids.front().first), topAsks.empty() ? -1 : std::ptrdiff_t(topAsks.front().first)};
        (void)hold.step(dom::AutoGroupRule::target(view, choices), rig.t);
        rig.model.setGroup(i % 9 == 0 ? (group = group == 10 ? 20 : 10) : group);
        received[0] = rig.received(200.1, 0.5, sb::Aggressor::Buy);
        rig.step(std::span(received.data(), 1), i % 7 != 0);
        (void)rig.model.needsRepublish(rig.wall);
    };
    const int c = rig.model.centerRow();
    rig.model.setVisibleRows(c - 20, c + 20);
    for (int i = 0; i < 60; ++i) cycle(i);
    std::vector<std::array<BookLevelUpdate, 2>> changes;
    for (int i = 0; i < 60; ++i) changes.push_back({BookLevelUpdate{false, 200.1, 10.0 - i % 3}, BookLevelUpdate{true, 199.9, 4.0 + i % 5}});
    size_t news = 0, mallocs = 0;
    for (int i = 0; i < 60; ++i) {
        rig.book.applyUpdates(std::span(changes[i]), std::chrono::system_clock::now(), nullptr);
        gAllocations = 0; gMallocs = 0; gCounting = true;
        cycle(i);
        gCounting = false;
        news += gAllocations; mallocs += gMallocs;
    }
    EXPECT_EQ(news, 0u);
    EXPECT_EQ(mallocs, 0u);
}

TEST_F(DomDock, GroupingMenuFixedPersistsAndLinkFollowsTheHeatmapTick) {
    qint64 clock = 1000;
    dock.setPulseClock([&] { return clock; });
    source.update({{true, 200, 1}, {true, 199.9, 2}, {false, 200.1, 1}});
    show();
    auto* timer = dock.findChild<QTimer*>("domPublishTimer");
    timer->stop();
    const auto tick = [&](qint64 ms = 67) { flushEvents(); clock += ms; QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection); };
    auto* button = dock.findChild<QToolButton*>("domGrouping");
    ASSERT_NE(button, nullptr);
    auto* menu = dock.findChild<QMenu*>("domGroupingMenu");
    emit menu->aboutToShow();
    QAction* dollar = nullptr;
    for (QAction* a : menu->actions()) if (a->text() == "1") dollar = a;
    ASSERT_NE(dollar, nullptr);
    dollar->trigger();
    tick();
    EXPECT_EQ(dock.grouping(), OrderBookDock::Grouping::Fixed);
    EXPECT_EQ(model->group(), 10);
    EXPECT_EQ(button->text(), "Fixed ▾");
    EXPECT_TRUE(dock.findChild<QLabel*>("domAggregation")->text().startsWith("Rows 1 USD"));
    {
        Source other;
        OrderBookDock reopened{nullptr, &other};
        EXPECT_EQ(reopened.grouping(), OrderBookDock::Grouping::Fixed);
        EXPECT_DOUBLE_EQ(reopened.fixedGroupTick(), 1.0);
    }
    dock.setGrouping(OrderBookDock::Grouping::Link);
    dock.setHeatmapTick(1.0, true);
    tick();
    EXPECT_EQ(model->group(), 10);
    dock.setHeatmapTick(2.0, true); // a locked heatmap tick applies at once
    tick();
    EXPECT_EQ(model->group(), 20);
    EXPECT_EQ(button->text(), "Link ▾");
    EXPECT_TRUE(button->toolTip().startsWith("Row size 2 (20 server ticks)")) << button->toolTip().toStdString();
    dock.setHeatmapTick(0.5, false); // heatmap Auto: the same hold
    tick();
    EXPECT_EQ(model->group(), 20);
    tick(dom::GroupHold::HoldMs + 10);
    EXPECT_EQ(model->group(), 5);
    dock.setHeatmapTick(0.15, true); // not a whole multiple (heatmap layer off): nearest, said in the tooltip
    tick();
    EXPECT_EQ(model->group(), 2);
    EXPECT_TRUE(button->toolTip().contains("not a whole multiple")) << button->toolTip().toStdString();
    dock.setGrouping(OrderBookDock::Grouping::Auto);
    tick();
    EXPECT_EQ(dock.grouping(), OrderBookDock::Grouping::Auto);
    EXPECT_EQ(button->text(), "Auto ▾");
}

TEST_F(DomDock, CheckedTogglesLookDifferentInTheDarkTheme) {
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme{}.stylesheet());
    show();
    for (const char* name : {"domPulse", "domLean", "domCumulative"}) {
        auto* button = dock.findChild<QToolButton*>(name);
        const bool was = button->isChecked(); // persisted: restore it for later tests
        button->setChecked(false);
        const QImage off = button->grab().toImage();
        button->setChecked(true);
        const QImage on = button->grab().toImage();
        const QPoint corner(3, button->height() / 2); // background beside the text
        EXPECT_NE(off.pixelColor(corner), on.pixelColor(corner)) << name;
        button->setChecked(was);
    }
    qApp->setStyleSheet(previousStyle);
}

TEST_F(DomDock, HeadingRestylesOnlyWhenCompactModeFlips) {
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme{}.stylesheet());
    show();
    struct Counter : QObject {
        int styles = 0;
        bool eventFilter(QObject*, QEvent* e) override { styles += e->type() == QEvent::StyleChange; return false; }
    } counter;
    auto* pulse = dock.findChild<QToolButton*>("domPulse");
    pulse->installEventFilter(&counter);
    for (int width : {640, 600, 520, 450}) { dock.resize(width, 600); QCoreApplication::processEvents(); }
    EXPECT_EQ(counter.styles, 0) << "wide resizes keep the styles";
    dock.resize(180, 600); QCoreApplication::processEvents();
    EXPECT_GT(counter.styles, 0);
    EXPECT_FALSE(pulse->styleSheet().isEmpty());
    const int compact = counter.styles;
    for (int width : {185, 190, 182}) { dock.resize(width, 600); QCoreApplication::processEvents(); }
    EXPECT_EQ(counter.styles, compact);
    qApp->setStyleSheet(previousStyle);
}

TEST_F(DomDock, HeadingButtonsKeepTheirWholeTextFrom180To650) {
    const QString previousStyle = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme{}.stylesheet());
    source.update({{true, 200, 1}, {false, 200.1, 1}});
    show();
    for (const auto& [mode, tick] : {std::pair{OrderBookDock::Grouping::Fixed, 0.1}, std::pair{OrderBookDock::Grouping::Fixed, 1.0},
                                     std::pair{OrderBookDock::Grouping::Auto, 0.0}}) {
        dock.setGrouping(mode, tick);
        for (int width : {650, 400, 260, 220, 180}) {
            dock.resize(width, 600);
            QCoreApplication::processEvents();
            ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "refreshDisplay", Qt::DirectConnection));
            QCoreApplication::processEvents();
            for (const char* name : {"domGrouping", "domPulse", "domLean", "domCumulative", "domRecenter", "domSymbol"}) {
                if (QString(name) == "domSymbol") continue; // the symbol yields its width
                auto* button = dock.findChild<QWidget*>(name);
                EXPECT_GE(button->width(), button->sizeHint().width()) << name << " at " << width;
                EXPECT_LE(button->mapTo(&dock, QPoint(button->width(), 0)).x(), width) << name << " at " << width;
            }
            EXPECT_TRUE(dock.findChild<QToolButton*>("domGrouping")->toolTip().startsWith(
                mode == OrderBookDock::Grouping::Auto ? "Row size" : tick == 1.0 ? "Row size 1 (10" : "Row size 0.1 (1"))
                << dock.findChild<QToolButton*>("domGrouping")->toolTip().toStdString();
        }
    }
    qApp->setStyleSheet(previousStyle);
}

TEST_F(DomPulseDock, FooterNeverCutsANumberAt180To400) {
    source.update({{true, 199.9, 0}, {false, 200.2, 1364.2}}); // a pull and a large add
    tick();
    source.update({{false, 200.2, 0}});
    tick();
    auto* footer = dock.findChild<QLabel*>("domPulseFooter");
    for (int width = 180; width <= 400; width += 20) {
        dock.resize(width, 600);
        QCoreApplication::processEvents();
        tick();
        const QString text = footer->text();
        EXPECT_LE(footer->fontMetrics().horizontalAdvance(text), footer->width()) << width << " " << text.toStdString();
        EXPECT_FALSE(text.isEmpty());
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings; // never the owner's settings
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
