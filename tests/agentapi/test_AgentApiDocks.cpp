#include "mainwindow/DockVisibilityController.hpp"
#include "mainwindow/GuiApiServer.h"
#include "config/AgentHostMode.hpp"
#include "widgets/LayoutManager.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSettings>
#include <QTabBar>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

namespace {
QByteArray requestPath(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = {}) {
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

QByteArray request(quint16 port, const QByteArray& method, const QByteArray& body = {}) {
    return requestPath(port, method, "/api/v1/docks", body);
}

QJsonObject responseData(const QByteArray& reply) {
    return QJsonDocument::fromJson(reply.mid(reply.indexOf("\r\n\r\n") + 4)).object().value("data").toObject();
}

QJsonObject responseError(const QByteArray& reply) {
    return QJsonDocument::fromJson(reply.mid(reply.indexOf("\r\n\r\n") + 4)).object().value("error").toObject();
}

struct Window {
    QMainWindow main;
    QDockWidget heatmap{"Heatmap", &main};
    QDockWidget orderBook{"Order Book", &main};
    QDockWidget watchlist{"Watchlist", &main};
    DockVisibilityController docks{&main};
    Window() {
        main.resize(900, 600);
        for (auto* dock : {&heatmap, &orderBook, &watchlist}) {
            dock->setWidget(new QWidget(dock));
            main.addDockWidget(Qt::LeftDockWidgetArea, dock);
        }
        main.splitDockWidget(&heatmap, &orderBook, Qt::Horizontal);
        main.splitDockWidget(&orderBook, &watchlist, Qt::Horizontal);
        docks.add("heatmap", &heatmap);
        docks.add("orderBook", &orderBook);
        docks.add("watchlist", &watchlist);
        main.show();
        QApplication::processEvents();
    }
};

TEST(AgentApiDocks, RouteListsAndFocusesARealDockWindow) {
    QTemporaryDir settingsDir;
    const auto previousFormat = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    Window w;
    AgentApi::StateSnapshot state;
    state.meta.symbol = "BTC-USD";
    GuiApiServer server(&w.main, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto&) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [](const auto&, auto complete) { complete(heatmap::WallsSnapshot{}); },
        [&](const QString& kind, const AgentApi::ControlBody& body) {
            EXPECT_EQ(kind, "docks");
            AgentApi::ControlApply result;
            result.data["visible"] = w.docks.apply(body.dockVisible, body.dockFocus, body.persistDocks);
            return result;
        }, [] { return std::pair<quint64, quint64>{0, 0}; }, [](quint64) {});
    server.setDocksSnapshot([&] { return w.docks.snapshot(); });
    ASSERT_TRUE(server.start(0, "."));
    const auto before = request(server.port(), "GET");
    ASSERT_TRUE(before.startsWith("HTTP/1.1 200")) << before.toStdString();
    EXPECT_EQ(responseData(before).value("visible").toObject().size(), 3);
    const auto focused = request(server.port(), "POST", R"({"focus":"heatmap"})");
    ASSERT_TRUE(focused.startsWith("HTTP/1.1 200")) << focused.toStdString();
    EXPECT_TRUE(responseData(focused).contains("operationId"));
    QApplication::processEvents();
    const auto visible = responseData(request(server.port(), "GET")).value("visible").toObject();
    EXPECT_TRUE(visible.value("heatmap").toBool());
    EXPECT_FALSE(visible.value("orderBook").toBool());
    EXPECT_FALSE(visible.value("watchlist").toBool());
    EXPECT_GE(w.heatmap.width(), 850); // freed dock areas give the heatmap the window width
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    EXPECT_FALSE(settings.contains("agentApi/docks/visible")); // owner-mode route default is transient
    const auto unknown = request(server.port(), "POST", R"({"focus":"missing"})");
    EXPECT_TRUE(unknown.startsWith("HTTP/1.1 422"));
    EXPECT_TRUE(unknown.contains("orderBook"));
    QSettings::setDefaultFormat(previousFormat);
}

TEST(AgentApiDocks, HostedDockProfileRestoresWithoutSharingOtherSettings) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString profile = dir.path() + "/profile/docks.ini";
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/first", {}, &error, profile)) << qPrintable(error);
    {
        Window first;
        first.docks.apply({}, "heatmap", AgentHostMode::dockChangesPersist(false));
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.setValue("heatmap/changed", true);
        settings.sync();
        EXPECT_FALSE(settings.contains("agentApi/docks/visible"));
    }
    AgentHostMode::resetForTests();
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/second", {}, &error, profile)) << qPrintable(error);
    {
        Window next;
        next.docks.restore();
        const auto visible = next.docks.snapshot();
        EXPECT_TRUE(visible.value("heatmap").toBool());
        EXPECT_FALSE(visible.value("orderBook").toBool());
        EXPECT_FALSE(visible.value("watchlist").toBool());
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        EXPECT_FALSE(settings.contains("heatmap/changed"));
    }
    AgentHostMode::resetForTests();
}

