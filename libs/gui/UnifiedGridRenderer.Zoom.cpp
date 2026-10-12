// Qt event adaptation and frame scheduling for chart zoom policies.
#include "UnifiedGridRenderer.h"
#include "chart/shell/ChartInputRouter.hpp"
#include <QTimer>
void UnifiedGridRenderer::bindZoomGlide() {
  connect(&m_view->zoom(), &chart::ZoomController::updateRequested, this, [this] { update(); }, Qt::DirectConnection);
  auto& glide = m_view->zoomGlide();
  glide.setClock([this] { return zoomNowMs(); });
  connect(&glide, &chart::ZoomGlide::rasterChanged, this, &UnifiedGridRenderer::rasterChanged, Qt::DirectConnection);
  connect(&glide, &chart::ZoomGlide::viewportChanged, view(), &chart::ChartViewQml::viewportChanged, Qt::DirectConnection);
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
  connect(view(), &chart::ChartViewQml::heatmapTickSizeChanged, context, onChange);
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

bool UnifiedGridRenderer::event(QEvent* e) {
  return chart::ChartInputRouter(*this, *m_view).nativeGesture(e) || QQuickItem::event(e);
}
