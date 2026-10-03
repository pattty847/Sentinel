#include "mainwindow/DockVisibilityController.hpp"
#include "mainwindow/GuiApiServer.h"

#include <QApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QJsonDocument>
#include <QSettings>
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

TEST(AgentApiDocks, IsolatedProfileRestoresAndTransientOwnerChangeDoesNotSave) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto previousFormat = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.remove("agentApi/docks");
        settings.sync();
    }
    {
        Window first;
        first.docks.apply({}, "heatmap", true); // hosted mode always persists immediately
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        EXPECT_TRUE(settings.contains("agentApi/docks/visible"));
    }
    {
        Window next;
        next.docks.restore();
        const auto visible = next.docks.snapshot();
        EXPECT_TRUE(visible.value("heatmap").toBool());
        EXPECT_FALSE(visible.value("orderBook").toBool());
        EXPECT_FALSE(visible.value("watchlist").toBool());
        next.docks.apply(QJsonObject{{"orderBook", true}}, {}, false); // owner request without persist
        next.docks.restoreBeforeSessionSave();
        EXPECT_FALSE(next.docks.snapshot().value("orderBook").toBool());
    }
    {
        Window last;
        last.docks.restore();
        EXPECT_FALSE(last.docks.snapshot().value("orderBook").toBool());
    }
    QSettings::setDefaultFormat(previousFormat);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
