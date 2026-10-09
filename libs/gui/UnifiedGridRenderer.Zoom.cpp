// Whole-pixel smooth zoom (slice A2): the zoom ladder, the glide and the continuous
// (pinch / trackpad) zoom. GUI thread; the frame (prepareSyncFrame) reads the glide
// state while the GUI thread is blocked.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "render/FrameContextBuilder.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"

#include <QNativeGestureEvent>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
// A trackpad scroll settles on a rung after this pause (a scroll without phases, or a
// pause with the fingers on the pad).
constexpr int kGestureSettleMs = 160;
// A gesture with an explicit end (pinch, axis drag) settles after this pause if its
// end never arrives.
constexpr int kGestureSafetyMs = 1000;
// The continuous zoom of one event, as the wheel always scaled it (>1 zooms in).
double continuousFactor(double delta) {
  return 1.0 + std::clamp(delta * GridViewState::ZOOM_SENSITIVITY, -GridViewState::MAX_ZOOM_DELTA,
                          GridViewState::MAX_ZOOM_DELTA);
}
} // namespace

qint64 UnifiedGridRenderer::zoomNowMs() const {
  return m_zoomClock ? m_zoomClock() : (m_frameClock.isValid() ? m_frameClock.elapsed() : 0);
}

UnifiedGridRenderer::Cameras UnifiedGridRenderer::camerasFor(const chart_raster::RasterInputs& in,
                                                             chart_raster::RasterStep previous) const {
  Cameras c;
  c.rest = chart_raster::computeRaster(in, previous);
  c.drawn = c.rest;
  if (!c.rest.valid) return c;
  if (m_zoomGesture) {
    c.drawn = chart_raster::continuousRaster(in);
    if (!c.drawn.valid) c.drawn = c.rest;
    return c;
  }
  if (!m_glide.active) return c;
  // A drag during a glide moves both ends by the same whole device pixels (1:1).
  const double dpr = c.rest.dpr;
  const double dx = std::isfinite(in.dragLogicalPx.x()) ? double(std::llround(in.dragLogicalPx.x() * dpr)) : 0.0;
  const double dy = std::isfinite(in.dragLogicalPx.y()) ? double(std::llround(in.dragLogicalPx.y() * dpr)) : 0.0;
  const auto from = chart_raster::shiftedRaster(m_glide.from, dx, dy);
  c.drawn = chart_raster::glideRaster(from, c.rest, m_glide.anchorTime, m_glide.anchorPrice, m_glide.progress);
  if (c.drawn.free) {
    // Bin the glide's whole extent (its start and its target) once, at its start.
    c.hasBinView = true;
    c.binTimeLo = std::min(from.drawnStartMs, c.rest.drawnStartMs);
    c.binTimeHi = std::max(from.drawnEndMs, c.rest.drawnEndMs);
    c.binPriceLo = std::min(from.drawnMinPrice, c.rest.drawnMinPrice);
    c.binPriceHi = std::max(from.drawnMaxPrice, c.rest.drawnMaxPrice);
  }
  return c;
}

chart_raster::RasterCamera UnifiedGridRenderer::rasterCameraNow(bool includeDrag) const {
  if (!m_viewState || !m_gpuLayer) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(m_viewState.get());
  if (!includeDrag) viewport.dragging = false;
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  const auto in = rasterInputs(viewport, m_gpuLayer->tickPrice(), width(), height(), dpr);
  chart_raster::RasterStep previous;
  {
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    previous = m_rasterStep;
  }
  return camerasFor(in, previous).drawn;
}

chart_raster::RasterCamera UnifiedGridRenderer::restCameraNow(bool includeDrag) const {
  if (!m_viewState || !m_gpuLayer) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(m_viewState.get());
  if (!includeDrag) viewport.dragging = false;
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  const auto in = rasterInputs(viewport, m_gpuLayer->tickPrice(), width(), height(), dpr);
  chart_raster::RasterStep previous;
  {
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    previous = m_rasterStep;
  }
  return chart_raster::computeRaster(in, previous);
}

