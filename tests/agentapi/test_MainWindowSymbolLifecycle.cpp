#include "MainWindowGpu.h"
#include "config/GuiConfigStore.hpp"
#include "datasources/IGridDataSource.hpp"
#include "widgets/ChartDock.hpp"
#include "widgets/ServiceLocator.hpp"
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"
#include "render/LabTextItem.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include "render/AlgoOverlayRenderer.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "render/PaperTradeOverlayRenderer.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

struct MainWindowSymbolLifecyclePeer {
    static bool subscriptionReady(const MainWindowGPU& window) { return window.m_userSubscribed; }
    static int retryInterval(const MainWindowGPU& window) { return window.m_heldRetryTimer->interval(); }
    static bool retryActive(const MainWindowGPU& window) { return window.m_heldRetryTimer->isActive(); }
    static void fireRetry(MainWindowGPU& window) {
        window.m_heldRetryTimer->stop();
        QMetaObject::invokeMethod(window.m_heldRetryTimer, "timeout", Qt::DirectConnection);
    }
};

namespace {
void connected(MainWindowGPU& window) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, true)));
}

void select(MainWindowGPU& window, QLineEdit* input, const QString& symbol) {
    input->setText(symbol);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onSubscribe", Qt::DirectConnection));
}

void waitForInput(QLineEdit* input, const QString& expected) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (input->text() != expected && elapsed.elapsed() < 7000)
        QTest::qWait(20);
    EXPECT_EQ(input->text(), expected);
}

TEST(MainWindowSymbolLifecycle, AcknowledgementRefusalTimeoutAndReconnect) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    ASSERT_NE(input, nullptr);
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_EQ(changes.size(), 1);

    select(window, input, "ETH-USD");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to ETH-USD"));
    emit source->subscriptionRefused("", 8, "Connection cap reached");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_EQ(window.statusBar()->currentMessage(), "Connection cap reached");

    select(window, input, "SOL-USD");
    emit source->subscriptionAcknowledged("sol-usd");
    ASSERT_EQ(changes.size(), 2);
    EXPECT_EQ(input->text(), "SOL-USD");

    select(window, input, "ADA-USD");
    waitForInput(input, "SOL-USD");
    EXPECT_EQ(changes.size(), 2);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ADA-USD timed out");

    select(window, input, "XRP-USD");
    select(window, input, "DOGE-USD");
    emit source->subscriptionRefused("XRP-USD", 8, "stale refusal");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to DOGE-USD"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_EQ(input->text(), "SOL-USD");
    connected(window);
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying SOL-USD in 5 s"));
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    select(window, input, "SOL-USD"); // manual retry while the backoff is armed
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionAcknowledged("SOL-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_EQ(changes.size(), 2);
    EXPECT_TRUE(window.statusBar()->currentMessage().isEmpty());

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 15000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 60000);
}

TEST(MainWindowSymbolLifecycle, ServerDefaultWinsAfterStartupAckUntilUserSelection) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_EQ(changes.size(), 1);
    ServerConfig config;
    config.defaultSymbols = {"ETH-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_EQ(changes.size(), 1);
    emit source->subscriptionAcknowledged("ETH-USD");
    ASSERT_EQ(changes.size(), 2);
    select(window, input, "SOL-USD");
    config.defaultSymbols = {"AVAX-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "SOL-USD");
    emit source->subscriptionAcknowledged("SOL-USD");
    EXPECT_EQ(changes.size(), 3);
}

TEST(MainWindowSymbolLifecycle, UserSelectionBeforeDefaultAckWinsStartupRace) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window); // BTC is requested but not yet acknowledged
    select(window, input, "SOL-USD");
    ServerConfig config;
    config.defaultSymbols = {"ETH-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "SOL-USD");
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(changes.size(), 0); // superseded startup ack must not activate
    emit source->subscriptionAcknowledged("SOL-USD");
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "SOL-USD");
}

TEST(MainWindowSymbolLifecycle, SupersededRefusalLeavesCurrentSwitchPending) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    select(window, input, "SOL-USD");
    emit source->subscriptionRefused("ETH-USD", 8, "stale refusal");
    EXPECT_EQ(input->text(), "SOL-USD");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to SOL-USD"));
}

TEST(MainWindowSymbolLifecycle, ReconnectReadinessAndRefusalBackoff) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    connected(window);
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 15000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 60000);
    select(window, input, "BTC-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
}

TEST(MainWindowSymbolLifecycle, MissingAckTimesOutAndRestoresInput) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    waitForInput(input, "BTC-USD");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ETH-USD timed out");
}

TEST(MainWindowSymbolLifecycle, DisconnectAbandonsPendingSwitch) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_EQ(input->text(), "BTC-USD");
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_EQ(changes.size(), 1);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    qmlRegisterModule("Sentinel", 1, 0);
    qmlRegisterType<UnifiedGridRenderer>("Sentinel", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel", 1, 0, "CoordinateSystem");
    qmlRegisterType<TimeAxisModel>("Sentinel", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel", 1, 0, "PriceAxisModel");
    qmlRegisterType<AlgoOverlayRenderer>("Sentinel", 1, 0, "AlgoOverlayRenderer");
    qmlRegisterType<PaperTradeOverlayModel>("Sentinel", 1, 0, "PaperTradeOverlayModel");
    qmlRegisterType<PaperTradeOverlayRenderer>("Sentinel", 1, 0, "PaperTradeOverlayRenderer");
    qmlRegisterModule("Sentinel.Charts", 1, 0);
    qmlRegisterType<LabTextItem>("Sentinel.Charts", 1, 0, "LabTextItem");
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    qmlRegisterType<CandlestickOverlayItem>("Sentinel.Charts", 1, 0, "CandlestickOverlayItem");
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    ClientConfig client;
    client.server.host = "127.0.0.1";
    client.server.port = "1"; // no owner recorder; signals below are deterministic
    client.gui.apiPort = 0;
    client.gui.startScreener = false;
    GuiConfigStore::instance().setClientConfig(client);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