TEST(AgentApiDocks, SavedLayoutWithRetiredDocksKeepsSurvivingDocks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto previousFormat = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
    QByteArray oldState;
    {
        QMainWindow old;
        QDockWidget heatmap("Heatmap", &old), watchlist("Watchlist", &old), lab("Lab", &old),
                    aiCommentary("AI Commentary", &old), screener("Screener", &old);
        heatmap.setObjectName("ChartDock");
        watchlist.setObjectName("WatchlistDock");
        lab.setObjectName("LabDock");
        aiCommentary.setObjectName("AICommentaryFeedDock");
        screener.setObjectName("ScreenerDock");
        old.addDockWidget(Qt::LeftDockWidgetArea, &heatmap);
        old.addDockWidget(Qt::RightDockWidgetArea, &watchlist);
        old.addDockWidget(Qt::RightDockWidgetArea, &lab);
        old.addDockWidget(Qt::RightDockWidgetArea, &aiCommentary);
        old.addDockWidget(Qt::RightDockWidgetArea, &screener);
        old.tabifyDockWidget(&watchlist, &lab);
        old.tabifyDockWidget(&watchlist, &aiCommentary);
        old.tabifyDockWidget(&watchlist, &screener);
        oldState = old.saveState();
    }
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.setValue("layouts/old/version", LayoutManager::APP_LAYOUT_VERSION);
        settings.setValue("layouts/old/state", oldState);
    }
    QMainWindow current;
    QDockWidget heatmap("Heatmap", &current), watchlist("Watchlist", &current), screener("Screener", &current);
    heatmap.setObjectName("ChartDock");
    watchlist.setObjectName("WatchlistDock");
    screener.setObjectName("ScreenerDock");
    current.addDockWidget(Qt::LeftDockWidgetArea, &heatmap);
    current.addDockWidget(Qt::RightDockWidgetArea, &watchlist);
    current.addDockWidget(Qt::RightDockWidgetArea, &screener);
    ASSERT_TRUE(LayoutManager::restoreLayout(&current, "old"));
    EXPECT_EQ(current.dockWidgetArea(&heatmap), Qt::LeftDockWidgetArea);
    EXPECT_EQ(current.dockWidgetArea(&watchlist), Qt::RightDockWidgetArea);
    EXPECT_EQ(current.dockWidgetArea(&screener), Qt::RightDockWidgetArea);
    EXPECT_TRUE(current.tabifiedDockWidgets(&watchlist).contains(&screener));
    EXPECT_TRUE(current.findChildren<QDockWidget*>("LabDock").isEmpty());
    EXPECT_TRUE(current.findChildren<QDockWidget*>("AICommentaryFeedDock").isEmpty());
    QByteArray migrated;
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        migrated = settings.value("layouts/old/state").toByteArray();
        EXPECT_NE(migrated, oldState);
    }
    QMainWindow nextSession;
    QDockWidget nextHeatmap("Heatmap", &nextSession), nextWatchlist("Watchlist", &nextSession),
                nextScreener("Screener", &nextSession), retiredProbe("Retired Lab", &nextSession),
                retiredAIProbe("Retired AI", &nextSession);
    nextHeatmap.setObjectName("ChartDock");
    nextWatchlist.setObjectName("WatchlistDock");
    nextScreener.setObjectName("ScreenerDock");
    retiredProbe.setObjectName("LabDock");
    retiredAIProbe.setObjectName("AICommentaryFeedDock");
    nextSession.addDockWidget(Qt::LeftDockWidgetArea, &nextHeatmap);
    nextSession.addDockWidget(Qt::RightDockWidgetArea, &nextWatchlist);
    nextSession.addDockWidget(Qt::RightDockWidgetArea, &nextScreener);
    ASSERT_TRUE(nextSession.restoreState(migrated));
    EXPECT_EQ(nextSession.dockWidgetArea(&nextWatchlist), Qt::RightDockWidgetArea);
    EXPECT_TRUE(nextSession.tabifiedDockWidgets(&nextWatchlist).contains(&nextScreener));
    EXPECT_FALSE(nextSession.restoreDockWidget(&retiredProbe)); // no saved Lab placeholder
    EXPECT_FALSE(nextSession.restoreDockWidget(&retiredAIProbe)); // no saved AI placeholder
    EXPECT_TRUE(LayoutManager::restoreLayout(&current, "old"));
    EXPECT_EQ(current.dockWidgetArea(&watchlist), Qt::RightDockWidgetArea);
    QSettings::setDefaultFormat(previousFormat);
}

