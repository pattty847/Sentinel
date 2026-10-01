#pragma once
#include "AgentApiTypes.hpp"
#include <QPointer>
#include <QQuickView>
#include <QRectF>

namespace AgentApi {
// GUI thread only. Coordinates are logical pixels relative to the named region.
// The chart rect is supplied by the host renderer, not hard-coded axis widths.
class InputDispatcher {
public:
    ControlApply apply(QQuickView *view, const QRectF &chartRect, const InputCommand &command);
private:
    QPointer<QQuickView> dragView_;
    QString dragTarget_;
};
} // namespace AgentApi
