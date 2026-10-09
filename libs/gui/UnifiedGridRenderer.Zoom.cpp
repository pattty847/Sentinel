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
// The continuous zoom of one event (an axis drag step, a trackpad scroll delta in
// angle units), as the wheel always scaled it: 1 + clamp(delta * 0.0005, +-0.4)
// (> 1 zooms in).
constexpr double kContinuousZoomPerUnit = 0.0005, kContinuousZoomMaxStep = 0.4;
double continuousFactor(double delta) {
  return 1.0 + std::clamp(delta * kContinuousZoomPerUnit, -kContinuousZoomMaxStep, kContinuousZoomMaxStep);
}
} // namespace

qint64 UnifiedGridRenderer::zoomNowMs() const {
  return m_zoomClock ? m_zoomClock() : (m_frameClock.isValid() ? m_frameClock.elapsed() : 0);
}

namespace {
// What a glide draws on its axis this frame (without a drag): its end while holding.
chart_raster::RasterCamera glidePart(const chart_raster::RasterCamera& from, const chart_raster::RasterCamera& end,
                                     double anchorTime, double anchorPrice, double progress, bool holding) {
  return holding ? end : chart_raster::glideRaster(from, end, anchorTime, anchorPrice, progress);
}
} // namespace

UnifiedGridRenderer::Cameras UnifiedGridRenderer::camerasFor(const chart_raster::RasterInputs& in,
                                                             chart_raster::RasterStep previous) const {
  Cameras c;
  c.rest = chart_raster::computeRaster(in, previous);
  c.drawn = c.rest;
  if (!c.rest.valid) return c;
  const bool gestureTime = m_zoomGesture && m_gestureTime, gesturePrice = m_zoomGesture && m_gesturePrice;
  const bool timeMoves = gestureTime || m_glideTime.active, priceMoves = gesturePrice || m_glidePrice.active;
  if (!timeMoves && !priceMoves) return c;
  // Each axis on its own: a gesture's continuous camera, a glide, or the rest camera.
  // A drag moves a glide's ends by the same whole device pixels as the picture (1:1).
  const double dpr = c.rest.dpr;
  const double dx = std::isfinite(in.dragLogicalPx.x()) ? double(std::llround(in.dragLogicalPx.x() * dpr)) : 0.0;
  const double dy = std::isfinite(in.dragLogicalPx.y()) ? double(std::llround(in.dragLogicalPx.y() * dpr)) : 0.0;
  const chart_raster::RasterCamera continuous =
      gestureTime || gesturePrice ? chart_raster::continuousRaster(in) : chart_raster::RasterCamera{};
  double timeLo = c.rest.drawnStartMs, timeHi = c.rest.drawnEndMs;
  double priceLo = c.rest.drawnMinPrice, priceHi = c.rest.drawnMaxPrice;
  auto axis = [&](const AxisGlide& g, bool time, bool gesture) {
    if (gesture) return continuous;
    if (!g.active) return c.rest;
    const auto from = chart_raster::shiftedRaster(g.from, dx, dy), end = chart_raster::shiftedRaster(g.end, dx, dy);
    // Bin the glide's whole extent (its start and its end) once, at its start.
    if (time) {
      timeLo = std::min({timeLo, from.drawnStartMs, end.drawnStartMs});
      timeHi = std::max({timeHi, from.drawnEndMs, end.drawnEndMs});
    } else {
      priceLo = std::min({priceLo, from.drawnMinPrice, end.drawnMinPrice});
      priceHi = std::max({priceHi, from.drawnMaxPrice, end.drawnMaxPrice});
    }
    return glidePart(from, end, g.anchorTime, g.anchorPrice, g.progress, g.holding);
  };
  const auto timeCam = axis(m_glideTime, true, gestureTime);
  const auto priceCam = axis(m_glidePrice, false, gesturePrice);
  c.drawn = chart_raster::composeAxes(timeCam, timeMoves, priceCam, priceMoves, c.rest);
  if (c.drawn.free) {
    c.hasBinView = true;
    c.binTimeLo = std::min(timeLo, c.drawn.drawnStartMs);
    c.binTimeHi = std::max(timeHi, c.drawn.drawnEndMs);
    c.binPriceLo = std::min(priceLo, c.drawn.drawnMinPrice);
    c.binPriceHi = std::max(priceHi, c.drawn.drawnMaxPrice);
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

chart_raster::RasterCamera UnifiedGridRenderer::intendedEndNow() const {
  if (!m_viewState || !m_gpuLayer) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(m_viewState.get());
  viewport.dragging = false;
  // The tick the frame chooses for the stored view (the chooser's own rule and state),
  // before it is committed.
  double tick = m_gpuLayer->tickPrice();
  if (tick > 0 && viewport.valid) {
    const int64_t units = m_gpuLayer->predictTickUnits(
        {double(viewport.timeStart), double(viewport.timeEnd), viewport.minPrice, viewport.maxPrice});
    if (units > 0) tick = heatmap::fromUnits(units, m_gpuLayer->priceScale());
  }
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  const auto in = rasterInputs(viewport, tick, width(), height(), dpr);
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

void UnifiedGridRenderer::endZoomGlide(bool time, bool price) {
  bool ended = false;
  if (time && m_glideTime.active) ended = true, m_glideTime.active = false;
  if (price && m_glidePrice.active) ended = true, m_glidePrice.active = false;
  if (ended) update();
}

void UnifiedGridRenderer::startAxisGlides(const chart_raster::RasterCamera& drawn,
                                          const chart_raster::RasterCamera& restBefore, double anchorTime,
                                          double anchorPrice, bool forceTime, bool forcePrice) {
  const auto end = intendedEndNow();
  if (!end.valid || !drawn.valid) return;
  const qint64 now = zoomNowMs();
  bool started = false;
  auto start = [&](AxisGlide& g, bool time, bool force) {
    // The end this axis had: its glide's, or the rest camera it was drawn at.
    const auto& before = g.active ? g.end : restBefore;
    const bool same = time ? chart_raster::sameTimeAxis(end, before) : chart_raster::samePriceAxis(end, before);
    if (same && !force) return; // unchanged: a running glide keeps going, a rest axis stays
    g.active = true;
    g.holding = false;
    g.from = drawn;
    g.end = end;
    g.anchorTime = anchorTime;
    g.anchorPrice = anchorPrice;
    g.beganMs = g.startMs = now;
    g.deadlineMs = now + chart_raster::kZoomGlideMs;
    g.progress = 0.0;
    started = true;
  };
  start(m_glideTime, true, forceTime);
  start(m_glidePrice, false, forcePrice);
  if (m_glideTime.active) m_glideColPx = m_glideTime.end.colPx;
  if (m_glidePrice.active) m_glideRow = {m_glidePrice.end.tick, m_glidePrice.end.rowPx};
  if (!started) return;
  sLog_Probe("zoom.glide", "start time=" << m_glideTime.active << " colPx=" << drawn.colPxF << "->" << end.colPx
             << " price=" << m_glidePrice.active << " rows=" << drawn.pxPerPrice() << "->" << end.rowPx << "@"
             << end.tick);
  emit rasterChanged();
  emit viewportChanged();
  polish(); // the next frame's updatePolish advances it
  update();
}

void UnifiedGridRenderer::updatePolish() {
  QQuickItem::updatePolish();
  advanceZoomGlide();
  if (!zoomGliding()) return;
  if (!m_glidePolishTimer) {
    m_glidePolishTimer = new QTimer(this);
    m_glidePolishTimer->setSingleShot(true);
    m_glidePolishTimer->setInterval(0);
    connect(m_glidePolishTimer, &QTimer::timeout, this, [this] {
      if (zoomGliding()) {
        polish();
        update();
      }
    });
  }
  m_glidePolishTimer->start();
}

void UnifiedGridRenderer::advanceAxisGlide(AxisGlide& g, bool time, const chart_raster::RasterCamera& rest,
                                           const chart_raster::RasterCamera& intended, qint64 now) {
  if (!g.active) return;
  const qint64 cap = g.beganMs + 2 * chart_raster::kZoomGlideMs;
  // The end is fixed at the click. A change of it that the user did not ask for (a
  // refit, another device pixel ratio or size, a tick that differs from the
  // predicted one) continues from what the last frame drew: no jump, no reverse
  // movement. A drag, a pan commit and a follow-live step move both ends instead.
  const bool same = time ? chart_raster::sameTimeAxis(intended, g.end) : chart_raster::samePriceAxis(intended, g.end);
  if (intended.valid && !same) {
    g.from = glidePart(g.from, g.end, g.anchorTime, g.anchorPrice, g.progress, g.holding);
    g.end = intended;
    g.holding = false;
    g.deadlineMs = std::min(cap, std::max(g.deadlineMs, now + chart_raster::kZoomGlideMs / 2));
    g.startMs = now;
    g.progress = 0.0;
    ++m_glideRebases;
    if (time) m_glideColPx = g.end.colPx;
    else m_glideRow = {g.end.tick, g.end.rowPx};
    sLog_Probe("zoom.glide", "rebased " << (time ? "time" : "price") << " to colPx=" << g.end.colPx << " rowPx="
               << g.end.rowPx << "@" << g.end.tick << " dpr=" << g.end.dpr << " leftMs=" << g.deadlineMs - now);
  }
  // At the cap the frame lands on the current rest camera (as at rest).
  if (now >= cap) {
    g.active = false;
    g.progress = 1.0;
    sLog_Probe("zoom.glide", "landed at the cap " << (time ? "time" : "price"));
    return;
  }
  const double t = double(now - g.startMs) / double(std::max<qint64>(1, g.deadlineMs - g.startMs));
  if (t < 1.0) {
    g.progress = chart_raster::easeZoom(t);
    return;
  }
  g.progress = 1.0;
  // The end is reached. It lands when the frame's rest camera is the end (the landing
  // frame is the rest frame); until then (the end's tick not committed yet) it holds
  // the end, drawn with coverage.
  const bool landed = time ? chart_raster::sameTimeAxis(rest, g.end) : chart_raster::samePriceAxis(rest, g.end);
  g.holding = !landed;
  if (landed) {
    g.active = false;
    sLog_Probe("zoom.glide", "landed " << (time ? "time" : "price"));
  }
}

void UnifiedGridRenderer::advanceZoomGlide() {
  if (!zoomGliding()) return;
  const qint64 now = zoomNowMs();
  const auto rest = restCameraNow(false), intended = intendedEndNow();
  advanceAxisGlide(m_glideTime, true, rest, intended, now);
  advanceAxisGlide(m_glidePrice, false, rest, intended, now);
  // This frame's camera for every layer: the axis models recalculate, the sibling
  // overlays (candles, paper, algo) redraw in this synchronization.
  emit rasterChanged();
  emit viewportChanged();
  update();
}

bool UnifiedGridRenderer::setZoomRung(const chart_raster::RasterCamera& drawn, double fracX, double fracY,
                                      bool time, int colPx, bool price, chart_raster::RowRung row,
                                      int autoDirection, double previousPriceSpan) {
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
  // Resolve the auto fit at the tick Auto will draw BEFORE publishing the time
  // rung. No camera/viewport changes occur inside the bounded solve. Manual tick
  // and the auto-scale-off path keep their existing fitting/zoom behavior.
  std::optional<SolvedZoomFit> solvedFit;
  if (time && m_viewState->autoPriceScale() && !m_gpuLayer->manualMode() && m_gpuLayer->tickPrice() > 0) {
    if (const auto raw = gpuFitPriceWindow(start, end, true, fracX, false)) {
      const auto fit = chart_raster::solveAutoPriceFit(raw->first, raw->second, drawn.heightDev,
          m_gpuLayer->tickPrice(), autoDirection, previousPriceSpan, [this, start, end](double low, double high) {
            return heatmap::fromUnits(m_gpuLayer->predictTickUnits({double(start), double(end), low, high}),
                                       m_gpuLayer->priceScale());
          });
      if (!fit) {
        sLog_Warning("zoom auto fit has no fixed point within the solve bound; keeping the viewport time=["
                     << start << ".." << end << "] price=[" << raw->first << ".." << raw->second << "]");
        return false; // never publish a partial solution or start a bouncing glide
      }
      lo = fit->lo;
      hi = fit->hi;
      solvedFit = SolvedZoomFit{start, end, m_currentTimeframe_ms, m_viewState->placementVersion() + 1,
                                drawn.widthDev, drawn.heightDev, *raw, *fit};
      m_gpuPriceKnown = true;
      m_gpuReseedPrice = false;
      m_priceCarry.reset();
      sLog_Probe("zoom.fit", "time=[" << start << ".." << end << "] price=[" << lo << ".." << hi
                 << "] tick=" << fit->tick << " P=" << fit->rowPx << " iterations=" << fit->iterations
                 << " cycle=" << fit->cycleBroken);
    }
  }
  const auto anchor = m_viewState->rasterAnchor();
  m_viewState->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  m_solvedZoomFit = std::move(solvedFit);
  m_viewState->setViewport(start, end, lo, hi); // one change; the auto price fit applies inside
  return true;
}

void UnifiedGridRenderer::zoomStoredView(double factor, double x, double y, bool time, bool price) {
  const double fracX = std::clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0);
  qint64 start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  double lo = m_viewState->getMinPrice(), hi = m_viewState->getMaxPrice();
  if (time) {
    double span = double(end - start) / factor;
    if (m_viewState->maxTimeSpanMs() > 0) span = std::min(span, m_viewState->maxTimeSpanMs());
    if (m_viewState->minTimeSpanMs() > 0) span = std::max(span, std::min(double(end - start), m_viewState->minTimeSpanMs()));
    const double at = double(start) + fracX * double(end - start);
    const int64_t s = std::max<int64_t>(1, std::llround(span));
    start = std::llround(at - fracX * double(s));
    end = start + s;
  }
  if (price && hi > lo) {
    double span = (hi - lo) / factor;
    if (m_viewState->maxPriceSpan() > 0) span = std::min(span, m_viewState->maxPriceSpan());
    if (m_viewState->minPriceSpan() > 0) span = std::max(span, std::min(hi - lo, m_viewState->minPriceSpan()));
    const double ap = hi - fracY * (hi - lo);
    hi = ap + fracY * span;
    lo = hi - span;
  }
  sLog_Probe("zoom.click", "no camera: the stored view x" << factor << " time=" << time << " price=" << price);
  if (start == m_viewState->getVisibleTimeStart() && end == m_viewState->getVisibleTimeEnd() &&
      lo == m_viewState->getMinPrice() && hi == m_viewState->getMaxPrice())
    return; // at the limits
  const auto anchor = m_viewState->rasterAnchor();
  m_viewState->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  m_viewState->setViewport(start, end, lo, hi);
  if (price) emit m_viewState->priceInteracted();
  if (m_viewState->isAutoScrollEnabled()) enableAutoScroll(false);
  update();
}

void UnifiedGridRenderer::zoomClicks(int clicks, double x, double y, bool time, bool price) {
  if (!m_viewState || !m_gpuLayer || !m_viewState->isTimeWindowValid() || clicks == 0 ||
      m_viewState->isDragging())
    return;
  if (m_zoomGesture) endZoomGesture();
  // The auto price scale owns price: the chart wheel zooms time only.
  price = price && !m_viewState->autoPriceScale();
  const auto drawn = rasterCameraNow(false); // what is on screen now (a glide in flight included)
  if (!time && !price) return;
  if (!drawn.valid) {
    // Nothing drawn yet (no tick or size): the stored view by the ratio, no glide.
    zoomStoredView(std::pow(chart_raster::kZoomStepRatio, double(clicks)), x, y, time, price);
    return;
  }
  const double W = drawn.widthDev, H = drawn.heightDev, tf = double(m_currentTimeframe_ms);
  const double fracX = std::clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0);
  // Rapid clicks step on from the glide's target, so they accumulate monotonically.
  const int baseCol = m_glideTime.active ? m_glideColPx : drawn.colPx;
  const chart_raster::RowRung baseRow =
      m_glidePrice.active ? m_glideRow : chart_raster::RowRung{drawn.tick, drawn.rowPx};
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
  // Auto's tick for a price rung is predicted over exactly the time bounds this click
  // commits (the frame chooses the tick over the stored view): the target's when time
  // zooms, else the stored bounds unchanged (not the drawn columns).
  const bool timeMoves = time && col != baseCol;
  const qint64 tickStart = timeMoves ? targetStart : m_viewState->getVisibleTimeStart();
  const qint64 tickEnd = timeMoves ? targetStart + targetSpan : m_viewState->getVisibleTimeEnd();
  chart_raster::RowRung row = baseRow;
  if (price && m_gpuLayer->tickPrice() > 0)
    row = chart_raster::rowRung(baseRow, clicks, int(H), m_viewState->minPriceSpan(), m_viewState->maxPriceSpan(),
                                tickPredictor(tickStart, tickEnd, ap, fracY));
  if (price && row == baseRow) {
    // No rows to whole-pixel (no tick drawn yet) or no rung (rows at the cell cap, e.g.
    // a carried window before the new symbol's price): the price span by the ratio, in
    // the limit of the click's direction (encoded as row {-span, 0}).
    const double current = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
    double span = current / std::pow(chart_raster::kZoomStepRatio, double(clicks));
    if (clicks < 0 && m_viewState->maxPriceSpan() > 0) span = std::min(span, std::max(current, m_viewState->maxPriceSpan()));
    if (clicks > 0 && m_viewState->minPriceSpan() > 0) span = std::max(span, std::min(current, m_viewState->minPriceSpan()));
    if (span != current && std::isfinite(span) && span > 0) row = {-span, 0};
  }
  const bool zoomTime = timeMoves, zoomPrice = price && !(row == baseRow);
  if (!zoomTime && !zoomPrice) {
    sLog_Probe("zoom.click", "at the limits clicks=" << clicks << " time=" << time << " price=" << price << " col="
               << baseCol << " row=" << baseRow.rowPx << "@" << baseRow.tick << " span="
               << m_viewState->getMaxPrice() - m_viewState->getMinPrice() << " limits=" << m_viewState->minPriceSpan()
               << ".." << m_viewState->maxPriceSpan());
    return;
  }
  sLog_Probe("zoom.click", "clicks=" << clicks << " col=" << baseCol << "->" << col << " row=" << baseRow.rowPx
             << "@" << baseRow.tick << "->" << row.rowPx << "@" << row.tick);
  const auto restBefore = restCameraNow(false);
  const auto& priceBase = m_glidePrice.active ? m_glidePrice.end : drawn;
  if (!setZoomRung(drawn, fracX, fracY, zoomTime, col, zoomPrice, row, clicks > 0 ? 1 : -1,
                    priceBase.drawnMaxPrice - priceBase.drawnMinPrice)) return;
  if (zoomPrice) emit m_viewState->priceInteracted();
  if (m_viewState->isAutoScrollEnabled()) enableAutoScroll(false);
  // A new glide from what is drawn on every axis whose end the click changed (the auto
  // price fit of the new time window included); the other axis is left as it is.
  startAxisGlides(drawn, restBefore, at, ap, false, false);
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
  // The camera on screen: before the gesture what is drawn (a rest view or a glide),
  // later the continuous camera on the axes the gesture zooms. An axis joins the
  // gesture from its drawn window, so its continuous camera starts exactly on the
  // picture (no jump), in one viewport change; an axis the gesture does not zoom keeps
  // its stored bounds and its whole pixels.
  const bool starting = !m_zoomGesture;
  const auto cam = rasterCameraNow(false);
  if (!cam.valid) {
    if (!m_zoomGesture) zoomStoredView(factor, x, y, time, price); // nothing drawn yet
    return;
  }
  if (starting) {
    m_zoomGesture = true;
    m_gestureTime = m_gesturePrice = false;
    m_gestureBaseTimeSpan = cam.drawnEndMs - cam.drawnStartMs;
    m_gestureBasePriceSpan = cam.drawnMaxPrice - cam.drawnMinPrice;
    sLog_Probe("zoom.gesture", "begin");
  }
  const bool timeJoins = time && !m_gestureTime, priceJoins = price && !m_gesturePrice;
  // An axis joining the gesture leaves its glide where it is drawn; a glide on the other
  // axis keeps running.
  endZoomGlide(timeJoins, priceJoins);
  m_gestureTime = m_gestureTime || time;
  m_gesturePrice = m_gesturePrice || price;
  m_gestureAt = QPointF(x, y);
  const double W = cam.widthDev, H = cam.heightDev;
  const double at = cam.timeAtXDev(fracX * W), ap = cam.priceAtYDev(fracY * H);
  qint64 start = timeJoins ? std::llround(cam.drawnStartMs) : m_viewState->getVisibleTimeStart();
  qint64 end = timeJoins ? std::llround(cam.drawnEndMs) : m_viewState->getVisibleTimeEnd();
  double lo = priceJoins ? cam.drawnMinPrice : m_viewState->getMinPrice();
  double hi = priceJoins ? cam.drawnMaxPrice : m_viewState->getMaxPrice();
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
  const auto from = rasterCameraNow(false); // the continuous camera on the gesture's axes, as drawn
  const auto restBefore = restCameraNow(false);
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
  // The tick predicted over the time bounds committed below (the stored ones when the
  // gesture did not zoom time).
  const qint64 tickStart = m_gestureTime ? start : m_viewState->getVisibleTimeStart();
  const qint64 tickEnd = m_gestureTime ? start + span : m_viewState->getVisibleTimeEnd();
  chart_raster::RowRung row{from.tick, from.rowPx};
  if (price)
    row = chart_raster::nearestRowRung(from.pxPerPrice(), int(H), m_viewState->minPriceSpan(),
                                       m_viewState->maxPriceSpan(), tickPredictor(tickStart, tickEnd, ap, fracY));
  if (!setZoomRung(from, fracX, fracY, m_gestureTime, col, price && row.rowPx > 0, row,
                    double(span) < m_gestureBaseTimeSpan ? 1 : -1, m_gestureBasePriceSpan)) {
    update();
    return;
  }
  // The gesture's axes glide from their continuous camera to the rung; the other axis
  // is left as it is.
  startAxisGlides(from, restBefore, at, ap, m_gestureTime, m_gesturePrice);
}

void UnifiedGridRenderer::wheelZoom(int angle, bool notch, Qt::ScrollPhase phase, double x, double y, bool time,
                                    bool price, WheelRoute route) {
  if (notch && phase == Qt::NoScrollPhase) {
    // A partial notch carries to the next event of the same route and axes, in the same
    // direction; at most four clicks per event, and only the partial notch is kept.
    const int key = int(route) * 4 + (time ? 2 : 0) + (price ? 1 : 0);
    if (key != m_wheelCarryKey || (m_wheelAngleRemainder > 0 && angle < 0) || (m_wheelAngleRemainder < 0 && angle > 0))
      m_wheelAngleRemainder = 0;
    m_wheelCarryKey = key;
    m_wheelAngleRemainder += angle;
    const int whole = m_wheelAngleRemainder / 120;
    m_wheelAngleRemainder -= whole * 120;
    const int clicks = std::clamp(whole, -4, 4);
    if (clicks) zoomClicks(clicks, x, y, time, price);
  } else if (phase == Qt::ScrollEnd) {
    endZoomGesture();
  } else if (angle != 0) {
    zoomContinuousFor(continuousFactor(angle), x, y, time, price, kGestureSettleMs);
  }
}

void UnifiedGridRenderer::zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x) {
  wheelZoom(angleDelta, pixelDelta == 0, Qt::ScrollPhase(phase), x, height() / 2, true, false, WheelRoute::TimeAxis);
}

void UnifiedGridRenderer::zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y) {
  if (m_viewState && angleDelta != 0) m_viewState->setAutoPriceScale(false);
  wheelZoom(angleDelta, pixelDelta == 0, Qt::ScrollPhase(phase), width() / 2, y, false, true, WheelRoute::PriceAxis);
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
