#pragma once

class QEvent;
class QMouseEvent;
class QQuickItem;
class QWheelEvent;
class GridViewState;
namespace chart { class ChartViewController; }

namespace chart {
class ChartInputRouter {
public:
    ChartInputRouter(QQuickItem& item, ChartViewController& view) : item_(item), view_(view) {}
    void mousePress(QMouseEvent* event);
    void mouseMove(QMouseEvent* event);
    void mouseRelease(QMouseEvent* event);
    void wheel(QWheelEvent* event);
    bool nativeGesture(QEvent* event);
private:
    GridViewState* getViewState() const;
    bool isVisible() const;
    void update();
    void endPanAt();
    QQuickItem& item_;
    ChartViewController& view_;
};
}