chart_raster::TickAt UnifiedGridRenderer::tickPredictor(qint64 start, qint64 end, double anchorPrice,
                                                       double fracY) const {
  // The window a price span is drawn with: about the anchor (Auto judges the rows in
  // view, so the window matters, not only its span).
  return [this, start, end, anchorPrice, fracY](double span) -> double {
    if (!m_gpuLayer || !(span > 0)) return 0.0;
    const double hi = anchorPrice + fracY * span;
    const heatmap::gpu::ViewWindow view{double(start), double(end), hi - span, hi};
    const int64_t units = m_gpuLayer->predictTickUnits(view);
    return units > 0 ? heatmap::fromUnits(units, m_gpuLayer->priceScale()) : 0.0;
  };
}

void UnifiedGridRenderer::endZoomGlide() {
  if (!m_glide.active) return;
  m_glide.active = false;
  m_glide.progress = 1.0;
  update();
}

void UnifiedGridRenderer::startZoomGlide(const chart_raster::RasterCamera& from, double anchorTime,
                                         double anchorPrice, int colPx, chart_raster::RowRung row) {
  m_glide.active = true;
  m_glide.from = from;
  m_glide.anchorTime = anchorTime;
  m_glide.anchorPrice = anchorPrice;
  m_glide.startMs = zoomNowMs();
  m_glide.progress = 0.0;
  m_glide.colPx = colPx;
  m_glide.row = row;
  sLog_Probe("zoom.glide", "start colPx=" << from.colPxF << "->" << colPx << " rowPx=" << from.rowPxF << "->"
             << row.rowPx << " tick=" << from.tick << "->" << row.tick);
  emit rasterChanged();
  emit viewportChanged();
  polish(); // the next frame's updatePolish advances it
  update();
}

void UnifiedGridRenderer::updatePolish() {
  QQuickItem::updatePolish();
  advanceZoomGlide();
  if (!m_glide.active) return;
  if (!m_glidePolishTimer) {
    m_glidePolishTimer = new QTimer(this);
    m_glidePolishTimer->setSingleShot(true);
    m_glidePolishTimer->setInterval(0);
    connect(m_glidePolishTimer, &QTimer::timeout, this, [this] {
      if (m_glide.active) {
        polish();
        update();
      }
    });
  }
  m_glidePolishTimer->start();
}

void UnifiedGridRenderer::advanceZoomGlide() {
  if (!m_glide.active) return;
  const double t = double(zoomNowMs() - m_glide.startMs) / double(chart_raster::kZoomGlideMs);
  m_glide.progress = chart_raster::easeZoom(t);
  // The frame that reaches the end draws the rest camera: the landing is the rest frame.
  if (t >= 1.0) {
    m_glide.active = false;
    m_glide.progress = 1.0;
    sLog_Probe("zoom.glide", "landed");
  }
  // This frame's camera for every layer: the axis models recalculate, the sibling
  // overlays (candles, paper, algo) redraw in this synchronization.
  emit rasterChanged();
  emit viewportChanged();
  update();
}

bool UnifiedGridRenderer::setZoomRung(const chart_raster::RasterCamera& drawn, double fracX, double fracY,
                                      bool time, int colPx, bool price, chart_raster::RowRung row) {
  if (!m_viewState || !drawn.valid) return false;
  qint64 start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  double lo = m_viewState->getMinPrice(), hi = m_viewState->getMaxPrice();
  const double tf = double(m_currentTimeframe_ms);
  if (time && colPx > 0 && tf > 0) {
    const int64_t span = std::max<int64_t>(1, std::llround(double(drawn.widthDev) * tf / double(colPx)));
    const double at = drawn.timeAtXDev(fracX * double(drawn.widthDev));
    start = std::llround(at - fracX * double(span));
    end = start + span;
  }
  // row.tick < 0: no tick drawn yet, -row.tick is the price span itself (no rung).
  if (price && ((row.rowPx > 0 && row.tick > 0) || row.tick < 0)) {
    const double span = row.tick < 0 ? -row.tick : double(drawn.heightDev) * row.tick / double(row.rowPx);
    const double ap = drawn.priceAtYDev(fracY * double(drawn.heightDev));
    hi = ap + fracY * span;
    lo = hi - span;
  }
  const auto anchor = m_viewState->rasterAnchor();
  m_viewState->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  m_viewState->setViewport(start, end, lo, hi); // one change; the auto price fit applies inside
  return true;
}

