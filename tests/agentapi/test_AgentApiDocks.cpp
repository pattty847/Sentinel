#include "mainwindow/DockVisibilityController.hpp"
#include "mainwindow/GuiApiServer.h"
#include "config/AgentHostMode.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QJsonDocument>
#include <QSettings>
#include <QTabBar>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

namespace {
QByteArray request(quint16 port, const QByteArray& method, const QByteArray& body = {}) {
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        QByteArray head = method + " /api/v1/docks HTTP/1.1\r\nHost: localhost\r\n";
        if (method == "POST")
            head += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        socket.write(head + "\r\n" + body);
    });
    socket.connectToHost("127.0.0.1", port);
    timer.start(3000);
    loop.exec();
    return socket.readAll();
}

QJsonObject responseData(const QByteArray& reply) {
    return QJsonDocument::fromJson(reply.mid(reply.indexOf("\r\n\r\n") + 4)).object().value("data").toObject();
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
    GuiApiServer server(&w.main, nullptr, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto&) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [](const auto&, auto complete) { complete(heatmap_window::WallsSnapshot{}); },
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