TEST(AgentApiDocks, DockScreenshotGrabsWidgetAndReportsHiddenDock) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Window w;
    AgentApi::StateSnapshot state;
    GuiApiServer server(&w.main, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto&) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [](const auto&, auto complete) { complete(heatmap::WallsSnapshot{}); },
        [](const auto&, const auto&) { return AgentApi::ControlApply{}; },
        [] { return std::pair<quint64, quint64>{0, 0}; }, [](quint64) {});
    server.setWidgetGrab([&](const QString& target, QString* error) {
        if (target == "window") return w.main.grab().toImage();
        if (target != "orderBook" || !w.orderBook.isVisible()) {
            *error = "orderBook_not_visible";
            return QImage{};
        }
        return w.orderBook.grab().toImage();
    });
    ASSERT_TRUE(server.start(0, dir.path()));
    EXPECT_TRUE(requestPath(server.port(), "GET", "/screenshot?name=retired&target=lab").startsWith("HTTP/1.1 422"));
    EXPECT_FALSE(QFile::exists(dir.path() + "/retired.png"));
    const auto shot = requestPath(server.port(), "GET", "/api/v1/screenshot?name=dock&target=orderBook");
    EXPECT_TRUE(shot.startsWith("HTTP/1.1 200")) << shot.toStdString();
    QImage image(dir.path() + "/dock.png");
    EXPECT_FALSE(image.isNull());
    EXPECT_GE(image.width(), w.orderBook.width());
    EXPECT_GE(image.height(), w.orderBook.height());
    w.orderBook.hide();
    QEventLoop wait;
    QTimer::singleShot(1100, &wait, &QEventLoop::quit);
    wait.exec();
    const auto hidden = requestPath(server.port(), "GET", "/api/v1/screenshot?name=hidden&target=orderBook");
    EXPECT_TRUE(hidden.startsWith("HTTP/1.1 500")) << hidden.toStdString();
    EXPECT_EQ(responseError(hidden).value("message"), "orderBook_not_visible");
    EXPECT_FALSE(QFile::exists(dir.path() + "/hidden.png"));
    QString hostError;
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/host", {}, &hostError)) << qPrintable(hostError);
    const auto refusedMain = requestPath(server.port(), "GET", "/api/v1/screenshot?name=screen&target=main");
    EXPECT_TRUE(refusedMain.startsWith("HTTP/1.1 403")) << refusedMain.toStdString();
    QEventLoop nextShot;
    QTimer::singleShot(1100, &nextShot, &QEventLoop::quit);
    nextShot.exec();
    const auto wholeWindow = requestPath(server.port(), "GET", "/api/v1/screenshot?name=window&target=window");
    EXPECT_TRUE(wholeWindow.startsWith("HTTP/1.1 200")) << wholeWindow.toStdString();
    QImage windowImage(dir.path() + "/window.png");
    EXPECT_FALSE(windowImage.isNull());
    EXPECT_GE(windowImage.width(), w.main.width());
    EXPECT_GE(windowImage.height(), w.main.height());
    AgentHostMode::resetForTests();
}