void UnifiedGridRenderer::zoomClicks(int clicks, double x, double y, bool time, bool price) {
  if (!m_viewState || !m_gpuLayer || !m_viewState->isTimeWindowValid() || clicks == 0 ||
      m_viewState->isDragging())
    return;
  if (m_zoomGesture) endZoomGesture();
  // The auto price scale owns price: the chart wheel zooms time only.
  price = price && !m_viewState->autoPriceScale();
  const auto drawn = rasterCameraNow(false); // what is on screen now (a glide in flight included)
  if (!drawn.valid || (!time && !price)) return;
  const double W = drawn.widthDev, H = drawn.heightDev, tf = double(m_currentTimeframe_ms);
  const double fracX = std::clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0);
  // Rapid clicks step on from the glide's target, so they accumulate monotonically.
  const int baseCol = m_glide.active ? m_glide.colPx : drawn.colPx;
  const chart_raster::RowRung baseRow =
      m_glide.active ? m_glide.row : chart_raster::RowRung{drawn.tick, drawn.rowPx};
  const double maxTime = m_viewState->maxTimeSpanMs(), minTime = m_viewState->minTimeSpanMs();
  const int minCol = maxTime > 0 ? std::max(1, int(std::ceil(W * tf / maxTime - 1e-9))) : 1;
  const int maxCol = std::min(chart_raster::maxColumnPixels(tf),
                              minTime > 0 ? std::max(1, int(std::floor(W * tf / minTime + 1e-9)))
                                          : chart_raster::kMaxCellPx);
  const int col = time ? chart_raster::columnRung(baseCol, clicks, minCol, maxCol) : baseCol;
  const qint64 targetSpan = col > 0 && tf > 0 ? std::llround(W * tf / double(col))
                                              : m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart();
  const double at = drawn.timeAtXDev(fracX * W), ap = drawn.priceAtYDev(fracY * H);
  const qint64 targetStart = std::llround(at - fracX * double(targetSpan));
  chart_raster::RowRung row = baseRow;
  if (price && m_gpuLayer->tickPrice() > 0) {
    row = chart_raster::rowRung(baseRow, clicks, int(H), m_viewState->minPriceSpan(), m_viewState->maxPriceSpan(),
                                tickPredictor(targetStart, targetStart + targetSpan, ap, fracY));
  } else if (price) {
    // No tick drawn yet (no rows to whole-pixel): the price span by the ratio, in limits.
    double span = (m_viewState->getMaxPrice() - m_viewState->getMinPrice()) /
                  std::pow(chart_raster::kZoomStepRatio, double(clicks));
    if (m_viewState->maxPriceSpan() > 0) span = std::min(span, m_viewState->maxPriceSpan());
    if (m_viewState->minPriceSpan() > 0) span = std::max(span, m_viewState->minPriceSpan());
    row = {-span, 0};
  }
  const bool zoomTime = time && col != baseCol, zoomPrice = price && !(row == baseRow);
  if (!zoomTime && !zoomPrice) return; // at the limits
  sLog_Probe("zoom.click", "clicks=" << clicks << " col=" << baseCol << "->" << col << " row=" << baseRow.rowPx
             << "@" << baseRow.tick << "->" << row.rowPx << "@" << row.tick);
  setZoomRung(drawn, fracX, fracY, zoomTime, col, zoomPrice, row);
  if (zoomPrice) emit m_viewState->priceInteracted();
  if (m_viewState->isAutoScrollEnabled()) enableAutoScroll(false);
  startZoomGlide(drawn, at, ap, zoomTime ? col : baseCol,
                 zoomPrice && row.tick > 0 ? row : chart_raster::RowRung{drawn.tick, drawn.rowPx});
}

void UnifiedGridRenderer::zoomContinuous(double factor, double x, double y, bool time, bool price) {
  zoomContinuousFor(factor, x, y, time, price, kGestureSettleMs);
}

