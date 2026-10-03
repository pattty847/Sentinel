#include "MainWindowGpu.h"
#include "config/GuiConfigStore.hpp"
#include "datasources/IGridDataSource.hpp"
#include "widgets/ChartDock.hpp"
#include "widgets/ServiceLocator.hpp"
#include "mainwindow/GuiApiServer.h"
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
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

struct MainWindowSymbolLifecyclePeer {
    static bool subscriptionReady(const MainWindowGPU& window) { return window.m_userSubscribed; }
    static int retryInterval(const MainWindowGPU& window) { return window.m_heldRetryTimer->interval(); }
    static bool retryActive(const MainWindowGPU& window) { return window.m_heldRetryTimer->isActive(); }
    static bool switchTimeoutActive(const MainWindowGPU& window) { return window.m_symbolSwitchTimer->isActive(); }
    static int switchTimeoutInterval(const MainWindowGPU& window) { return window.m_symbolSwitchTimer->interval(); }
    static void fireSwitchTimeout(MainWindowGPU& window) {
        window.m_symbolSwitchTimer->stop();
        QMetaObject::invokeMethod(window.m_symbolSwitchTimer, "timeout", Qt::DirectConnection);
    }
    static quint16 apiPort(const MainWindowGPU& window) { return window.m_guiApiServer->port(); }
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

QByteArray apiRequest(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = {}) {
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        QByteArray head = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
        if (method == "POST")
            head += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        socket.write(head + "\r\n" + body);
    });
    socket.connectToHost("127.0.0.1", port);
    timer.start(3000);
    loop.exec();
    return socket.readAll();
}

QJsonObject responseField(const QByteArray& response, const char* field) {
    return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object()
        .value(QLatin1String(field)).toObject();
}

struct LocalApiPort {
    ClientConfig saved = GuiConfigStore::instance().clientConfig();
    QTemporaryDir screenshots;
    quint16 port = 0;
    LocalApiPort() {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0)) return;
        port = reservation.serverPort();
        reservation.close();
        auto config = saved;
        config.gui.apiPort = port;
        config.gui.screenshotDir = screenshots.path().toStdString();
        GuiConfigStore::instance().setClientConfig(config);
    }
    ~LocalApiPort() { GuiConfigStore::instance().setClientConfig(saved); }
};

TEST(MainWindowSymbolLifecycle, AcknowledgementRefusalTimeoutAndReconnect) {
    MainWindowGPU window(nullptr, 40);
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
    MainWindowGPU window(nullptr, 40);
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

TEST(MainWindowSymbolLifecycle, OfflineSelectionStartsDeadlineOnlyAfterConnect) {
    MainWindowGPU window(nullptr, 40);
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    select(window, input, "ETH-USD");
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::switchTimeoutActive(window));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Waiting to connect"));
    QTest::qWait(100);
    EXPECT_EQ(input->text(), "ETH-USD");
    connected(window);
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::switchTimeoutActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::switchTimeoutInterval(window), 40);
    waitForInput(input, "BTC-USD");
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ETH-USD timed out");
}

TEST(MainWindowSymbolLifecycle, AgentOperationSurvivesSameSymbolReentryAndReportsEachFailureCode) {
    LocalApiPort api;
    ASSERT_NE(api.port, 0);
    MainWindowGPU window(nullptr, 500);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    const quint16 port = MainWindowSymbolLifecyclePeer::apiPort(window);
    ASSERT_EQ(port, api.port);
    const auto post = [port](const char* symbol) {
        return apiRequest(port, "POST", "/api/v1/symbol", QByteArray("{\"symbol\":\"") + symbol + "\"}");
    };
    const auto get = [port](const QString& id) {
        return apiRequest(port, "GET", "/api/v1/operations/" + id.toUtf8());
    };

    const auto first = post("ETH-USD");
    ASSERT_TRUE(first.startsWith("HTTP/1.1 200")) << first.toStdString();
    const QString firstId = responseField(first, "data").value("operationId").toString();
    ASSERT_EQ(responseField(first, "data").value("status"), "pending");
    select(window, input, "ETH-USD");
    const auto stillPending = get(firstId);
    EXPECT_TRUE(stillPending.startsWith("HTTP/1.1 200"));
    EXPECT_EQ(responseField(stillPending, "data").value("status"), "pending");
    emit source->subscriptionAcknowledged("ETH-USD");
    const auto activated = get(firstId);
    EXPECT_TRUE(activated.startsWith("HTTP/1.1 200"));
    EXPECT_NE(responseField(activated, "data").value("status"), "superseded");

    const auto refused = post("SOL-USD");
    const QString refusedId = responseField(refused, "data").value("operationId").toString();
    emit source->subscriptionRefused("SOL-USD", 8, "Cap reached");
    const auto refusedReply = get(refusedId);
    EXPECT_TRUE(refusedReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(refusedReply, "error").value("code"), "connection_cap");

    const auto timedOut = post("ADA-USD");
    const QString timeoutId = responseField(timedOut, "data").value("operationId").toString();
    MainWindowSymbolLifecyclePeer::fireSwitchTimeout(window);
    const auto timeoutReply = get(timeoutId);
    EXPECT_TRUE(timeoutReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(timeoutReply, "error").value("code"), "switch_timeout");

    const auto interrupted = post("DOT-USD");
    const QString interruptedId = responseField(interrupted, "data").value("operationId").toString();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    const auto disconnectedReply = get(interruptedId);
    EXPECT_TRUE(disconnectedReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(disconnectedReply, "error").value("code"), "disconnected");
}

TEST(MainWindowSymbolLifecycle, RefusedReplacementRearmsRetryForUnconfirmedHeldSymbol) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    ASSERT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    select(window, input, "ETH-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionRefused("ETH-USD", 8, "Cap reached for ETH");
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying BTC-USD in 5 s"));
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
}

TEST(MainWindowSymbolLifecycle, TimedOutReplacementRearmsRetryForUnconfirmedHeldSymbol) {
    MainWindowGPU window(nullptr, 40);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    select(window, input, "ETH-USD");
    waitForInput(input, "BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying BTC-USD in 5 s"));
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
