#include "mainwindow/AgentApiInput.hpp"
#include "mainwindow/GuiApiServer.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>
#include <QWheelEvent>
#include <gtest/gtest.h>

namespace {
struct Events : QObject {
    int wheels = 0, presses = 0, moves = 0, releases = 0;
    QPointF position;
    Qt::KeyboardModifiers modifiers;
    int delta = 0;
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::Wheel) {
            auto *w = static_cast<QWheelEvent *>(e);
            ++wheels; position = w->position(); modifiers = w->modifiers(); delta = w->angleDelta().y();
            return true;
        }
        if (e->type() == QEvent::MouseButtonPress) { ++presses; return true; }
        if (e->type() == QEvent::MouseMove) { ++moves; return true; }
        if (e->type() == QEvent::MouseButtonRelease) { ++releases; return true; }
        return false;
    }
};
TEST(AgentApiInput, HttpHandlerDeliversOneWheelToTheViewAndReturnsAnOperation) {
    QQuickView view;
    view.resize(640, 480);
    Events events;
    view.installEventFilter(&events);
    AgentApi::InputDispatcher dispatcher;
    AgentApi::StateSnapshot state;
    state.meta.symbol = "BTC-USD";
    quint64 revision = 0;
    GuiApiServer server(nullptr, &view, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto &) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [](const auto &, auto complete) { complete(heatmap_window::WallsSnapshot{}); },
        [&](const QString &kind, const AgentApi::ControlBody &body) {
            EXPECT_EQ(kind, "input");
            return dispatcher.apply(&view, {0, 0, 600, 450}, body.input);
        }, [] { return std::pair<quint64, quint64>{0, 0}; }, [&](quint64 r) { revision = r; });
    ASSERT_TRUE(server.start(0, "."));
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        const QByteArray body = R"({"kind":"wheel","target":"priceAxis","x":10,"y":25,"deltaY":120,"modifiers":["shift"]})";
        socket.write("POST /api/v1/input HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    });
    socket.connectToHost("127.0.0.1", server.port());
    timeout.start(3000);
    loop.exec();
    const auto reply = socket.readAll();
    ASSERT_TRUE(reply.startsWith("HTTP/1.1 200")) << reply.toStdString();
    const auto json = QJsonDocument::fromJson(reply.mid(reply.indexOf("\r\n\r\n") + 4)).object();
    EXPECT_EQ(json["data"].toObject()["operationId"], "o1");
    EXPECT_EQ(json["data"].toObject()["status"], "applied");
    EXPECT_EQ(revision, 1);
    EXPECT_EQ(events.wheels, 1);
    EXPECT_EQ(events.position, QPointF(610, 25));
    EXPECT_EQ(events.delta, 120);
    EXPECT_TRUE(events.modifiers.testFlag(Qt::ShiftModifier));
}
TEST(AgentApiInput, BoundsAndDragSequenceAreValidatedBeforeSending) {
    QQuickView view;
    view.resize(640, 480);
    Events events;
    view.installEventFilter(&events);
    AgentApi::InputDispatcher dispatcher;
    AgentApi::InputCommand c{"wheel", "chart", 600, 10, 120, {}};
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 422);
    EXPECT_EQ(events.wheels, 0);
    c = {"dragMove", "timeAxis", 100, 10, 0, {}};
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 409);
    c.kind = "dragStart";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 409);
    c.kind = "dragMove";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    c.kind = "dragEnd";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    EXPECT_EQ(events.presses, 1); EXPECT_EQ(events.moves, 1); EXPECT_EQ(events.releases, 1);
    c.kind = "click";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    EXPECT_EQ(events.presses, 2); EXPECT_EQ(events.releases, 2);
}
}
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