TEST(AgentApiSymbol, OperationWaitsForActivationAndReportsRefusalOrTimeout) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QWidget window;
    window.resize(100, 100);
    window.show();
    AgentApi::StateSnapshot state;
    state.meta.symbol = "BTC-USD";
    quint64 renderedRevision = 0;
    quint64 publishedRevision = 0;
    GuiApiServer server(&window, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto&) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [](const auto&, auto complete) { complete(heatmap::WallsSnapshot{}); },
        [](const QString& kind, const AgentApi::ControlBody& body) {
            EXPECT_EQ(kind, "symbol");
            AgentApi::ControlApply result;
            result.pendingSymbol = body.symbol;
            result.data["symbol"] = body.symbol;
            return result;
        }, [&] { return std::pair<quint64, quint64>{renderedRevision, 77}; },
        [&](quint64 revision) { publishedRevision = revision; });
    ASSERT_TRUE(server.start(0, dir.path()));

    const auto post = [&](const char* symbol) {
        return requestPath(server.port(), "POST", "/api/v1/symbol",
                           QByteArray("{\"symbol\":\"") + symbol + "\"}");
    };
    const auto get = [&](const QString& id, int waitMs = 0) {
        return requestPath(server.port(), "GET", "/api/v1/operations/" + id.toUtf8()
                           + "?waitMs=" + QByteArray::number(waitMs));
    };

    const auto first = post("ETH-USD");
    ASSERT_TRUE(first.startsWith("HTTP/1.1 200")) << first.toStdString();
    const QString firstId = responseData(first).value("operationId").toString();
    EXPECT_EQ(responseData(first).value("status"), "pending");
    EXPECT_EQ(publishedRevision, 0u);
    renderedRevision = 100; // even a later frame cannot render an unactivated switch
    EXPECT_EQ(responseData(get(firstId)).value("status"), "pending");
    EXPECT_TRUE(requestPath(server.port(), "GET", "/api/v1/screenshot?name=before&target=main&afterOperation="
        + firstId.toUtf8() + "&waitMs=0").startsWith("HTTP/1.1 408"));
    renderedRevision = 0;
    server.completeSymbolSwitch("eth-usd");
    EXPECT_EQ(publishedRevision, 2u);
    EXPECT_EQ(responseData(get(firstId)).value("status"), "applied");
    renderedRevision = publishedRevision;
    EXPECT_EQ(responseData(get(firstId)).value("status"), "rendered");

    const auto refused = post("SOL-USD");
    const QString refusedId = responseData(refused).value("operationId").toString();
    server.failSymbolSwitch("SOL-USD", "connection_cap", "Cap of 8 reached");
    const auto refusedReply = get(refusedId, 5000);
    EXPECT_TRUE(refusedReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseError(refusedReply).value("code"), "connection_cap");
    EXPECT_EQ(responseError(refusedReply).value("message"), "Cap of 8 reached");

    const auto timedOut = post("ADA-USD");
    const QString timeoutId = responseData(timedOut).value("operationId").toString();
    server.failSymbolSwitch("ADA-USD", "switch_timeout", "Switch to ADA-USD timed out");
    EXPECT_EQ(responseError(get(timeoutId)).value("code"), "switch_timeout");
    EXPECT_EQ(responseError(get(timeoutId)).value("message"), "Switch to ADA-USD timed out");

    const auto disconnected = post("DOT-USD");
    const QString disconnectedId = responseData(disconnected).value("operationId").toString();
    server.failSymbolSwitch("DOT-USD", "disconnected", "Switch to DOT-USD interrupted by disconnection");
    EXPECT_EQ(responseError(get(disconnectedId)).value("code"), "disconnected");
    EXPECT_EQ(responseError(get(disconnectedId)).value("message"),
              "Switch to DOT-USD interrupted by disconnection");

    const auto waiting = post("XRP-USD");
    const QString waitingId = responseData(waiting).value("operationId").toString();
    QTimer::singleShot(50, &server, [&] {
        server.completeSymbolSwitch("XRP-USD");
        renderedRevision = publishedRevision;
    });
    const auto waited = get(waitingId, 1000);
    EXPECT_TRUE(waited.startsWith("HTTP/1.1 200")) << waited.toStdString();
    EXPECT_EQ(responseData(waited).value("status"), "rendered");
}

