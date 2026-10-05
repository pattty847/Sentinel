#include "widgets/ScreenerDock.hpp"
#include "widgets/WatchlistDock.hpp"
#include "protocol/SentinelStreamClient.hpp"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
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
