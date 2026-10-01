#pragma once
#include "AgentApiTypes.hpp"
#include <QPointer>
#include <QQuickView>
#include <QRectF>
#include <memory>

class QTimer;

namespace AgentApi {
// GUI thread only. Coordinates are logical pixels relative to the named region.
// The chart rect is supplied by the host renderer, not hard-coded axis widths.
// A drag that receives no dragMove/dragEnd within the drag timeout is released
// at its last position (an agent that dies mid-drag never leaves a pressed button).
class InputDispatcher {
public:
    static constexpr int kDefaultDragTimeoutMs = 10'000;
    InputDispatcher();
    ~InputDispatcher();
    ControlApply apply(QQuickView *view, const QRectF &chartRect, const InputCommand &command);
    void setDragTimeoutMs(int ms);
    bool dragActive() const { return !dragView_.isNull(); }
private:
    void releaseDrag(); // timeout: the button goes up where the drag last was
    QPointer<QQuickView> dragView_;
    QString dragTarget_;
    QPointF dragLocal_, dragGlobal_;
    Qt::KeyboardModifiers dragModifiers_;
    std::unique_ptr<QTimer> dragTimer_;
};
} // namespace AgentApi
