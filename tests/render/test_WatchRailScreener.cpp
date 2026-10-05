#include "widgets/ScreenerDock.hpp"
#include "widgets/WatchlistDock.hpp"
#include "protocol/SentinelStreamClient.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QSettings>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <gtest/gtest.h>

struct ScreenerErrorDispatchTest {
    static void deliver(SentinelStreamClient& client, const std::string& frame) {
        client.handleMessage(frame);
    }
};

namespace {
int rowFor(QStandardItemModel* model, const QString& symbol) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->item(row, 0)->data(Qt::UserRole + 1).toString() == symbol ||
            model->item(row, 0)->text() == symbol) return row;
    return -1;
}

void updateScreener(ScreenerDock& dock, const QString& asset, const QJsonArray& rows) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onScreenerUpdate", Qt::DirectConnection,
        Q_ARG(QString, asset), Q_ARG(int, rows.size()),
        Q_ARG(QByteArray, QJsonDocument(rows).toJson(QJsonDocument::Compact))));
}

TEST(WatchRail, PendingAckRefusalKeyboardAndCatalogTruth) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    WatchlistDock dock;
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    auto* status = dock.findChild<QLabel*>("watchStatus");
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(status, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(tree->model());
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->rowCount(), 7);
    EXPECT_TRUE(status->text().contains("Availability unknown"));
    QSignalSpy selected(&dock, &WatchlistDock::symbolSelected);
    const int eth = rowFor(model, "ETH-USD");
    ASSERT_GE(eth, 0);
    tree->setCurrentIndex(model->index(eth, 0));
    QTest::keyClick(tree, Qt::Key_Return);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).toString(), "ETH-USD");
    EXPECT_TRUE(status->text().contains("Switching to ETH-USD"));
    EXPECT_FALSE(model->item(eth, 0)->text().startsWith("●"));

    dock.setChartSwitchState("ETH-USD", "");
    EXPECT_TRUE(model->item(eth, 0)->text().startsWith("●"));
    dock.setChartSwitchState("BTC-USD", "", "ETH-USD", "Connection cap reached");
    EXPECT_TRUE(model->item(eth, 0)->text().startsWith("!"));
    EXPECT_TRUE(status->text().contains("Connection cap"));
    dock.setCryptoAvailability({"BTC-USD"}, true, "server catalog");
    QTest::keyClick(tree, Qt::Key_Return);
    EXPECT_EQ(selected.count(), 1);
    EXPECT_TRUE(status->text().contains("unavailable"));
}

TEST(WatchRail, PinsPersistWithDefaultFormatSettings) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    {
        WatchlistDock dock;
        auto* edit = dock.findChild<QLineEdit*>("watchSymbolEntry");
        ASSERT_NE(edit, nullptr);
        edit->setText("XRP-USD");
        dock.findChild<QToolButton*>("watchAdd")->click();
        EXPECT_EQ(dock.findChild<QComboBox*>("watchPreset")->currentText(), "Pinned");
        auto* tree = dock.findChild<QTreeView*>("watchRows");
        EXPECT_EQ(rowFor(qobject_cast<QStandardItemModel*>(tree->model()), "XRP-USD"), 0);
    }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.sync();
    EXPECT_TRUE(settings.value("watchRail/pinnedSymbols").toStringList().contains("XRP-USD"));
    WatchlistDock restored;
    auto* tree = restored.findChild<QTreeView*>("watchRows");
    EXPECT_EQ(restored.findChild<QComboBox*>("watchPreset")->currentText(), "Pinned");
    EXPECT_EQ(rowFor(qobject_cast<QStandardItemModel*>(tree->model()), "XRP-USD"), 0);
}

TEST(WatchRail, MouseAndKeyboardDispatchOnce) {
    WatchlistDock dock;
    dock.show();
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(2); // Stock rows have no pending deduplication.
    auto* model = qobject_cast<QStandardItemModel*>(tree->model());
    const QModelIndex index = model->index(0, 0);
    QSignalSpy selected(&dock, &WatchlistDock::symbolSelected);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    tree->setCurrentIndex(index);
    QTest::keyClick(tree, Qt::Key_Return);
    QTest::keyClick(tree, Qt::Key_Space);
    EXPECT_EQ(selected.count(), 3);
}

