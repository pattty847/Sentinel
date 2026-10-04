#include "mainwindow/AgentApiInput.hpp"
#include "mainwindow/GuiApiServer.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <gtest/gtest.h>

namespace {
struct Events : QObject {
    int wheels = 0, presses = 0, moves = 0, releases = 0;
    QPointF position, releasePosition;
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
        if (e->type() == QEvent::MouseButtonRelease) {
            ++releases; releasePosition = static_cast<QMouseEvent *>(e)->position(); return true;
        }
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
    GuiApiServer server(nullptr, &view, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
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
// S6a review minor 6: an agent that dies mid-drag never leaves the button down.
TEST(AgentApiInput, AnAbandonedDragIsReleasedAfterTheTimeout) {
    QQuickView view;
    view.resize(640, 480);
    Events events;
    view.installEventFilter(&events);
    AgentApi::InputDispatcher dispatcher;
    dispatcher.setDragTimeoutMs(50);
    AgentApi::InputCommand c{"dragStart", "timeAxis", 100, 10, 0, {}};
    ASSERT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    c = {"dragMove", "timeAxis", 120, 12, 0, {}};
    ASSERT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    EXPECT_TRUE(dispatcher.dragActive());
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(events.releases, 0) << "a move restarts the timeout";
    QTimer::singleShot(150, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(events.releases, 1) << "released once";
    EXPECT_EQ(events.releasePosition, QPointF(120, 462)) << "where the drag last was (time axis below the chart)";
    EXPECT_FALSE(dispatcher.dragActive());
    c.kind = "dragEnd";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 409) << "the drag is over";
    c = {"dragStart", "chart", 10, 10, 0, {}};
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200) << "a new drag can start";
    c.kind = "dragEnd";
    EXPECT_EQ(dispatcher.apply(&view, {0, 0, 600, 450}, c).status, 200);
    EXPECT_EQ(events.releases, 2);
}
TEST(AgentApiWallsRoute, GpuReturns200ValidatesPeriodAndKeepsSelectionEpoch409) {
    AgentApi::StateSnapshot state; state.meta.symbol = "BTC-USD"; state.meta.selectionEpoch = 1;
    bool changeSelection = false;
    auto wallError = heatmap_window::WallError::None;
    double expectedTick = 5;
    int scans = 0;
    QObject context;
    GuiApiServer server(nullptr, nullptr, [&] { return state; }, [] { return AgentApi::ViewportSnapshot{}; },
        [](const auto&) { return std::optional<AgentApi::CandleSnapshot>{}; },
        [](int) { return AgentApi::BookSnapshot{}; }, [](qint64, int) { return AgentApi::TradesSnapshot{}; },
        [&](const heatmap_window::WallQuery& q, auto complete) {
            ++scans;
            EXPECT_EQ(q.startMs, 100); EXPECT_EQ(q.endMs, 200); EXPECT_EQ(q.tick, expectedTick);
            QMetaObject::invokeMethod(&context, [&, complete = std::move(complete)] {
                if (changeSelection) ++state.meta.selectionEpoch;
                heatmap_window::WallsSnapshot result; result.gpuRenderer = true; result.bandTick = 5;
                result.recordedColumns = 1; result.rangeStartMs = 100; result.rangeEndMs = 200;
                result.error = wallError;
                if (wallError != heatmap_window::WallError::None) result.status = 422;
                complete(result);
            }, Qt::QueuedConnection);
        }, [](const QString&, const AgentApi::ControlBody&) { return AgentApi::ControlApply{}; },
        [] { return std::pair<quint64, quint64>{0, 0}; }, [](quint64) {});
    ASSERT_TRUE(server.start(0, "."));
    auto get = [&](QByteArray query) {
        QTcpSocket socket; QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
            socket.write("GET /api/v1/heatmap/walls?" + query + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        });
        socket.connectToHost("127.0.0.1", server.port()); timeout.start(3000); loop.exec();
        return socket.readAll();
    };
    const auto ok = get("from_ms=100&to_ms=200&tick=5");
    EXPECT_TRUE(ok.startsWith("HTTP/1.1 200")) << ok.toStdString();
    EXPECT_TRUE(ok.contains("\"renderer\":\"gpu\"")); EXPECT_EQ(scans, 1);
    for (const auto* bad : {"from_ms=100", "from_ms=200&to_ms=100", "tick=0", "tick=nan"}) {
        const auto rejected = get(bad);
        EXPECT_TRUE(rejected.startsWith("HTTP/1.1 422")) << rejected.toStdString();
    }
    EXPECT_EQ(scans, 1);
    wallError = heatmap_window::WallError::BadTick; expectedTick = 1.234;
    const auto badTick = get("from_ms=100&to_ms=200&tick=1.234");
    EXPECT_TRUE(badTick.startsWith("HTTP/1.1 422"));
    EXPECT_TRUE(badTick.contains("bad_tick"));
    EXPECT_FALSE(badTick.contains("scan_limit"));
    wallError = heatmap_window::WallError::ScanLimit; expectedTick = 5;
    const auto limited = get("from_ms=100&to_ms=200&tick=5");
    EXPECT_TRUE(limited.startsWith("HTTP/1.1 422"));
    EXPECT_TRUE(limited.contains("scan_limit"));
    wallError = heatmap_window::WallError::None;
    changeSelection = true;
    const auto changed = get("from_ms=100&to_ms=200&tick=5");
    EXPECT_TRUE(changed.startsWith("HTTP/1.1 409")) << changed.toStdString();
    EXPECT_TRUE(changed.contains("selection_changed"));
}

}
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
