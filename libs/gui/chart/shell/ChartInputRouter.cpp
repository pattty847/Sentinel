#include "ChartInputRouter.hpp"
#include "chart/core/ChartViewController.hpp"
#include "SentinelLogging.hpp"
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QQuickItem>
#include <QWheelEvent>

static_assert(int(Qt::NoScrollPhase) == chart::WheelInterpreter::kNoScrollPhase);
static_assert(int(Qt::ScrollEnd) == chart::WheelInterpreter::kScrollEnd);

GridViewState* chart::ChartInputRouter::getViewState() const { return view_.viewPtr(); }
bool chart::ChartInputRouter::isVisible() const { return item_.isVisible(); }
void chart::ChartInputRouter::update() { item_.update(); }
void chart::ChartInputRouter::endPanAt() { view_.commands().endPanAt(); }

void chart::ChartInputRouter::mousePress(QMouseEvent *event) {
  if (getViewState() && isVisible() && event->button() == Qt::LeftButton) {
    if (getViewState()->isAutoScrollEnabled()) {
      getViewState()->enableAutoScroll(false);
      sLog_Render("auto-scroll enabled=false reason=mouse_pan");
    }
    getViewState()->handlePanStart(event->position());
    event->accept();
  } else
    event->ignore();
}

void chart::ChartInputRouter::mouseMove(QMouseEvent *event) {
  if (getViewState()) {
    getViewState()->handlePanMove(event->position());
    event->accept();
    update();
  } else
    event->ignore();
}

void chart::ChartInputRouter::mouseRelease(QMouseEvent *event) {
  if (getViewState()) {
    endPanAt();
    event->accept();
  }
}

void chart::ChartInputRouter::wheel(QWheelEvent *event) {
  if (!getViewState() || !isVisible() || !getViewState()->isTimeWindowValid()) {
    event->ignore();
    return;
  }
  // Shift+wheel scales price only; macOS delivers it as a
  // horizontal delta, so whichever axis moved is used. A price zoom takes price over.
  const bool shift = event->modifiers() & Qt::ShiftModifier;
  const int angle = shift && event->angleDelta().y() == 0 ? event->angleDelta().x() : event->angleDelta().y();
  if (shift) getViewState()->setAutoPriceScale(false);
  const QPointF at = event->position();
  view_.zoom().wheelZoom(angle, event->pixelDelta().isNull(), event->phase(),
            event->source() == Qt::MouseEventSynthesizedBySystem,
            at.x(), at.y(), !shift, true, chart::ZoomController::WheelRoute::Chart);
  update();
  event->accept();
}

bool chart::ChartInputRouter::nativeGesture(QEvent* e) {
  if (e->type() == QEvent::NativeGesture) {
    auto* gesture = static_cast<QNativeGestureEvent*>(e);
    switch (gesture->gestureType()) {
    case Qt::ZoomNativeGesture: {
      const QPointF at = gesture->position();
      const bool shift = gesture->modifiers() & Qt::ShiftModifier;
      if (shift && getViewState()) getViewState()->setAutoPriceScale(false);
      view_.zoom().zoomContinuousFor(1.0 + gesture->value(), at.x(), at.y(), !shift, true,
                                      chart::ZoomController::kGestureSafetyMs);
      gesture->accept();
      return true;
    }
    case Qt::EndNativeGesture:
      view_.zoom().endZoomGesture();
      gesture->accept();
      return true;
    case Qt::BeginNativeGesture:
      gesture->accept();
      return true;
    default:
      break;
    }
  }
  return false;
}