TEST(Screener, StableRowsMissingValuesAssetSwitchAndSource) {
    ScreenerDock dock;
    auto* table = dock.findChild<QTableView*>("screenerRows");
    auto* status = dock.findChild<QLabel*>("screenerStatus");
    ASSERT_NE(table, nullptr);
    ASSERT_NE(status, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(table->model());
    ASSERT_NE(model, nullptr);
    updateScreener(dock, "crypto", QJsonArray{
        QJsonObject{{"symbol", "BTC-USD"}, {"Name", "Bitcoin"}, {"Price", QJsonValue::Null},
                    {"Change %", QJsonValue::Null}, {"Volume", 0}},
        QJsonObject{{"symbol", "ETH-USD"}, {"Name", "Ethereum"}, {"Price", 100.0},
                    {"Change %", 2.0}, {"Volume", 2000.0}},
    });
    ASSERT_EQ(model->rowCount(), 2);
    const int btc = rowFor(model, "BTC-USD");
    const int eth = rowFor(model, "ETH-USD");
    ASSERT_GE(btc, 0);
    ASSERT_GE(eth, 0);
    EXPECT_EQ(model->item(btc, 2)->text(), "—");
    EXPECT_EQ(model->item(btc, 3)->text(), "—");
    EXPECT_EQ(model->item(btc, 4)->text(), "0");
    EXPECT_TRUE(status->text().contains("TradingView via Sentinel"));
    EXPECT_TRUE(status->text().contains("vendor as-of unavailable"));
    QSignalSpy selected(&dock, &ScreenerDock::rowSelected);
    table->setCurrentIndex(model->index(eth, 0));
    QTest::keyClick(table, Qt::Key_Return);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).toString(), "ETH-USD");
    table->selectRow(eth);
    updateScreener(dock, "crypto", QJsonArray{
        QJsonObject{{"symbol", "ETH-USD"}, {"Price", 101.0}},
        QJsonObject{{"symbol", "BTC-USD"}, {"Price", 99.0}},
        QJsonObject{{"symbol", "SOL-USD"}, {"Price", 20.0}},
    });
    EXPECT_EQ(model->rowCount(), 3);
    EXPECT_EQ(table->currentIndex().siblingAtColumn(0).data().toString(), "ETH-USD");
    EXPECT_EQ(model->item(rowFor(model, "ETH-USD"), 2)->text(), "101.0000");

    dock.findChild<QComboBox*>("screenerAsset")->setCurrentIndex(1);
    EXPECT_EQ(model->rowCount(), 0);
    updateScreener(dock, "crypto", QJsonArray{QJsonObject{{"symbol", "BTC-USD"}}});
    EXPECT_EQ(model->rowCount(), 0);
    updateScreener(dock, "stock", QJsonArray{QJsonObject{{"symbol", "AAPL"}, {"Price", QJsonValue::Null}}});
    EXPECT_EQ(model->item(0, 2)->text(), "—");
    EXPECT_FALSE(table->isColumnHidden(7));
    dock.showServiceError("upstream failed");
    EXPECT_TRUE(status->text().contains("upstream failed"));
}

TEST(Screener, TimeoutKeepsExistingRows) {
    ScreenerDock dock;
    SentinelStreamClient client("127.0.0.1", "1"); // Never connect; requests stay local to the client.
    dock.setStreamClient(&client);
    updateScreener(dock, "crypto", QJsonArray{QJsonObject{{"symbol", "BTC-USD"}}});
    dock.findChild<QToolButton*>("screenerRefresh")->click();
    auto* timer = dock.findChild<QTimer*>("screenerFetchTimeout");
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(timer->isActive());
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onFetchTimeout", Qt::DirectConnection));
    EXPECT_EQ(qobject_cast<QStandardItemModel*>(dock.findChild<QTableView*>("screenerRows")->model())->rowCount(), 1);
    EXPECT_TRUE(dock.findChild<QLabel*>("screenerStatus")->text().contains("timed out"));
}

