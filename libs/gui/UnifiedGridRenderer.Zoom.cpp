// Qt event adaptation and frame scheduling for chart zoom policies.
#include "UnifiedGridRenderer.h"
#include "chart/shell/ChartInputRouter.hpp"
#include <QTimer>
void UnifiedGridRenderer::bindZoomGlide() {
  connect(&m_view->zoom(), &chart::ZoomController::updateRequested, this, [this] { update(); }, Qt::DirectConnection);
  auto& glide = m_view->zoomGlide();
  glide.setClock([this] { return zoomNowMs(); });
  connect(&glide, &chart::ZoomGlide::rasterChanged, this, &UnifiedGridRenderer::rasterChanged, Qt::DirectConnection);
  connect(&glide, &chart::ZoomGlide::viewportChanged, this, &UnifiedGridRenderer::viewportChanged, Qt::DirectConnection);
  connect(&glide, &chart::ZoomGlide::polishRequested, this, [this] { polish(); }, Qt::DirectConnection);
  connect(&glide, &chart::ZoomGlide::updateRequested, this, [this] { update(); }, Qt::DirectConnection);
}

qint64 UnifiedGridRenderer::zoomNowMs() const {
  return m_clock.zoom ? m_clock.zoom() : (m_clock.frame.isValid() ? m_clock.frame.elapsed() : 0);
}

chart_raster::RasterCamera UnifiedGridRenderer::rasterCameraNow(bool includeDrag) const {
  Q_ASSERT(m_view);
  return m_view->camera().rasterCameraNow(includeDrag);
}

chart_raster::RasterCamera UnifiedGridRenderer::restCameraNow(bool includeDrag) const {
  Q_ASSERT(m_view);
  return m_view->camera().restCameraNow(includeDrag);
}

void UnifiedGridRenderer::connectCameraChanged(QObject* context, std::function<void()> onChange) {
  connect(this, &UnifiedGridRenderer::rasterChanged, context, onChange);
  connect(this, &UnifiedGridRenderer::heatmapTickSizeChanged, context, onChange);
}

void UnifiedGridRenderer::updatePolish() {
  QQuickItem::updatePolish();
  if (zoomGliding()) m_view->advanceFrame(zoomNowMs());
  if (!zoomGliding()) return;
  if (!m_clock.glidePolish) {
    m_clock.glidePolish = new QTimer(this);
    m_clock.glidePolish->setSingleShot(true);
    m_clock.glidePolish->setInterval(0);
    connect(m_clock.glidePolish, &QTimer::timeout, this, [this] {
      if (zoomGliding()) {
        polish();
        update();
      }
    });
  }
  m_clock.glidePolish->start();
}

void UnifiedGridRenderer::zoomClicks(int clicks, double x, double y, bool time, bool price) {
  m_view->zoom().zoomClicks(clicks, x, y, time, price);
}

void UnifiedGridRenderer::zoomContinuous(double factor, double x, double y, bool time, bool price) {
  m_view->zoom().zoomContinuous(factor, x, y, time, price);
}

void UnifiedGridRenderer::endZoomGesture() {
  m_view->zoom().endZoomGesture();
}

void UnifiedGridRenderer::zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x) {
  m_view->zoom().zoomTimeWheel(angleDelta, pixelDelta, phase, x);
}

void UnifiedGridRenderer::zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y) {
  m_view->zoom().zoomPriceWheel(angleDelta, pixelDelta, phase, y);
}

void UnifiedGridRenderer::zoomTimeDrag(double delta, double x) {
  m_view->zoom().zoomTimeDrag(delta, x);
}

void UnifiedGridRenderer::zoomPriceDrag(double delta, double y) {
  m_view->zoom().zoomPriceDrag(delta, y);
}

void UnifiedGridRenderer::zoomTimeClicks(int clicks, double x) {
  m_view->zoom().zoomTimeClicks(clicks, x);
}

void UnifiedGridRenderer::zoomPriceClicks(int clicks, double y) {
  m_view->zoom().zoomPriceClicks(clicks, y);
}

bool UnifiedGridRenderer::event(QEvent* e) {
  return chart::ChartInputRouter(*this, *m_view).nativeGesture(e) || QQuickItem::event(e);
}
