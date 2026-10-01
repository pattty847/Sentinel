#include "AgentApiInput.hpp"
#include <QCoreApplication>
#include <QMouseEvent>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include "SentinelLogging.hpp"
#include <cmath>

namespace AgentApi {
InputDispatcher::InputDispatcher() : dragTimer_(std::make_unique<QTimer>()) {
    dragTimer_->setSingleShot(true);
    dragTimer_->setInterval(kDefaultDragTimeoutMs);
    QObject::connect(dragTimer_.get(), &QTimer::timeout, dragTimer_.get(), [this] { releaseDrag(); });
}
InputDispatcher::~InputDispatcher() = default;

void InputDispatcher::setDragTimeoutMs(int ms) { dragTimer_->setInterval(std::max(1, ms)); }

void InputDispatcher::releaseDrag() {
    if (QQuickView *view = dragView_.data()) {
        sLog_Warning("Agent API drag timed out on " << dragTarget_ << "; releasing at its last position");
        QMouseEvent event(QEvent::MouseButtonRelease, dragLocal_, dragLocal_, dragGlobal_, Qt::LeftButton, Qt::NoButton,
                          dragModifiers_);
        QCoreApplication::sendEvent(view, &event);
    }
    dragView_.clear();
    dragTarget_.clear();
}

ControlApply InputDispatcher::apply(QQuickView *view, const QRectF &chart, const InputCommand &c) {
    auto fail = [](int status, const char *code, const char *message) {
        ControlApply out;
        out.status = status; out.code = code; out.message = message;
        return out;
    };
    if (!view || !chart.isValid()) return fail(503, "input_unavailable", "Heatmap view is unavailable");
    if (QThread::currentThread() != view->thread()) return fail(503, "input_unavailable", "Input requires the GUI thread");
    QRectF region = chart;
    if (c.target == "priceAxis") region = {chart.right(), chart.top(), view->width() - chart.right(), chart.height()};
    else if (c.target == "timeAxis") region = {chart.left(), chart.bottom(), chart.width(), view->height() - chart.bottom()};
    else if (c.target != "chart") return fail(422, "invalid_target", "Unknown input target");
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || c.x < 0 || c.y < 0 || c.x >= region.width() || c.y >= region.height())
        return fail(422, "invalid_position", "Input lies outside the target region");
    const bool continuing = c.kind == "dragMove" || c.kind == "dragEnd";
    if (continuing && (dragView_ != view || dragTarget_ != c.target))
        return fail(409, "no_drag", "No drag is active on this target");
    if (!continuing && dragView_) return fail(409, "drag_active", "End the active drag first");
    const QPointF local = region.topLeft() + QPointF(c.x, c.y);
    const QPointF global = view->mapToGlobal(local);
    Qt::KeyboardModifiers modifiers;
    for (const auto &m : c.modifiers) {
        if (m == "shift") modifiers |= Qt::ShiftModifier;
        else if (m == "control") modifiers |= Qt::ControlModifier;
        else if (m == "alt") modifiers |= Qt::AltModifier;
        else if (m == "meta") modifiers |= Qt::MetaModifier;
        else return fail(422, "invalid_modifiers", "Unknown modifier");
    }
    auto mouse = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, local, global, button, buttons, modifiers);
        QCoreApplication::sendEvent(view, &event);
    };
    if (c.kind == "wheel") {
        if (!c.deltaY || (c.deltaY < -12000 || c.deltaY > 12000)) return fail(422, "invalid_delta", "Wheel deltaY must be nonzero and within +/-12000");
        QWheelEvent event(local, global, {}, QPoint(0, c.deltaY), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(view, &event);
    } else if (c.kind == "dragStart") {
        dragView_ = view;
        dragTarget_ = c.target;
        dragLocal_ = local; dragGlobal_ = global; dragModifiers_ = modifiers;
        mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
        dragTimer_->start();
    } else if (c.kind == "dragMove") {
        dragLocal_ = local; dragGlobal_ = global;
        mouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton);
        dragTimer_->start();
    } else if (c.kind == "dragEnd") {
        dragTimer_->stop();
        mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
        dragView_.clear(); dragTarget_.clear();
    } else if (c.kind == "click") {
        mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    } else return fail(422, "invalid_kind", "Unknown input kind");
    ControlApply out;
    out.data = {{"kind", c.kind}, {"target", c.target}};
    return out;
}
} // namespace AgentApi