void UnifiedGridRenderer::zoomContinuousFor(double factor, double x, double y, bool time, bool price,
                                            int settleMs) {
  if (!m_viewState || !m_gpuLayer || !m_viewState->isTimeWindowValid() || !std::isfinite(factor) ||
      !(factor > 0) || factor == 1.0 || m_viewState->isDragging())
    return;
  price = price && !m_viewState->autoPriceScale();
  if (!time && !price) return;
  const double fracX = std::clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0);
  if (!m_zoomGesture) {
    // The gesture starts from what is drawn: the stored view becomes the drawn window,
    // so the continuous camera starts exactly on the picture (no jump).
    const auto drawn = rasterCameraNow(false);
    endZoomGlide();
    if (!drawn.valid) return;
    const auto anchor = m_viewState->rasterAnchor();
    m_viewState->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
    m_viewState->setViewport(std::llround(drawn.drawnStartMs), std::llround(drawn.drawnEndMs), drawn.drawnMinPrice,
                             drawn.drawnMaxPrice);
    m_zoomGesture = true;
    sLog_Probe("zoom.gesture", "begin");
  }
  m_gestureTime = time;
  m_gesturePrice = price;
  m_gestureAt = QPointF(x, y);
  const auto cam = rasterCameraNow(false); // the continuous camera
  if (!cam.valid) return;
  const double W = cam.widthDev, H = cam.heightDev;
  const double at = cam.timeAtXDev(fracX * W), ap = cam.priceAtYDev(fracY * H);
  qint64 start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  double lo = m_viewState->getMinPrice(), hi = m_viewState->getMaxPrice();
  if (time) {
    // Inside the zoom limits (one column per pixel out; the zoom-in floor in).
    const double maxTime = m_viewState->maxTimeSpanMs(), minTime = m_viewState->minTimeSpanMs();
    double span = double(end - start) / factor;
    if (maxTime > 0) span = std::min(span, maxTime);
    if (minTime > 0) span = std::max(span, std::min(double(end - start), minTime));
    const int64_t s = std::max<int64_t>(1, std::llround(span));
    start = std::llround(at - fracX * double(s));
    end = start + s;
  }
  if (price) {
    const double maxPrice = m_viewState->maxPriceSpan(), minPrice = m_viewState->minPriceSpan();
    double span = (hi - lo) / factor;
    if (maxPrice > 0) span = std::min(span, maxPrice);
    if (minPrice > 0) span = std::max(span, std::min(hi - lo, minPrice));
    hi = ap + fracY * span;
    lo = hi - span;
  }
  const auto anchor = m_viewState->rasterAnchor();
  m_viewState->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  m_viewState->setViewport(start, end, lo, hi);
  if (price) emit m_viewState->priceInteracted();
  if (m_viewState->isAutoScrollEnabled()) enableAutoScroll(false);
  if (!m_zoomSettleTimer) {
    m_zoomSettleTimer = new QTimer(this);
    m_zoomSettleTimer->setSingleShot(true);
    connect(m_zoomSettleTimer, &QTimer::timeout, this, [this] { endZoomGesture(); });
  }
  m_zoomSettleTimer->start(settleMs);
  update();
}