TEST(Screener, BatchedSortRetainsItemsAndMissingRoles) {
    ScreenerDock dock;
    auto* table = dock.findChild<QTableView*>("screenerRows");
    auto* model = qobject_cast<QStandardItemModel*>(table->model());
    const QJsonArray first{
        QJsonObject{{"symbol", "ETH-USD"}, {"Price", 2.0}, {"Change %", 1.0}},
        QJsonObject{{"symbol", "BTC-USD"}, {"Price", 1.0}, {"Change %", -1.0}}};
    updateScreener(dock, "crypto", first);
    EXPECT_EQ(model->item(0, 0)->text(), "BTC-USD"); // Header says Symbol ascending.
    auto* btcSymbol = model->item(rowFor(model, "BTC-USD"), 0);
    auto* btcPrice = model->item(rowFor(model, "BTC-USD"), 2);
    auto* btcChange = model->item(rowFor(model, "BTC-USD"), 3);
    QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy layout(model, &QAbstractItemModel::layoutChanged);
    QSignalSpy data(model, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    updateScreener(dock, "crypto", first);
    EXPECT_EQ(inserted.count(), 0);
    EXPECT_EQ(removed.count(), 0);
    EXPECT_EQ(layout.count(), 0);
    EXPECT_EQ(data.count(), 0);
    EXPECT_EQ(reset.count(), 0);
    EXPECT_EQ(model->item(rowFor(model, "BTC-USD"), 0), btcSymbol);
    EXPECT_EQ(model->item(rowFor(model, "BTC-USD"), 2), btcPrice);

    table->sortByColumn(2, Qt::AscendingOrder);
    table->selectRow(rowFor(model, "BTC-USD"));
    layout.clear();
    updateScreener(dock, "crypto", QJsonArray{
        QJsonObject{{"symbol", "ETH-USD"}, {"Price", 0.00000012}, {"Change %", QJsonValue::Null}},
        QJsonObject{{"symbol", "BTC-USD"}, {"Price", 4.0}, {"Change %", QJsonValue::Null}}});
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(model->item(0, 0)->text(), "ETH-USD");
    EXPECT_EQ(model->item(0, 2)->text(), "1.2e-07");
    EXPECT_EQ(model->item(1, 0)->text(), "BTC-USD");
    EXPECT_EQ(table->currentIndex().siblingAtColumn(0).data().toString(), "BTC-USD");
    EXPECT_EQ(model->item(rowFor(model, "BTC-USD"), 0), btcSymbol);
    EXPECT_EQ(model->item(rowFor(model, "BTC-USD"), 2), btcPrice);
    EXPECT_EQ(model->item(rowFor(model, "BTC-USD"), 3), btcChange);
    EXPECT_FALSE(btcChange->data(Qt::UserRole + 1).isValid());
    EXPECT_FALSE(btcChange->data(Qt::ForegroundRole).isValid());
    EXPECT_EQ(layout.count(), 1); // Only the completed refresh batch sorts the model.

    table->sortByColumn(2, Qt::DescendingOrder);
    updateScreener(dock, "crypto", QJsonArray{
        QJsonObject{{"symbol", "BTC-USD"}, {"Price", 0.0}},
        QJsonObject{{"symbol", "ETH-USD"}, {"Price", 0.00000012}}});
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(model->item(0, 0)->text(), "ETH-USD");
    EXPECT_EQ(model->item(0, 2)->text(), "1.2e-07");
    EXPECT_EQ(btcPrice->text(), "0.000000");
    updateScreener(dock, "crypto", QJsonArray{
        QJsonObject{{"symbol", "BTC-USD"}, {"Price", QJsonValue::Null}},
        QJsonObject{{"symbol", "ETH-USD"}, {"Price", 0.00000012}}});
    EXPECT_EQ(btcPrice->text(), "—");
    EXPECT_FALSE(btcPrice->data(Qt::UserRole + 1).isValid());
}

TEST(Screener, RefreshKeepsScrolledSymbolAndSelectionAcrossNumericReorder) {
    ScreenerDock dock;
    dock.resize(640, 360);
    dock.show();
    auto* table = dock.findChild<QTableView*>("screenerRows");
    auto* model = qobject_cast<QStandardItemModel*>(table->model());
    QJsonArray first, reversed;
    for (int i = 0; i < 80; ++i) {
        const QString symbol = QStringLiteral("PAIR%1-USD").arg(i, 2, 10, QChar('0'));
        first.append(QJsonObject{{"symbol", symbol}, {"Price", double(i + 1)}});
        reversed.append(QJsonObject{{"symbol", symbol}, {"Price", double(80 - i)}});
    }
    updateScreener(dock, "crypto", first);
    table->sortByColumn(2, Qt::AscendingOrder);
    table->selectRow(rowFor(model, "PAIR40-USD"));
    table->scrollTo(model->index(rowFor(model, "PAIR30-USD"), 0), QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    ASSERT_GT(table->verticalScrollBar()->value(), 0);
    const QString topSymbol = table->indexAt(QPoint(1, 1)).siblingAtColumn(0).data().toString();
    ASSERT_EQ(topSymbol, "PAIR30-USD");
    updateScreener(dock, "crypto", reversed);
    EXPECT_EQ(model->item(0, 0)->text(), "PAIR79-USD");
    EXPECT_EQ(table->currentIndex().siblingAtColumn(0).data().toString(), "PAIR40-USD");
    EXPECT_EQ(table->indexAt(QPoint(1, 1)).siblingAtColumn(0).data().toString(), topSymbol);
}

TEST(Screener, HiddenAutoRefreshSuspendsAndResumes) {
    ScreenerDock dock;
    SentinelStreamClient client("127.0.0.1", "1");
    dock.setStreamClient(&client);
    auto* fetchTimer = dock.findChild<QTimer*>("screenerFetchTimeout");
    auto* autoTimer = dock.findChild<QTimer*>("screenerAutoTimer");
    ASSERT_NE(autoTimer, nullptr);
    dock.findChild<QCheckBox*>()->setChecked(true);
    EXPECT_FALSE(fetchTimer->isActive());
    EXPECT_FALSE(autoTimer->isActive());
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onAutoTimer", Qt::DirectConnection));
    EXPECT_FALSE(fetchTimer->isActive());
    dock.show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(fetchTimer->isActive());
    EXPECT_TRUE(autoTimer->isActive());
    dock.hide();
    EXPECT_FALSE(autoTimer->isActive());
    updateScreener(dock, "crypto", QJsonArray{}); // The in-flight response can complete while hidden.
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onAutoTimer", Qt::DirectConnection));
    EXPECT_FALSE(fetchTimer->isActive());
    dock.show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(fetchTimer->isActive());
    EXPECT_TRUE(autoTimer->isActive());
}

TEST(Screener, MouseAndKeyboardDispatchOnce) {
    ScreenerDock dock;
    dock.show();
    auto* table = dock.findChild<QTableView*>("screenerRows");
    updateScreener(dock, "crypto", QJsonArray{QJsonObject{{"symbol", "BTC-USD"}}});
    auto* model = qobject_cast<QStandardItemModel*>(table->model());
    const QModelIndex index = model->index(0, 0);
    QSignalSpy selected(&dock, &ScreenerDock::rowSelected);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(index).center());
    QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    table->setCurrentIndex(index);
    QTest::keyClick(table, Qt::Key_Return);
    QTest::keyClick(table, Qt::Key_Space);
    EXPECT_EQ(selected.count(), 3);
}

TEST(Screener, RequestErrorDispatchAndUncorrelatedRecovery) {
    ScreenerDock dock;
    SentinelStreamClient client("127.0.0.1", "1");
    dock.setStreamClient(&client);
    QSignalSpy errors(&client, &SentinelStreamClient::screenerRequestError);
    auto* status = dock.findChild<QLabel*>("screenerStatus");
    auto* timer = dock.findChild<QTimer*>("screenerFetchTimeout");
    auto* table = dock.findChild<QTableView*>("screenerRows");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(timer, nullptr);
    ASSERT_NE(table, nullptr);
    updateScreener(dock, "crypto", QJsonArray{QJsonObject{{"symbol", "BTC-USD"}}});
    const QString received = status->text();
    dock.showServiceError("unidentified earlier failure");
    EXPECT_TRUE(status->text().contains("TradingView via Sentinel"));
    EXPECT_TRUE(status->text().contains("vendor as-of unavailable"));
    EXPECT_EQ(qobject_cast<QStandardItemModel*>(table->model())->rowCount(), 1);

    ScreenerErrorDispatchTest::deliver(client, R"({"type":"error","context":"subscribe","message":"other"})");
    QCoreApplication::processEvents();
    EXPECT_EQ(errors.count(), 0);

    dock.findChild<QComboBox*>("screenerAsset")->setCurrentIndex(1);
    dock.findChild<QToolButton*>("screenerRefresh")->click();
    ASSERT_TRUE(timer->isActive());
    ScreenerErrorDispatchTest::deliver(client, R"({"type":"error","context":"screener_request","message":"old request failed"})");
    QCoreApplication::processEvents();
    ASSERT_EQ(errors.count(), 1);
    EXPECT_EQ(errors.at(0).at(0).toString(), "old request failed");
    EXPECT_TRUE(timer->isActive());
    EXPECT_TRUE(status->text().contains("request not identified"));
    EXPECT_TRUE(status->text().contains("old request failed"));
    EXPECT_EQ(qobject_cast<QStandardItemModel*>(table->model())->rowCount(), 0);

    updateScreener(dock, "stock", QJsonArray{QJsonObject{{"symbol", "AAPL"}}});
    EXPECT_FALSE(timer->isActive());
    EXPECT_TRUE(status->text().contains("TradingView via Sentinel"));
    EXPECT_FALSE(status->text().contains("old request failed"));
    EXPECT_TRUE(received.contains("vendor as-of unavailable"));
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
