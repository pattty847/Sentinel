// Learning-mode contracts run with DomTests' isolated QSettings and offscreen QApplication.
#include "config/TooltipHelp.hpp"
#include "mainwindow/MenuBuilder.h"
#include "models/DomModel.hpp"
#include "widgets/OrderBookDock.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/WatchlistDock.hpp"
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTableView>
#include <QToolButton>
#include <gtest/gtest.h>

namespace {
class TooltipHelpTest : public testing::Test {
protected:
    void SetUp() override { TooltipHelp::instance().setExplanatory(false); }
    void TearDown() override { TooltipHelp::instance().setExplanatory(false); }
    static QAction* toggle(QMenuBar& bar) {
        MenuBuilder builder(&bar);
        builder.buildMenus({}, {});
        for (auto* action : bar.findChildren<QAction*>())
            if (action->text() == "Show explanatory tooltips") return action;
        return nullptr;
    }
};

TEST_F(TooltipHelpTest, DefaultIsOff) {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    settings.remove("ui/showExplanatoryTooltips");
    TooltipHelp reconstructed;
    EXPECT_FALSE(reconstructed.explanatory());
    EXPECT_EQ(reconstructed.text("value", "explanation"), "value");
    QMenuBar bar;
    auto* action = toggle(bar);
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isCheckable());
    EXPECT_FALSE(action->isChecked());
}

TEST_F(TooltipHelpTest, MenuChoicePersistsAcrossReconstructionInBothDirections) {
    QMenuBar bar;
    auto* action = toggle(bar);
    ASSERT_NE(action, nullptr);
    action->trigger();
    {
        TooltipHelp reconstructed;
        EXPECT_TRUE(reconstructed.explanatory());
    }
    QMenuBar secondBar;
    auto* secondAction = toggle(secondBar);
    ASSERT_NE(secondAction, nullptr);
    EXPECT_TRUE(secondAction->isChecked());
    secondAction->trigger();
    EXPECT_FALSE(action->isChecked());
    TooltipHelp reconstructed;
    EXPECT_FALSE(reconstructed.explanatory());
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    ASSERT_TRUE(settings.contains("ui/showExplanatoryTooltips"));
    EXPECT_FALSE(settings.value("ui/showExplanatoryTooltips").toBool());
}

TEST_F(TooltipHelpTest, DomValuesStayShortAndLearningRestoresOriginalBytes) {
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    const std::array<BookLevelUpdate, 3> updates{{{true, 200, 0.75}, {true, 199.9, 2}, {false, 200.1, 1.25}}};
    book.applyUpdates(updates, std::chrono::system_clock::now(), nullptr);
    DomModel model;
    model.clear("BTC-USD");
    constexpr qint64 now = 1'800'000'000'000;
    DomTradeWindow trades;
    const auto at = std::chrono::system_clock::time_point(std::chrono::milliseconds(now));
    trades.ingest({at, "BTC-USD", "", AggressorSide::Buy, 199.9, 1.5});
    trades.ingest({at, "BTC-USD", "", AggressorSide::Sell, 199.9, 0.25});
    trades.ingest({at, "BTC-USD", "", AggressorSide::Unknown, 199.9, 7});
    model.publish(book, trades, true, now);
    int row = -1;
    for (int r = 0; r < model.rowCount(); ++r)
        if (model.index(r, 0).data(DomModel::BucketRole).toLongLong() == 1999) row = r;
    ASSERT_GE(row, 0);
    const QString summary = "Traded volume in BTC · last 60 s by trade time · 3 trades · unknown side: 1 (excluded from buy/sell/Δ)";
    const QString volumes = "Bought 1.5 · Sold 0.25 · Δ 1.25 BTC";
    const QStringList longForms{
        "2 BTC resting bid\n2.75 BTC cumulative from the touch\n" + summary,
        " · aggregated price bucket, not an exchange spread\n" + volumes + "\n" + summary,
        "0 BTC resting ask\n0 BTC cumulative from the touch\n" + summary,
        volumes + "\n" + summary, volumes + "\n" + summary, volumes + "\n" + summary};
    const QStringList shortForms{"2 BTC · Σ 2.75", volumes, "0 BTC · Σ 0", volumes, volumes, volumes};
    for (int col = 0; col < DomModel::Columns; ++col) {
        const auto tip = model.index(row, col).data(Qt::ToolTipRole).toString();
        EXPECT_EQ(tip, shortForms[col]);
        EXPECT_LE(tip.count('\n'), 1);
        EXPECT_LE(tip.size(), 80);
    }
    auto& help = TooltipHelp::instance();
    help.setExplanatory(true);
    for (int col = 0; col < DomModel::Columns; ++col)
        EXPECT_EQ(model.index(row, col).data(Qt::ToolTipRole).toString().toUtf8(), longForms[col].toUtf8());
    help.setExplanatory(false);
    EXPECT_EQ(model.index(row, DomModel::Bid).data(Qt::ToolTipRole).toString(), shortForms[0]);
}