void UnifiedGridRenderer::endZoomGesture() {
  if (m_zoomSettleTimer) m_zoomSettleTimer->stop();
  if (!m_zoomGesture) return;
  const auto from = rasterCameraNow(false); // the continuous camera, as drawn
  m_zoomGesture = false;
  sLog_Probe("zoom.gesture", "end");
  if (!from.valid || !m_viewState) {
    update();
    return;
  }
  // Settle on the nearest rung about the last gesture point.
  const double W = from.widthDev, H = from.heightDev, tf = double(m_currentTimeframe_ms);
  const double fracX = std::clamp(width() > 0 ? m_gestureAt.x() / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? m_gestureAt.y() / height() : 0.5, 0.0, 1.0);
  const double maxTime = m_viewState->maxTimeSpanMs(), minTime = m_viewState->minTimeSpanMs();
  const int minCol = maxTime > 0 ? std::max(1, int(std::ceil(W * tf / maxTime - 1e-9))) : 1;
  const int maxCol = std::min(chart_raster::maxColumnPixels(tf),
                              minTime > 0 ? std::max(1, int(std::floor(W * tf / minTime + 1e-9)))
                                          : chart_raster::kMaxCellPx);
  // An axis the gesture did not zoom keeps its drawn whole pixels.
  const int col = m_gestureTime ? chart_raster::nearestColumnRung(from.colPxF, minCol, maxCol) : from.colPx;
  const qint64 span = std::llround(W * tf / double(col));
  const double at = from.timeAtXDev(fracX * W), ap = from.priceAtYDev(fracY * H);
  const qint64 start = std::llround(at - fracX * double(span));
  const bool price = m_gesturePrice && !m_viewState->autoPriceScale() && m_gpuLayer && m_gpuLayer->tickPrice() > 0;
  chart_raster::RowRung row{from.tick, from.rowPx};
  if (price)
    row = chart_raster::nearestRowRung(from.pxPerPrice(), int(H), m_viewState->minPriceSpan(),
                                       m_viewState->maxPriceSpan(), tickPredictor(start, start + span, ap, fracY));
  setZoomRung(from, fracX, fracY, m_gestureTime, col, price && row.rowPx > 0, row);
  startZoomGlide(from, at, ap, col, price && row.rowPx > 0 ? row : chart_raster::RowRung{from.tick, from.rowPx});
}

void UnifiedGridRenderer::wheelZoom(int angle, bool notch, Qt::ScrollPhase phase, double x, double y, bool time,
                                    bool price) {
  if (notch && phase == Qt::NoScrollPhase) {
    m_wheelAngleRemainder += angle;
    const int clicks = std::clamp(m_wheelAngleRemainder / 120, -4, 4);
    m_wheelAngleRemainder -= clicks * 120;
    if (clicks) zoomClicks(clicks, x, y, time, price);
  } else if (phase == Qt::ScrollEnd) {
    endZoomGesture();
  } else if (angle != 0) {
    zoomContinuousFor(continuousFactor(angle), x, y, time, price, kGestureSettleMs);
  }
}

void UnifiedGridRenderer::zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x) {
  wheelZoom(angleDelta, pixelDelta == 0, Qt::ScrollPhase(phase), x, height() / 2, true, false);
}

void UnifiedGridRenderer::zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y) {
  if (m_viewState && angleDelta != 0) m_viewState->setAutoPriceScale(false);
  wheelZoom(angleDelta, pixelDelta == 0, Qt::ScrollPhase(phase), width() / 2, y, false, true);
}

void UnifiedGridRenderer::zoomTimeDrag(double delta, double x) {
  zoomContinuousFor(continuousFactor(delta), x, height() / 2, true, false, kGestureSafetyMs);
}

void UnifiedGridRenderer::zoomPriceDrag(double delta, double y) {
  if (m_viewState) m_viewState->setAutoPriceScale(false);
  zoomContinuousFor(continuousFactor(delta), width() / 2, y, false, true, kGestureSafetyMs);
}

void UnifiedGridRenderer::zoomTimeClicks(int clicks, double x) { zoomClicks(clicks, x, height() / 2, true, false); }

void UnifiedGridRenderer::zoomPriceClicks(int clicks, double y) {
  if (m_viewState) m_viewState->setAutoPriceScale(false);
  zoomClicks(clicks, width() / 2, y, false, true);
}

bool UnifiedGridRenderer::event(QEvent* e) {
  if (e->type() == QEvent::NativeGesture) {
    auto* gesture = static_cast<QNativeGestureEvent*>(e);
    switch (gesture->gestureType()) {
    case Qt::ZoomNativeGesture: {
      const QPointF at = gesture->position();
      const bool shift = gesture->modifiers() & Qt::ShiftModifier;
      if (shift && m_viewState) m_viewState->setAutoPriceScale(false);
      zoomContinuousFor(1.0 + gesture->value(), at.x(), at.y(), !shift, true, kGestureSafetyMs);
      gesture->accept();
      return true;
    }
    case Qt::EndNativeGesture:
      endZoomGesture();
      gesture->accept();
      return true;
    case Qt::BeginNativeGesture:
      gesture->accept();
      return true;
    default:
      break;
    }
  }
  return QQuickItem::event(e);
}