TEST(AgentApiDocks, OwnerManualToggleAfterApiChangeSurvivesClose) {
    QTemporaryDir dir;
    const auto previousFormat = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
    {
        Window w;
        w.docks.apply({}, "heatmap", false);
        w.orderBook.toggleViewAction()->trigger(); // owner opens it by hand
        w.orderBook.toggleViewAction()->trigger(); // then decides to leave it hidden
        QApplication::processEvents();
        w.docks.restoreBeforeSessionSave();
        EXPECT_FALSE(w.docks.snapshot().value("orderBook").toBool());
        EXPECT_TRUE(w.docks.snapshot().value("watchlist").toBool()); // untouched baseline restored
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        EXPECT_FALSE(settings.contains("agentApi/docks/visible"));
    }
    QSettings::setDefaultFormat(previousFormat);
}

TEST(AgentApiDocks, FocusRedocksFloatingTargetAndRaisesTabbedTarget) {
    Window w;
    w.watchlist.setFloating(true);
    QApplication::processEvents();
    ASSERT_TRUE(w.watchlist.isFloating());
    w.docks.apply({}, "watchlist", false);
    QApplication::processEvents();
    EXPECT_FALSE(w.watchlist.isFloating());
    EXPECT_NE(w.main.dockWidgetArea(&w.watchlist), Qt::NoDockWidgetArea);
    EXPECT_TRUE(w.docks.snapshot().value("watchlist").toBool());
    EXPECT_FALSE(w.docks.snapshot().value("heatmap").toBool());
    w.heatmap.show();
    w.main.tabifyDockWidget(&w.watchlist, &w.heatmap);
    w.watchlist.raise();
    QApplication::processEvents();
    QTabBar* dockTabs = nullptr;
    for (auto* tabs : w.main.findChildren<QTabBar*>()) {
        if (tabs->count() == 2 && tabs->tabText(0) == "Watchlist" && tabs->tabText(1) == "Heatmap") {
            dockTabs = tabs;
            break;
        }
    }
    ASSERT_NE(dockTabs, nullptr);
    EXPECT_EQ(dockTabs->tabText(dockTabs->currentIndex()), "Watchlist");
    EXPECT_TRUE(w.docks.snapshot().value("heatmap").toBool()) << "an inactive tab remains shown";
    w.docks.apply({}, "heatmap", false);
    QApplication::processEvents();
    EXPECT_EQ(dockTabs->tabText(dockTabs->currentIndex()), "Heatmap");
    EXPECT_TRUE(w.docks.snapshot().value("heatmap").toBool());
    EXPECT_FALSE(w.docks.snapshot().value("watchlist").toBool());
    EXPECT_GE(w.heatmap.width(), 850);
}

TEST(AgentApiDocks, RestoreIgnoresStaleIdsAndMalformedValues) {
    QTemporaryDir dir;
    QString error;
    const QString profile = dir.path() + "/profile/docks.ini";
    ASSERT_TRUE(AgentHostMode::activate(dir.path() + "/session", {}, &error, profile)) << qPrintable(error);
    {
        QSettings saved(profile, QSettings::IniFormat);
        saved.setValue("agentApi/docks/visible", QVariantMap{{"heatmap", true}, {"watchlist", false},
                                                               {"retiredDock", false}, {"orderBook", "bad"}});
        saved.sync();
    }
    Window w;
    w.docks.restore();
    const auto visible = w.docks.snapshot();
    EXPECT_EQ(visible.size(), 3);
    EXPECT_FALSE(visible.contains("retiredDock"));
    EXPECT_FALSE(visible.value("watchlist").toBool());
    EXPECT_TRUE(visible.value("orderBook").toBool());
    {
        QSettings saved(profile, QSettings::IniFormat);
        saved.setValue("agentApi/docks/visible", QVariantMap{{"heatmap", false}, {"watchlist", false},
                                                               {"orderBook", false}, {"retiredDock", true}});
        saved.sync();
    }
    Window another;
    another.docks.restore();
    const auto afterStaleOnly = another.docks.snapshot();
    EXPECT_TRUE(afterStaleOnly.value("heatmap").toBool());
    EXPECT_TRUE(afterStaleOnly.value("orderBook").toBool());
    EXPECT_TRUE(afterStaleOnly.value("watchlist").toBool());
    AgentHostMode::resetForTests();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