TEST_F(TooltipHelpTest, ExistingDockControlsAndModelRefreshWithoutMarketData) {
    OrderBookDock dock;
    dock.resize(650, 600);
    dock.show(); // DomTests uses Qt's offscreen platform, never a native window.
    QCoreApplication::processEvents();
    StatusBar status;
    auto* table = dock.findChild<QTableView*>("domLadder");
    ASSERT_NE(table, nullptr);
    auto* model = static_cast<DomModel*>(table->model());
    LiveOrderBook book;
    book.initialize(0, 400, 0.1);
    const std::array<BookLevelUpdate, 2> updates{{{true, 200, 1}, {false, 200.1, 2}}};
    book.applyUpdates(updates, std::chrono::system_clock::now(), nullptr);
    model->clear("BTC-USD");
    model->publish(book, {}, true);
    QSignalSpy rows(model, &QAbstractItemModel::dataChanged);
    QSignalSpy headers(model, &QAbstractItemModel::headerDataChanged);
    const QList<QPair<QString, QString>> controls{{"domPulse", ""}, {"domLean", ""},
        {"domCumulative", "Cumulative size"}, {"domRecenter", ""}, {"domGrouping", "Row grouping"},
        {"domLadder", ""}, {"domLeanStrip", ""}};
    for (const auto& [name, shortForm] : controls) {
        auto* widget = dock.findChild<QWidget*>(name);
        ASSERT_NE(widget, nullptr);
        EXPECT_EQ(widget->toolTip(), shortForm);
    }
    auto* age = status.findChild<QLabel*>("marketBookAge");
    ASSERT_NE(age, nullptr);
    EXPECT_TRUE(age->toolTip().isEmpty());
    QMenuBar bar;
    auto* action = toggle(bar);
    ASSERT_NE(action, nullptr);
    action->trigger();
    EXPECT_EQ(rows.count(), 1);
    EXPECT_EQ(headers.count(), 1);
    EXPECT_EQ(rows.first().at(2).value<QList<int>>(), QList<int>{Qt::ToolTipRole});
    for (const auto& [name, shortForm] : controls)
        EXPECT_GT(dock.findChild<QWidget*>(name)->toolTip().size(), shortForm.size());
    EXPECT_EQ(age->toolTip(), "Time since the last accepted book message. A quiet market is not necessarily stale.");
    EXPECT_EQ(model->headerData(DomModel::Bid, Qt::Horizontal, Qt::ToolTipRole).toString(), "Resting bid size in BTC");
    action->trigger();
    EXPECT_EQ(rows.count(), 2);
    EXPECT_EQ(headers.count(), 2);
    for (const auto& [name, shortForm] : controls)
        EXPECT_EQ(dock.findChild<QWidget*>(name)->toolTip(), shortForm);
    EXPECT_TRUE(age->toolTip().isEmpty());
    for (int column = 0; column < DomModel::Columns; ++column)
        EXPECT_TRUE(model->headerData(column, Qt::Horizontal, Qt::ToolTipRole).toString().isEmpty());
}

TEST_F(TooltipHelpTest, LeanKeepsHiddenWidthStateButSuppressesItsVisibleName) {
    OrderBookDock dock;
    dock.resize(650, 600);
    dock.show();
    QCoreApplication::processEvents();
    auto* lean = dock.findChild<QToolButton*>("domLean");
    ASSERT_NE(lean, nullptr);
    lean->setChecked(true);
    auto& help = TooltipHelp::instance();
    const auto check = [&](const QString& shortForm, const QString& longForm) {
        EXPECT_EQ(lean->toolTip(), shortForm);
        help.setExplanatory(true);
        EXPECT_EQ(lean->toolTip().toUtf8(), longForm.toUtf8());
        help.setExplanatory(false);
        EXPECT_EQ(lean->toolTip(), shortForm);
    };
    const QString normal = "Book imbalance, depth and two minute history; shows from 280 px dock width.";
    check({}, normal);
    dock.resize(260, 600);
    QCoreApplication::processEvents();
    check("Lean · hidden below 280 px", "Lean Gauge: on, hidden at this width; it shows from 280 px dock width.");
    lean->setChecked(false);
    check({}, normal);
    lean->setChecked(true);
    check("Lean · hidden below 280 px", "Lean Gauge: on, hidden at this width; it shows from 280 px dock width.");
    dock.resize(650, 600);
    QCoreApplication::processEvents();
    check({}, normal);
}

TEST_F(TooltipHelpTest, WatchlistStatesRefreshAndActiveRowStaysSilent) {
    WatchlistDock dock;
    auto* tree = dock.findChild<QTreeView*>();
    ASSERT_NE(tree, nullptr);
    const auto index = tree->model()->index(0, 0);
    ASSERT_TRUE(index.isValid());
    const QString symbol = index.data(WatchlistDock::TickerRole).toString();
    auto& help = TooltipHelp::instance();
    const auto check = [&](const QString& shortForm, const QString& longForm) {
        EXPECT_EQ(index.data(Qt::ToolTipRole).toString(), shortForm);
        help.setExplanatory(true);
        EXPECT_EQ(index.data(Qt::ToolTipRole).toString(), longForm);
        help.setExplanatory(false);
        EXPECT_EQ(index.data(Qt::ToolTipRole).toString(), shortForm);
    };
    check({}, "Availability unverified until selected");
    dock.setChartSwitchState({}, symbol);
    check({}, "Waiting for chart confirmation");
    const QString reason = "The server refused this symbol because it is absent from the catalog.";
    dock.setChartSwitchState({}, {}, symbol, reason);
    check(reason, reason);
    dock.setChartSwitchState({}, {}, symbol, {});
    check({}, {}); // A marker without a reason has nothing to add.
    dock.setChartSwitchState({}, {});
    dock.setCryptoAvailability({}, true);
    check({}, "Not in the current market catalog");
    dock.setCryptoAvailability({symbol}, true);
    dock.setChartSwitchState(symbol, {});
    check({}, {});
}
} // namespace
