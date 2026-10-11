// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "PerformanceMonitor.hpp"
#include "SentinelLogging.hpp"
#include "config/GuiConfigStore.hpp"
#include "datasources/CandleSeriesBuffer.hpp"
#include "render/DataProcessor.hpp"
#include "render/GridViewState.hpp"
#include "render/FrameContextBuilder.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "chart/shell/HeatmapChartPort.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "../core/servermodel/SessionManager.hpp"
#include <QDateTime>
#include <QImage>
#include <tuple>
#include <QMetaObject>
#include <algorithm>
#include <cmath>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>


namespace {
bool envFloatValue(const char *name, float &out) {
  const QByteArray value = qgetenv(name);
  if (value.isEmpty()) {
    return false;
  }
  bool ok = false;
  const float parsed = value.toFloat(&ok);
  if (!ok) {
    return false;
  }
  out = parsed;
  return true;
}

bool envIntValue(const char *name, int &out) {
  const QByteArray value = qgetenv(name);
  if (value.isEmpty()) {
    return false;
  }
  bool ok = false;
  const int parsed = value.toInt(&ok);
  if (!ok) {
    return false;
  }
  out = parsed;
  return true;
}
} // namespace

chart::SurfaceFacts chart::HeatmapChartPort::surface() const {
  if (!layer_) return {};
  return {item_.width(), item_.height(), item_.window() ? item_.window()->effectiveDevicePixelRatio() : 1.0};
}

std::optional<heatmap::gpu::ViewWindow> chart::HeatmapChartPort::restView() const {
  if (!layer_) return std::nullopt;
  const auto cam = item_.restCameraNow(false);
  if (!cam.valid) return std::nullopt;
  return heatmap::gpu::ViewWindow{cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice, cam.drawnMaxPrice};
}

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem *parent)
    : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setFlag(ItemAcceptsInputMethod, true);

  setAcceptHoverEvents(false); // Reduce event capture
  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* w) { bindWindow(w); });

  init();
  bindZoomGlide();
  connect(getViewState(), &GridViewState::autoPriceScaleChanged, this, [this] {
    if (!getViewState()->autoPriceScale() && m_gpuLayer->setAutoPriceTick(std::nullopt)) update();
  });
  // A new resolution summary can change which candidate fits are buildable.
  // Refit on the GUI thread, never from the frame's tick chooser.
  connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, this,
          &UnifiedGridRenderer::refitAutoPrice);
  connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, this,
          &UnifiedGridRenderer::refitAutoPrice);
  // An item constructed with a parent already in a window (C++ hosts, tests)
  // saw windowChanged before the connection above.
  if (window()) bindWindow(window());
}

void UnifiedGridRenderer::bindWindow(QQuickWindow* w) {
  if (m_axisTextService) {
    m_axisTextService->bindAxisLayoutWindow(w);
  }
  syncGpuSurface(); // the device pixel ratio sets the 1 column/px clamp
  if (m_screenChangedConn) disconnect(m_screenChangedConn);
  if (m_beforeSyncConn) disconnect(m_beforeSyncConn);
  if (!w) return;
  // The frame's raster camera is computed and published before any item syncs
  // (render thread, GUI blocked; prepareSyncFrame).
  m_beforeSyncConn = connect(w, &QQuickWindow::beforeSynchronizing, this, [this] { prepareSyncFrame(); },
                             Qt::DirectConnection);
  // A move to a screen of another device pixel ratio: the clamps follow it, and the
  // controller's drawn window (the raster camera re-rounds in the next frame).
  m_screenChangedConn = connect(w, &QQuickWindow::screenChanged, this, [this] {
    syncGpuSurface();
    m_view->syncGpuView();
    update();
  });
  // The GPU tile node's stats outlive this connection (shared).
  auto tileStats = m_gpuLayer ? m_gpuLayer->tileStatsPtr() : nullptr;
  connect(w, &QQuickWindow::afterRendering, this, [this, tileStats]() {
    // Direct render-thread callback: fixed-size frame snapshot only.
    m_renderedFrameId.store(m_pendingFrameId, std::memory_order_release);
    m_renderedRevision.store(m_pendingFrameRevision, std::memory_order_release);
    // GPU heatmap work left after this frame (budgeted uploads, a crossfade,
    // live paging): ask for the next frame; an idle node stops asking.
    if (tileStats && tileStats->wantsFrame.exchange(false))
      QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    // Labels held back by a transition (a crossfade's last frame, a picture not
    // yet matched): once this frame's prepare() has moved the node on, redraw
    // only if the labels would now draw differently after the transition.
    if (m_gpuLabelsIncomplete.exchange(false))
      QMetaObject::invokeMethod(this, [this] {
        if (m_gpuLayer && m_gpuLayer->labelSignature() != m_gpuLabelSignature.load()) update();
      }, Qt::QueuedConnection);
  }, Qt::DirectConnection);
}


UnifiedGridRenderer::~UnifiedGridRenderer() {
  for (auto* timer : {m_glidePolishTimer, m_zoomSettleTimer, m_carryWaitTimer, m_gpuBootstrapTimer})
    if (timer) {
      timer->stop();
      disconnect(timer, nullptr, this, nullptr);
    }
  if (m_view) {
    disconnect(&m_view->zoomGlide(), nullptr, this, nullptr);
    disconnect(getViewState(), nullptr, this, nullptr);
    getViewState()->setPanShift({});
  }
  // ~QQuickItem leaves the window (windowChanged) after this class's members are gone:
  // unbind the window now, while they exist.
  disconnect(this, &QQuickItem::windowChanged, this, nullptr);
  if (m_screenChangedConn) disconnect(m_screenChangedConn);
  if (m_beforeSyncConn) disconnect(m_beforeSyncConn);
  if (m_heatmapPort) m_heatmapPort->detach();
  m_gpuLayer.reset(); // destroys its controller on the heatmap-data thread
  if (m_dataProcessor) {
    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
      QMetaObject::invokeMethod(m_dataProcessor.get(),
                                &DataProcessor::stopProcessing,
                                Qt::BlockingQueuedConnection);
    } else {
      m_dataProcessor->stopProcessing();
    }
    disconnect(m_dataProcessor.get(), nullptr, this, nullptr);
  }

  if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
    m_dataProcessorThread->quit();
    if (!m_dataProcessorThread->wait(5000)) {
      m_dataProcessorThread->terminate();
      m_dataProcessorThread->wait(1000);
    }
  }

  m_dataProcessor.reset();
  m_dataProcessorThread.reset();
}

void UnifiedGridRenderer::onTradeReceived(const Trade &trade) {
  if (m_view->session().symbol == QLatin1String(trade.product_id.data(), qsizetype(trade.product_id.size()))) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    if (m_tradeBubbleTape->append({ms, trade.price, trade.size, trade.side, trade.trade_id}) && m_showTrades)
      update(); // Qt coalesces trade bursts into one scene synchronization
  }
  // The live price for auto-fit compares the ASCII product id without conversion.
  // The separate seed comparison below converts to QString and may allocate.
  if (std::isfinite(trade.price) && trade.price > 0 &&
      m_view->session().symbol == QLatin1String(trade.product_id.data(), static_cast<qsizetype>(trade.product_id.size())))
    m_view->session().lastTrade = trade.price;
  // A trade seeds the price window only when no book top has (yet) for this symbol.
  if ((m_view->flags().reseedPrice || !m_view->flags().priceKnown) && QString::fromStdString(trade.product_id) == m_view->session().symbol)
    seedGpuViewport(trade.price, trade.price);
}

void UnifiedGridRenderer::setLiveBookTop(double bestBid, double bestAsk) {
  // The book top seeds the first viewport (and the price window after a symbol switch).
  if (std::isfinite(bestBid) && std::isfinite(bestAsk) && bestBid > 0 && bestAsk >= bestBid)
    m_view->session().bookMid = (bestBid + bestAsk) * 0.5;
  seedGpuViewport(bestBid, bestAsk);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs,
                                        double minPrice, double maxPrice) {
  if (getViewState()) {
    getViewState()->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
  }

  update();

  sLog_Probe("viewport.view",
             "t=[" << startTimeMs << ".." << endTimeMs << "]"
             << " price=[" << minPrice << ".." << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
  if (!getViewState()) return;
  update();
  if (!m_view->flags().selfViewport) m_view->flags().pristine = false; // someone else moved the view
  // The controller plans from the committed view.
  m_view->syncGpuView();
}

void UnifiedGridRenderer::setPriceAxisSource(QObject *source) {
  if (m_axisTextService) {
    m_axisTextService->setPriceAxisSource(source);
  }
}

void UnifiedGridRenderer::setTimeAxisSource(QObject *source) {
  if (m_axisTextService) {
    m_axisTextService->setTimeAxisSource(source);
  }
}


void UnifiedGridRenderer::geometryChange(const QRectF &newGeometry,
                                         const QRectF &oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);

  if (newGeometry.size() != oldGeometry.size()) {
    sLog_Probe("viewport.resize",
               "old=" << oldGeometry.width() << "x" << oldGeometry.height()
               << " new=" << newGeometry.width() << "x" << newGeometry.height());

    if (getViewState()) {
      getViewState()->setViewportSize(newGeometry.width(), newGeometry.height());
    }
    if (m_axisTextService) {
      m_axisTextService->refreshAxisLayout();
    }
    // Whole-pixel mapping: layers align item-relative; absolute alignment also needs
    // the item origin on a device pixel.
    if (window()) {
      const double dpr = window()->effectiveDevicePixelRatio();
      const QPointF origin = mapToScene(QPointF(0, 0)) * dpr;
      sLog_Probe("raster.origin", "x=" << origin.x() << " y=" << origin.y() << " dpr=" << dpr
                 << " size=" << newGeometry.width() << "x" << newGeometry.height());
    }
    syncGpuSurface();
    if (m_view->flags().pristine && getViewState() && getViewState()->isTimeWindowValid() &&
        newGeometry.width() > 0) {
      // Untouched seeded view: keep initial_column_px on the laid-out width.
      const qint64 end = getViewState()->getVisibleTimeEnd();
      m_view->setGpuViewportSelf(end - gpuInitialSpanMs(newGeometry.width()), end, getViewState()->getMinPrice(),
                         getViewState()->getMaxPrice());
    }
    update();
  }
}

void UnifiedGridRenderer::componentComplete() {
  QQuickItem::componentComplete();

  if (m_axisTextService) {
    m_axisTextService->bindAxisLayoutWindow(window());
  }
  if (getViewState() && width() > 0 && height() > 0) {
    getViewState()->setViewportSize(width(), height());
  }
  if (m_axisTextService) {
    m_axisTextService->refreshAxisLayout();
  }
}

void UnifiedGridRenderer::setShowGpuStatsOverlay(bool show) {
  if (m_showGpuStatsOverlay != show) {
    m_showGpuStatsOverlay = show;
    emit showGpuStatsOverlayChanged();
  }
}

void UnifiedGridRenderer::setShowDataPipelineOverlay(bool show) {
  if (m_showDataPipelineOverlay != show) {
    m_showDataPipelineOverlay = show;
    emit showDataPipelineOverlayChanged();
  }
}

void UnifiedGridRenderer::setShowViewportMathOverlay(bool show) {
  if (m_showViewportMathOverlay != show) {
    m_showViewportMathOverlay = show;
    emit showViewportMathOverlayChanged();
  }
}

void UnifiedGridRenderer::clearData() {
  m_tradeBubbleTape->clear();
  if (getViewState()) {
    getViewState()->resetZoom();
  }
  if (m_dataProcessor) {
    QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::clearData,
                              Qt::QueuedConnection);
  }
  m_footprintOverlay.clearPending();
  m_vpRenderer.clearPending();
  m_footprintOverlay.requestNeutralReset();
  update();
}

void UnifiedGridRenderer::setActiveSymbol(const QString& symbol) {
  const QString normalized = symbol.trimmed().toUpper();
  if (m_view->session().symbol == normalized) {
    return;
  }
  sLog_Render("active symbol changed, clearing chart data: prev=" << m_view->session().symbol
              << " symbol=" << normalized);
  m_view->zoomGlide().endZoomGlide();
  m_view->glide().gesture = false;
  // Auto price scale off: carry the zoom as a fraction of the price, with the current
  // price at the same height, onto the new symbol's first price (seedGpuViewport).
  // A symbol that never got its own price (m_view->flags().reseedPrice) still shows an earlier
  // symbol's bounds: its pending carry is kept, not recomputed against them.
  if (!getViewState() || getViewState()->autoPriceScale() || !m_view->flags().reseedPrice) m_priceCarry.reset();
  if (m_view->flags().priceKnown && !m_view->flags().reseedPrice && getViewState() && getViewState()->isTimeWindowValid() &&
      !getViewState()->autoPriceScale()) {
    double now = m_view->gpuLivePrice();
    if (!(now > 0) && m_candleBuffer) {
      // No book, trade or decoded price: the newest visible candle's close.
      const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
      m_candleBuffer->getVisibleSlice(m_view->session().symbol, std::max<int64_t>(1, (tf + 500) / 1000),
                                      recording::floorDiv(getViewState()->getVisibleTimeStart(), tf) * tf,
                                      getViewState()->getVisibleTimeEnd(), m_fitBars);
      for (auto it = m_fitBars.rbegin(); it != m_fitBars.rend() && !(now > 0); ++it)
        if (std::isfinite(it->close) && it->close > 0) now = it->close;
    }
    const double lo = getViewState()->getMinPrice(), span = getViewState()->getMaxPrice() - lo;
    const PriceCarry carry{span / now, (now - lo) / span};
    if (now > 0 && span > 0 && std::isfinite(carry.spanRatio) && carry.spanRatio > 0 && std::isfinite(carry.heightFrac))
      m_priceCarry = carry;
  }
  m_view->session().symbol = normalized;
  m_view->session().bookMid = m_view->session().lastTrade = 0.0;
  // The controller serial switches the heatmap (the node holds the old picture
  // until the new spans are ready); clearData() resets the trade overlays. The
  // next book top centres the new price.
  if (m_carryWaitTimer) m_carryWaitTimer->stop(); // the live-only wait is per symbol
  // Reseed first: the layer's limitsChanged (setSymbol) must not apply a pending carry
  // before the new symbol has a price of its own.
  m_view->flags().reseedPrice = true;
  if (m_gpuLayer) m_gpuLayer->setSymbol(normalized.toStdString());
  clearData();
  refitAutoPrice(); // auto price scale on: the new symbol's candles when already held
  if (m_dataProcessor) {
    QMetaObject::invokeMethod(m_dataProcessor.get(),
                              [processor = m_dataProcessor.get(), normalized]() {
                                processor->setActiveSymbol(normalized);
                              },
                              Qt::QueuedConnection);
  }
}

// The configured TPO style, session and bracket (startup, and the settings
// dialog's TPO reset).
void UnifiedGridRenderer::applyTpoConfig(const ClientTpoConfig& config) {
  TpoOverlayRenderer::Style style = m_tpoOverlay.style();
  style.layout = tpo::parseLayout(config.layout, tpo::Layout::Collapsed);
  style.theme = tpo::parseTheme(config.theme, tpo::Theme::Rainbow);
  style.rowPx = config.rowPx;
  style.maxSessions = config.sessions;
  m_tpoOverlay.setStyle(style);
  const int sessionType = tpo::parseSessionType(config.session, 4);
  const int period = static_cast<int>(tpo::resolvePeriodMs(
      sessionType, static_cast<int64_t>(std::max(1, config.periodMinutes)) * 60000));
  if (sessionType != m_tpoSessionType || period != m_tpoTimeframeMs) {
    m_tpoSessionType = sessionType;
    m_tpoTimeframeMs = period;
    emit tpoConfigChanged();
  }
  sLog_Render("TPO config: layout=" << tpo::layoutName(style.layout)
              << " theme=" << tpo::themeName(style.theme)
              << " session=" << tpo::sessionTypeName(sessionType)
              << " periodMs=" << period << " sessions=" << m_tpoOverlay.style().maxSessions
              << " rowPx=" << m_tpoOverlay.style().rowPx);
  update();
}

void UnifiedGridRenderer::setTpoTimeframeMs(int timeframeMs) {
  const int resolved = static_cast<int>(tpo::resolvePeriodMs(m_tpoSessionType, timeframeMs));
  if (m_tpoTimeframeMs == resolved) {
    return;
  }
  m_tpoTimeframeMs = resolved;
  emit tpoConfigChanged();
}

void UnifiedGridRenderer::setTpoSessionType(int sessionType) {
  const int clamped = (sessionType >= 0 &&
                       sessionType <= static_cast<int>(SessionManager::SessionType::M1)) ? sessionType : 4;
  const int period = static_cast<int>(tpo::resolvePeriodMs(clamped, m_tpoTimeframeMs));
  if (m_tpoSessionType == clamped && m_tpoTimeframeMs == period) {
    return;
  }
  m_tpoSessionType = clamped;
  m_tpoTimeframeMs = period;
  emit tpoConfigChanged();
}

void UnifiedGridRenderer::setTpoLayout(const QString& layout) {
  auto style = m_tpoOverlay.style();
  const auto next = tpo::parseLayout(layout.toStdString(), style.layout);
  if (next == style.layout) return;
  style.layout = next;
  m_tpoOverlay.setStyle(style);
  sLog_Render("TPO layout=" << tpo::layoutName(next));
  update();
  emit tpoStyleChanged();
}

void UnifiedGridRenderer::setTpoTheme(const QString& theme) {
  auto style = m_tpoOverlay.style();
  const auto next = tpo::parseTheme(theme.toStdString(), style.theme);
  if (next == style.theme) return;
  style.theme = next;
  m_tpoOverlay.setStyle(style);
  sLog_Render("TPO theme=" << tpo::themeName(next));
  update();
  emit tpoStyleChanged();
}

QString UnifiedGridRenderer::tpoLayout() const {
  return QString::fromLatin1(tpo::layoutName(m_tpoOverlay.style().layout));
}

QString UnifiedGridRenderer::tpoTheme() const {
  return QString::fromLatin1(tpo::themeName(m_tpoOverlay.style().theme));
}

void UnifiedGridRenderer::setVolumeProfileLayerEnabled(bool enabled) {
  const bool newHeatmapEnabled = enabled ? false : m_heatmapLayerEnabled;
  const bool newFootprintEnabled = enabled ? false : m_footprintLayerEnabled;
  const bool newTpoEnabled = enabled ? false : m_tpoLayerEnabled;
  if (m_volumeProfileLayerEnabled == enabled &&
      m_heatmapLayerEnabled == newHeatmapEnabled &&
      m_footprintLayerEnabled == newFootprintEnabled &&
      m_tpoLayerEnabled == newTpoEnabled) {
    return;
  }
  m_volumeProfileLayerEnabled = enabled;
  if (enabled) {
    m_heatmapLayerEnabled = false;
    m_footprintLayerEnabled = false;
    m_tpoLayerEnabled = false;
  }
  sLog_Render("layers: set volumeProfile=" << enabled
              << " -> heatmap=" << m_heatmapLayerEnabled
              << " footprint=" << m_footprintLayerEnabled
              << " tpo=" << m_tpoLayerEnabled
              << " volumeProfile=" << m_volumeProfileLayerEnabled);
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
  if (m_view->session().timeframeMs != timeframe_ms) {
    sLog_Render("timeframe changed: prevMs=" << m_view->session().timeframeMs
                << " tfMs=" << timeframe_ms);
    // A zoom glide or gesture lands first (the switch rescales the view).
    m_view->zoomGlide().endZoomGlide();
    m_view->glide().gesture = false;
    // What is drawn before the switch (the raster camera at the old timeframe): the Now
    // column keeps its drawn position, not its position in the stored bounds.
    const auto drawnBefore = rasterCameraNow(false);
    const int64_t previousTf = m_view->session().timeframeMs;
    m_view->session().timeframeMs = timeframe_ms;
    if (timeframe_ms > 0) m_timeAuthority.setActiveTimeframeMs(timeframe_ms);
    // A column is the timeframe, and a longer one is how to see further
    // back. A fixed time span would leave too few long-timeframe columns or too many
    // narrow short-timeframe columns. Scale the span by the timeframe ratio about
    // the view end, inside the new timeframe's 1 column/px limit. previousTf >= 1 s skips the 100 ms
    // default before the server advertises its timeframe. The span is read before the new
    // limits apply (72 h of 1h is 72 columns of 1m, not 30.8 h of the clamp then /60), and
    // limits, scaled span and follow-live end are published as ONE viewport change: the
    // layer's limitsChanged is held back here, so the old view is never re-clamped first.
    const bool keepColumns = m_gpuLayer && timeframe_ms > 0 && previousTf >= 1000 && getViewState() &&
                             getViewState()->isTimeWindowValid();
    const int64_t oldStart = keepColumns ? getViewState()->getVisibleTimeStart() : 0;
    const int64_t oldEnd = keepColumns ? getViewState()->getVisibleTimeEnd() : 0;
    const double scaledSpan = keepColumns ? static_cast<double>(oldEnd - oldStart) * static_cast<double>(timeframe_ms) /
                                                static_cast<double>(previousTf)
                                          : 0.0;
    // The Now column (the live bucket) keeps its screen x when it is in view or the view follows live; a
    // historical view keeps its end. rightFrac: the view's part right of the Now centre.
    const int64_t anchor = keepColumns ? m_gpuLayer->liveAnchorMs() : 0;
    auto nowCentre = [anchor](int64_t tf) {
      return static_cast<double>(recording::floorDiv(anchor + tf - 1, tf) * tf) - static_cast<double>(tf) * 0.5;
    };
    // Following live, the view stays anchored at its end (the live edge one padding
    // inside, exact): the padding scales with the span (same columns), so the Now
    // column keeps its drawn distance from that edge. A historical view keeps the Now
    // column where it is drawn (its drawn fraction, from the camera before the switch).
    const bool following = getViewState() && getViewState()->isAutoScrollEnabled();
    std::optional<double> rightFrac;
    if (anchor > 0 && oldEnd > oldStart && !following) {
      const double now = nowCentre(previousTf);
      if (drawnBefore.valid) {
        const double x = drawnBefore.xDev(now) / drawnBefore.widthDev; // its drawn fraction
        if (x >= 0.0 && x <= 1.0) rightFrac = 1.0 - x;
      } else if (now >= double(oldStart) && now <= double(oldEnd)) {
        rightFrac = (static_cast<double>(oldEnd) - now) / static_cast<double>(oldEnd - oldStart);
      }
    }
    if (m_gpuLayer && timeframe_ms > 0) {
      m_view->flags().limitsDeferred = keepColumns;
      m_gpuLayer->setTimeframeMs(timeframe_ms); // new limits (applied below when keepColumns)
      m_view->flags().limitsDeferred = false;
    }
    if (keepColumns) {
      const double maxTime = m_gpuLayer->maxTimeSpanMs();
      int64_t span = std::max<int64_t>(1, static_cast<int64_t>(std::llround(scaledSpan)));
      if (maxTime > 0) span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))));
      int64_t end = oldEnd;
      double anchorX = 1.0; // the view end (follow-live, or a historical view without Now)
      if (following) {
        const qint64 nominal = gpuLiveEndMs(span);
        if (nominal > 0) end = nominal; // the new timeframe's padded live edge
        m_followPadding.reset();
        if (drawnBefore.valid && anchor > 0 && nominal > 0) {
          // The destination camera's column width: when it equals the drawn one, the Now
          // column stays exactly where it is drawn in whole device pixels and
          // follow-live keeps that padding; when a cap changed it (a column is at most tf
          // px, at least one per px), the chart re-lays out at the nominal live end.
          const int64_t tf = timeframe_ms;
          const auto surface = m_heatmapPort->surface();
          const chart_raster::RasterStep previous = m_published.step();
          auto restAt = [&](int64_t e) {
            auto vp = FrameContextBuilder::viewportSnapshot(getViewState());
            vp.timeStart = e - span;
            vp.timeEnd = e;
            vp.dragging = false;
            vp.anchorFracX = 1.0;
            return chart_raster::computeRaster(m_view->camera().rasterInputs(vp, m_gpuLayer->tickPrice(), surface),
                                               previous);
          };
          if (const auto dest = restAt(nominal); dest.valid && dest.colPx == drawnBefore.colPx) {
            const double oldLiveEnd = double(recording::floorDiv(anchor + previousTf - 1, previousTf) * previousTf);
            const double liveEndX = drawnBefore.xDev(oldLiveEnd); // whole device px at rest
            const int64_t newLiveEnd = recording::floorDiv(anchor + tf - 1, tf) * tf;
            const double C = dest.colPx, W = dest.widthDev;
            int64_t e = newLiveEnd + std::llround((W - liveEndX) * double(tf) / C);
            for (const int64_t candidate : {e, e - 1, e + 1})
              if (const auto cam = restAt(candidate); cam.valid && cam.colPx == dest.colPx &&
                                                      cam.xDev(double(newLiveEnd)) == liveEndX) {
                e = candidate;
                break;
              }
            end = e;
            m_followPadding = FollowPadding{e - newLiveEnd, e - span, e, tf};
          }
        }
      } else if (rightFrac) {
        end = static_cast<int64_t>(std::llround(nowCentre(timeframe_ms) + *rightFrac * double(span)));
        anchorX = 1.0 - *rightFrac;
      }
      // Price: auto price scale off keeps it; on, setViewport fits the new timeframe's
      // candles held so far (later pages refit through candlesDirty). setViewport
      // applies the new max price span.
      getViewState()->setMinSpans(m_gpuLayer->minTimeSpanMs(), m_gpuLayer->minPriceSpan());
      // The raster camera keeps the Now column (else the view end) where it was drawn.
      getViewState()->setRasterAnchor(anchorX, getViewState()->rasterAnchor().fracY);
      getViewState()->setViewportAndMaxSpans(end - span, end, getViewState()->getMinPrice(), getViewState()->getMaxPrice(),
                                          maxTime, m_gpuLayer->maxPriceSpan());
      if (following && m_followPadding) m_followPadding->placement = getViewState()->placementVersion();
      m_view->syncGpuView();
      if (getViewState()->isAutoScrollEnabled()) emit liveRenderTick();
    }
    if (m_dataProcessor) {
      QMetaObject::invokeMethod(
          m_dataProcessor.get(),
          [this, timeframe_ms]() { m_dataProcessor->setTimeframe(timeframe_ms); },
          Qt::QueuedConnection);
    }
    update();
    emit timeframeChanged();
  }
}

void UnifiedGridRenderer::setHeatmapGamma(double gamma) {
  const double clamped = std::clamp(gamma, 0.1, 5.0);
  if (std::abs(m_heatmapGamma - clamped) < 1e-6) {
    return;
  }
  m_heatmapGamma = clamped;
  syncGpuTone();
  update();
  emit heatmapGammaChanged();
}

void UnifiedGridRenderer::setHeatmapContrast(double contrast) {
  const double clamped = std::clamp(contrast, 0.1, 5.0);
  if (std::abs(m_heatmapContrast - clamped) < 1e-6) {
    return;
  }
  m_heatmapContrast = clamped;
  syncGpuTone();
  update();
  emit heatmapContrastChanged();
}

void UnifiedGridRenderer::setHeatmapShaderFloor(double floor) {
  const double clamped = std::clamp(floor, 0.0, 0.5);
  if (std::abs(m_heatmapShaderFloor - clamped) < 1e-6) {
    return;
  }
  m_heatmapShaderFloor = clamped;
  syncGpuTone();
  update();
  emit heatmapShaderFloorChanged();
}

void UnifiedGridRenderer::setCandleStyle(int style) {
    style = std::clamp(style, 0, 2);
    if (m_candleStyle == style) return;
    m_candleStyle = style;
    emit candleStyleChanged();
}

void UnifiedGridRenderer::setCandleAppearance(const heatmap::HeatmapChartSettings& settings) {
    const QColor up(QString::fromStdString(settings.candleUpColor));
    const QColor down(QString::fromStdString(settings.candleDownColor));
    const QString wick = QString::fromStdString(settings.candleWickColor);
    if (m_candleUpColor == up && m_candleDownColor == down && m_candleWickColor == wick &&
        m_candleBodyOpacity == settings.candleBodyOpacity && m_candleWickWidth == settings.candleWickWidth) return;
    m_candleUpColor = up;
    m_candleDownColor = down;
    m_candleWickColor = wick;
    m_candleBodyOpacity = settings.candleBodyOpacity;
    m_candleWickWidth = settings.candleWickWidth;
    emit candleAppearanceChanged();
}

void UnifiedGridRenderer::setPrimaryField(int field) {
  if (m_primaryField == field) {
    return;
  }
  m_primaryField = field;
  if (field == 0) {
    m_heatmapLayerEnabled = true;
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  } else if (field == 1) {
    m_footprintLayerEnabled = true;
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  } else if (field == 2) {
    m_tpoLayerEnabled = true;
    m_heatmapLayerEnabled = false;
    m_footprintLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  } else if (field == 3) {
    m_volumeProfileLayerEnabled = true;
    m_tpoLayerEnabled = false;
    m_heatmapLayerEnabled = false;
    m_footprintLayerEnabled = false;
  }
  sLog_Render("layers: primaryField=" << m_primaryField
              << " -> heatmap=" << m_heatmapLayerEnabled
              << " footprint=" << m_footprintLayerEnabled
              << " tpo=" << m_tpoLayerEnabled
              << " volumeProfile=" << m_volumeProfileLayerEnabled);
  update();
  emit primaryFieldChanged();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setHeatmapLayerEnabled(bool enabled) {
  const bool newTpoEnabled = enabled ? false : m_tpoLayerEnabled;
  const bool newVpEnabled = enabled ? false : m_volumeProfileLayerEnabled;
  if (m_heatmapLayerEnabled == enabled && m_tpoLayerEnabled == newTpoEnabled &&
      m_volumeProfileLayerEnabled == newVpEnabled) {
    sLog_Render("layers: set heatmap=" << enabled << " unchanged");
    return;
  }
  m_heatmapLayerEnabled = enabled;
  if (enabled) {
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  sLog_Render("layers: set heatmap=" << enabled
              << " -> heatmap=" << m_heatmapLayerEnabled
              << " footprint=" << m_footprintLayerEnabled
              << " tpo=" << m_tpoLayerEnabled
              << " volumeProfile=" << m_volumeProfileLayerEnabled);
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setFootprintLayerEnabled(bool enabled) {
  const bool newTpoEnabled = enabled ? false : m_tpoLayerEnabled;
  const bool newVpEnabled = enabled ? false : m_volumeProfileLayerEnabled;
  if (m_footprintLayerEnabled == enabled &&
      m_tpoLayerEnabled == newTpoEnabled &&
      m_volumeProfileLayerEnabled == newVpEnabled) {
    sLog_Render("layers: set footprint=" << enabled << " unchanged");
    return;
  }
  m_footprintLayerEnabled = enabled;
  if (enabled) {
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  sLog_Render("layers: set footprint=" << enabled
              << " -> heatmap=" << m_heatmapLayerEnabled
              << " footprint=" << m_footprintLayerEnabled
              << " tpo=" << m_tpoLayerEnabled
              << " volumeProfile=" << m_volumeProfileLayerEnabled);
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setTpoLayerEnabled(bool enabled) {
  const bool newHeatmapEnabled = enabled ? false : m_heatmapLayerEnabled;
  const bool newFootprintEnabled = enabled ? false : m_footprintLayerEnabled;
  const bool newVolumeProfileEnabled =
      enabled ? false : m_volumeProfileLayerEnabled;
  if (m_tpoLayerEnabled == enabled &&
      m_heatmapLayerEnabled == newHeatmapEnabled &&
      m_footprintLayerEnabled == newFootprintEnabled &&
      m_volumeProfileLayerEnabled == newVolumeProfileEnabled) {
    sLog_Render("layers: set tpo=" << enabled << " unchanged");
    return;
  }
  m_tpoLayerEnabled = enabled;
  if (enabled) {
    m_heatmapLayerEnabled = false;
    m_footprintLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  sLog_Render("layers: set tpo=" << enabled
              << " -> heatmap=" << m_heatmapLayerEnabled
              << " footprint=" << m_footprintLayerEnabled
              << " tpo=" << m_tpoLayerEnabled
              << " volumeProfile=" << m_volumeProfileLayerEnabled);
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
  if (!getViewState()) return;
  getViewState()->enableAutoScroll(enabled);
  if (enabled) returnGpuToLive(); // back to the live edge from history or the future, same span
  update();
  emit autoScrollEnabledChanged();
  sLog_Render("auto-scroll enabled=" << enabled << " reason=request");
}


// ── GPU heatmap renderer ──
double UnifiedGridRenderer::heatmapTickSize() const {
  return m_gpuLayer ? m_gpuLayer->tickPrice() : 0.0;
}

void UnifiedGridRenderer::setHeatmapService(heatmap::HeatmapDataService* service) {
  if (!m_gpuLayer) return;
  m_gpuLayer->setService(service);
  bootstrapGpuTimeView(); // no view yet: wait for this service's availability
}

void UnifiedGridRenderer::setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings,
                                                  bool explicitManualTick) {
  if (!m_gpuLayer) return;
  m_gpuLayer->setSettings(settings, explicitManualTick);
  refitAutoPrice();
  const auto gradients = heatmap::gpu::gradientsFor(settings);
  m_showTrades = settings.showTrades;
  if (m_tradesAboveCandles != settings.tradesAboveCandles) {
    m_tradesAboveCandles = settings.tradesAboveCandles;
    emit tradeBubbleSettingsChanged();
  }
  m_tradeMinNotional = settings.tradeMinNotional;
  const auto& bid = gradients.bid.back();
  const auto& ask = gradients.ask.back();
  m_tradeBuyColor = QColor(bid.r, bid.g, bid.b);
  m_tradeSellColor = QColor(ask.r, ask.g, ask.b);
  m_view->applyGpuLimits();
  update();
}

void UnifiedGridRenderer::setHeatmapTickMemory(const heatmap::ManualTickMemory& memory) {
  if (m_gpuLayer) m_gpuLayer->setTickMemory(memory);
}

// The GPU palette's tone mapping: gamma, contrast and shader floor.
void UnifiedGridRenderer::syncGpuTone() {
  if (m_gpuLayer) {
    m_gpuLayer->setTone({static_cast<float>(m_heatmapGamma), static_cast<float>(m_heatmapContrast),
                         static_cast<float>(m_heatmapShaderFloor)});
  }
}

void UnifiedGridRenderer::syncGpuSurface() {
  if (!m_gpuLayer) return;
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  m_gpuLayer->setSurface(width(), height(), dpr);
  refitAutoPrice(); // raw row height and candidate pixel phases follow the surface
}

// Follow-live from LiveSnapshot::openEndMs: the view end stays one padding past
// the live bucket's end.
void UnifiedGridRenderer::followGpuLive() {
  if (m_gpuLayer) followGpuLiveTo(m_gpuLayer->liveOpenEndMs());
}

void UnifiedGridRenderer::followGpuLiveTo(int64_t openEnd) {
  if (!m_gpuLayer || !getViewState() || !getViewState()->isTimeWindowValid() ||
      !getViewState()->isAutoScrollEnabled() || getViewState()->isDragging())
    return;
  const int64_t tf = m_view->session().timeframeMs;
  if (openEnd <= 0 || tf <= 0) return;
  const int64_t start = getViewState()->getVisibleTimeStart(), end = getViewState()->getVisibleTimeEnd();
  const int64_t span = end - start;
  const int64_t liveEnd = recording::floorDiv(openEnd + tf - 1, tf) * tf;
  // The padding a timeframe switch kept (the Now column where it was drawn), while the
  // span and timeframe are the switch's; else the nominal one.
  // Only while the view is still the placement the switch made (follow updates move it
  // along); any other write of the time window (return to live, reset, the API, an
  // explicit view, a pan) is a new placement with the nominal padding.
  const bool kept = m_followPadding && m_followPadding->startMs == start && m_followPadding->endMs == end &&
                    m_followPadding->tfMs == tf && m_followPadding->placement == getViewState()->placementVersion();
  const int64_t pad = kept ? m_followPadding->padMs
                           : std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) *
                                                                         kAutoScrollPaddingFrac));
  const int64_t target = liveEnd + pad;
  // Whole buckets, and nothing while the end is within one drawn pixel of the padding:
  // the drawn phase never changes, a live step moves the picture by whole columns.
  const auto cam = restCameraNow(false);
  const int64_t shift = chart_raster::followShift(end, target, tf, cam.valid ? cam.colPx : 0);
  if (shift <= 0) return; // the live bucket is inside the padded view
  // A glide in flight on time moves with the view (the same world shift at both ends).
  m_view->zoomGlide().shiftTime(shift);
  m_view->setGpuViewportSelf(start + shift, end + shift, getViewState()->getMinPrice(), getViewState()->getMaxPrice(), true);
  if (kept) { // the follow update keeps the switch's placement
    m_followPadding->startMs = getViewState()->getVisibleTimeStart();
    m_followPadding->endMs = getViewState()->getVisibleTimeEnd();
  }
  emit liveRenderTick();
}

// On cold start, the first book top places the first viewport and
// the view is requested at once; the node draws the loading hatch until the newest
// span lands. The price window uses initial_price_pct of kSeedBandRows rows at
// kSeedBandTick.
void UnifiedGridRenderer::seedGpuViewport(double bestBid, double bestAsk) {
  if (!getViewState() || !std::isfinite(bestBid) || !std::isfinite(bestAsk) || bestBid <= 0 || bestAsk < bestBid) return;
  const bool timeValid = getViewState()->isTimeWindowValid();
  if (timeValid && m_view->flags().priceKnown && !m_view->flags().reseedPrice) return;
  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
  qint64 start = getViewState()->getVisibleTimeStart(), end = getViewState()->getVisibleTimeEnd();
  if (!timeValid) {
    const int64_t span = gpuInitialSpanMs(width());
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    m_view->flags().pristine = true;
    end = recording::floorDiv(now, tf) * tf + tf +
          std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * kAutoScrollPaddingFrac));
    start = end - span;
  }
  const int pct = m_view->initialViewSettings().pricePct;
  const double full = kSeedBandTick * kSeedBandRows;
  const double span = pct > 0 && pct < 100 ? full * pct / 100.0 : full;
  const double mid = (bestBid + bestAsk) * 0.5;
  const double lo = mid - span * 0.5, hi = mid + span * 0.5;
  const bool carry = m_priceCarry && timeValid && m_view->flags().reseedPrice;
  if (!carry) m_priceCarry.reset();
  m_view->flags().priceKnown = true;
  m_view->flags().reseedPrice = false;
  sLog_Render("GPU heatmap viewport seeded from the book top: mid=" << mid << " time=[" << start << ".." << end
              << "] carry=" << carry << " autoPriceScale=" << autoPriceScale());
  // A symbol switch with auto price scale off: the carried zoom; else the default band
  // (auto price scale on: the visible candles win inside setViewport).
  // The raster camera's anchor: the seeded price centred, a seeded time at its end.
  getViewState()->setRasterAnchor(timeValid ? getViewState()->rasterAnchor().fracX : 1.0, 0.5);
  if (!carry || !applyPriceCarry(mid)) m_view->setGpuViewportSelf(start, end, lo, hi);
  m_view->syncGpuView(); // also when the viewport did not change (priceKnown flips)
  update();
}

bool UnifiedGridRenderer::applyPriceCarry(double now, bool liveOnly) {
  if (!m_priceCarry || !getViewState() || !(now > 0) || !std::isfinite(now)) return false;
  const double span = m_priceCarry->spanRatio * now;
  const double lo = now - m_priceCarry->heightFrac * span, hi = lo + span;
  if (!std::isfinite(span) || !(span > 0) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
    m_priceCarry.reset();
    return false;
  }
  // Consumed once the layer's price scale (and so its limits) is the new symbol's;
  // before that the limits are unknown (no clamp) and the carry is applied again
  // from the then current price when they arrive (limitsChanged). A live-only symbol
  // (no recorded availability) never gets one: kCarryLiveOnlyWaitMs after the first
  // pending apply, the carry is applied from the live price and consumed, with no
  // Manual max span (the limits stay unknown).
  const bool confirmed = liveOnly || (m_gpuLayer && m_gpuLayer->priceScaleCurrent());
  sLog_Render("price carry applied: now=" << now << " price=[" << lo << ".." << hi << "] confirmed=" << confirmed
              << " liveOnly=" << liveOnly);
  if (confirmed) {
    m_priceCarry.reset();
    if (m_carryWaitTimer) m_carryWaitTimer->stop();
  } else {
    if (!m_carryWaitTimer) {
      m_carryWaitTimer = new QTimer(this);
      m_carryWaitTimer->setSingleShot(true);
      connect(m_carryWaitTimer, &QTimer::timeout, this, [this] {
        if (m_gpuLayer && m_priceCarry && !m_view->flags().reseedPrice && !m_gpuLayer->priceScaleCurrent())
          applyPriceCarry(m_view->gpuLivePrice(), true);
      });
    }
    if (!m_carryWaitTimer->isActive()) m_carryWaitTimer->start(kCarryLiveOnlyWaitMs);
  }
  m_view->setGpuViewportSelf(getViewState()->getVisibleTimeStart(), getViewState()->getVisibleTimeEnd(), lo, hi, true);
  return true;
}

qint64 UnifiedGridRenderer::gpuLiveEndMs(qint64 spanMs) const {
  const int64_t anchor = m_gpuLayer ? m_gpuLayer->liveAnchorMs() : 0;
  const int64_t tf = m_view->session().timeframeMs;
  if (anchor <= 0 || tf <= 0) return 0;
  const int64_t liveEnd = recording::floorDiv(anchor + tf - 1, tf) * tf;
  return liveEnd + std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(spanMs) * kAutoScrollPaddingFrac));
}

void UnifiedGridRenderer::returnGpuToLive() {
  if (!m_gpuLayer || !getViewState() || !getViewState()->isTimeWindowValid()) return;
  const int64_t span = getViewState()->getVisibleTimeEnd() - getViewState()->getVisibleTimeStart();
  const int64_t end = gpuLiveEndMs(span);
  if (end <= 0) return; // nothing known yet: the next live frame steps forward
  sLog_Render("GPU heatmap returns to live: anchor=" << m_gpuLayer->liveAnchorMs() << " end=" << end);
  getViewState()->setRasterAnchor(1.0, getViewState()->rasterAnchor().fracY); // the live edge
  getViewState()->setViewport(end - span, end, getViewState()->getMinPrice(), getViewState()->getMaxPrice());
  m_view->syncGpuView(); // publishes the view even when it was already there
  emit liveRenderTick();
}

void UnifiedGridRenderer::bootstrapGpuTimeView() {
  if (!m_gpuLayer || !getViewState() || getViewState()->isTimeWindowValid()) {
    if (m_gpuBootstrapTimer) m_gpuBootstrapTimer->stop();
    return;
  }
  const int64_t anchor = m_gpuLayer->liveAnchorMs();
  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
  if (anchor <= 0) {
    // Availability arrives on the data thread: look again shortly (stops once
    // a view exists, from here, a book top, a trade or the API).
    if (!m_gpuBootstrapTimer) {
      m_gpuBootstrapTimer = new QTimer(this);
      m_gpuBootstrapTimer->setInterval(50);
      connect(m_gpuBootstrapTimer, &QTimer::timeout, this, [this] { bootstrapGpuTimeView(); });
    }
    if (!m_gpuBootstrapTimer->isActive()) m_gpuBootstrapTimer->start();
    return;
  }
  if (m_gpuBootstrapTimer) m_gpuBootstrapTimer->stop();
  const int64_t span = gpuInitialSpanMs(width());
  const int64_t end = recording::floorDiv(anchor, tf) * tf + tf +
                      std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * kAutoScrollPaddingFrac));
  sLog_Render("GPU heatmap time-only bootstrap from availability: anchor=" << anchor << " time=[" << end - span
              << ".." << end << "] (price from the first decoded data)");
  m_view->flags().priceKnown = false;
  m_view->flags().pristine = true;
  getViewState()->setRasterAnchor(1.0, 0.5);
  m_view->setGpuViewportSelf(end - span, end, 0.0, 1.0); // placeholder price: nothing draws until it is known
  m_view->syncGpuView();
}

// The first view's time span: initial_column_px per column (16 columns min).
qint64 UnifiedGridRenderer::gpuInitialSpanMs(double widthPx) const {
  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
  const double width = widthPx > 0.0 ? widthPx : 800.0;
  return static_cast<int64_t>(std::max(16, static_cast<int>(width / m_view->initialViewSettings().columnPx))) * tf;
}

// ── Auto-fit ────────────────────────────────────────────────────────────────
QObject* UnifiedGridRenderer::candleBuffer() const { return m_candleBuffer.data(); }

void UnifiedGridRenderer::setCandleBuffer(QObject* buffer) {
  auto* candles = qobject_cast<CandleSeriesBuffer*>(buffer);
  if (m_candleBuffer == candles) return;
  if (m_candleDirtyConn) disconnect(m_candleDirtyConn);
  m_candleBuffer = candles;
  if (candles) {
    // Auto price scale: a candle update or history page in view refits price (a few
    // compares when it is off or out of view; a bump only when the fit changed).
    m_candleDirtyConn = connect(candles, &CandleSeriesBuffer::candlesDirty, this,
                                [this](const QString& symbol, int64_t timeframeSec, qint64 dirtyStart,
                                       qint64 dirtyEnd) {
                                  if (!getViewState() || !getViewState()->autoPriceScale() ||
                                      !getViewState()->isTimeWindowValid() || symbol != m_view->session().symbol)
                                    return;
                                  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
                                  if (timeframeSec != std::max<int64_t>(1, (tf + 500) / 1000)) return;
                                  // The window drawn on screen (a drag commits only at release).
                                  const auto [shownStart, shownEnd] = getViewState()->displayedTimeWindow();
                                  const auto [drawnStart, drawnEnd] = m_view->camera().drawnTimeWindow(shownStart, shownEnd);
                                  const qint64 viewStart =
                                      recording::floorDiv(static_cast<qint64>(std::floor(drawnStart)), tf) * tf;
                                  if (std::max(dirtyEnd, dirtyStart + tf) <= viewStart || double(dirtyStart) >= drawnEnd)
                                    return;
                                  refitAutoPrice();
                                });
  }
  refitAutoPrice();
  emit candleBufferChanged();
}

std::optional<std::pair<qint64, qint64>> UnifiedGridRenderer::gpuFitTimeWindow() const {
  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
  const int64_t anchor = m_gpuLayer->liveAnchorMs();
  if (anchor <= 0) return std::nullopt;
  int64_t oldest = m_gpuLayer->oldestAvailableMs();
  if (oldest <= 0 && m_candleBuffer)
    oldest = m_candleBuffer->oldestTimeMs(m_view->session().symbol, std::max<int64_t>(1, (tf + 500) / 1000));
  const int64_t dataEnd = recording::floorDiv(anchor + tf - 1, tf) * tf; // the live bucket's end
  const int64_t dataStart = oldest > 0 ? recording::floorDiv(oldest, tf) * tf : 0;
  const double maxTime = getViewState()->maxTimeSpanMs();
  const int64_t maxSpan = maxTime > 0 ? std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))) : INT64_MAX / 4;
  const int64_t minSpan = std::max<int64_t>(tf, static_cast<int64_t>(std::ceil(getViewState()->minTimeSpanMs())));
  const double pad = kAutoScrollPaddingFrac;
  // The span that shows [dataStart, dataEnd] plus the follow-live padding
  // (max(tf, span * pad)) after it.
  int64_t span = maxSpan;
  bool fits = false;
  if (dataStart > 0 && dataEnd > dataStart) {
    const double dataSpan = static_cast<double>(dataEnd - dataStart);
    const double padded = pad > 0 && pad < 1 && dataSpan / (1 - pad) * pad >= double(tf) ? dataSpan / (1 - pad)
                                                                                          : dataSpan + double(tf);
    const int64_t wanted = static_cast<int64_t>(std::ceil(padded));
    fits = wanted <= maxSpan;
    span = std::min(wanted, maxSpan);
  }
  span = std::max(span, std::min(minSpan, maxSpan));
  const bool following = getViewState()->isAutoScrollEnabled();
  if (fits || following || dataStart <= 0) {
    const int64_t end = gpuLiveEndMs(span);
    if (end <= 0) return std::nullopt;
    return std::make_pair<qint64, qint64>(end - span, qint64(end));
  }
  // History that does not fit: keep the view centre, inside the data.
  const int64_t centre = getViewState()->getVisibleTimeStart() +
                         (getViewState()->getVisibleTimeEnd() - getViewState()->getVisibleTimeStart()) / 2;
  const int64_t lastStart = gpuLiveEndMs(span) - span;
  const int64_t start = std::clamp<int64_t>(centre - span / 2, dataStart, std::max(dataStart, lastStart));
  return std::make_pair<qint64, qint64>(qint64(start), start + span);
}

std::optional<std::pair<double, double>> UnifiedGridRenderer::gpuFitPriceWindow(qint64 start, qint64 end,
                                                                                bool candlesOnly,
                                                                                std::optional<double> anchorFracX) {
  const int64_t tf = std::max<int64_t>(1, m_view->session().timeframeMs);
  double lo = 0, hi = 0, centre = 0, span = 0;
  // The candles the window draws: the raster camera's time window (wider or narrower
  // than [start, end] by the column rounding), as the candle overlay selects them.
  const auto [drawnStart, drawnEnd] = m_view->camera().drawnTimeWindow(start, end, anchorFracX);
  const qint64 first = static_cast<qint64>(std::floor(drawnStart)), last = static_cast<qint64>(std::ceil(drawnEnd));
  // From the bucket that contains the view start: getVisibleSlice selects by bar
  // start, and the candle that starts before the view and ends inside it counts.
  const int64_t alignedStart = recording::floorDiv(first, tf) * tf;
  auto& bars = m_fitBars; // reused: the auto price scale runs this on every change in view
  bars.clear();
  if (m_candleBuffer && last > first)
    m_candleBuffer->getVisibleSlice(m_view->session().symbol, std::max<int64_t>(1, (tf + 500) / 1000), alignedStart, last, bars);
  size_t used = 0;
  double newestClose = 0;
  qint64 newestStart = 0;
  for (const auto& bar : bars) {
    const qint64 barEnd = bar.timeEndMs > bar.timeStartMs ? bar.timeEndMs : bar.timeStartMs + tf;
    if (double(bar.timeStartMs) >= drawnEnd || double(barEnd) <= drawnStart || !std::isfinite(bar.high) ||
        !std::isfinite(bar.low) || !(bar.low > 0) || bar.high < bar.low)
      continue;
    lo = used ? std::min(lo, bar.low) : bar.low;
    hi = used ? std::max(hi, bar.high) : bar.high;
    if (!used || bar.timeStartMs >= newestStart) {
      newestStart = bar.timeStartMs;
      newestClose = std::isfinite(bar.close) && bar.close > 0 ? bar.close : (bar.low + bar.high) * 0.5;
    }
    ++used;
  }
  const bool onePrice = used && hi == lo;
  const double current = getViewState()->getMaxPrice() - getViewState()->getMinPrice();
  if (used) {
    centre = (lo + hi) * 0.5;
    span = hi > lo ? (hi - lo) * (1.0 + 2.0 * kFitPriceMargin) : current;
  } else {
    if (candlesOnly) return std::nullopt;
    centre = m_view->gpuLivePrice();
    span = current;
  }
  if (!(centre > 0) || !std::isfinite(centre)) return std::nullopt;
  if (!(span > 0) || !std::isfinite(span) || span > centre * 4) span = centre * 0.01; // e.g. a span from another symbol
  // A minimum span wider than the price itself is another symbol's (the layer's limits
  // follow the new symbol's data a moment after a switch): not applied.
  if (const double minSpan = getViewState()->minPriceSpan(); minSpan < centre) span = std::max(span, minSpan);
  const double maxSpan = getViewState()->maxPriceSpan();
  if (maxSpan > 0 && span > maxSpan) {
    // Manual tick: the user's tick stays (never Auto,
    // never coarsened); the candles need more than one row per pixel, so show what
    // the limit can, centred on the current price: book mid, last trade, newest close.
    span = maxSpan;
    if (used) {
      const double now = m_view->session().bookMid > 0 ? m_view->session().bookMid : m_view->session().lastTrade > 0 ? m_view->session().lastTrade : newestClose;
      if (now > 0) centre = now;
    }
  }
  lo = centre - span * 0.5;
  hi = centre + span * 0.5;
  // Whole pixels per row (raster camera): the fitted span is heightDev * tick / P for
  // an integer P, its top on a row edge, so the drawn window equals the fitted one
  // (no snap crops the candles). Before a tick is drawn the span stays.
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  const int H = chart_raster::devicePixels(height(), dpr);
  const double tick = m_gpuLayer ? m_gpuLayer->tickPrice() : 0.0;
  if (getViewState()->autoPriceScale() && m_gpuLayer && !m_gpuLayer->manualMode()) {
    // Startup and symbol/timeframe switches can lack a current resolution; a fit
    // needs it, while a failed solve after resolution arrives warrants a warning.
    if (!m_gpuLayer->hasCurrentResolution()) return std::make_pair(lo, hi);
    if (onePrice && current > 0 && std::isfinite(current)) {
      // One price (hi == lo) has no new scale information. Keep the span and
      // leave the tick decision/proposal untouched; translate by drawn whole rows.
      double low = getViewState()->getMinPrice(), high = getViewState()->getMaxPrice();
      const double row = m_gpuLayer->tickPrice();
      const double margin = current * kFitPriceMargin;
      if ((centre <= low + margin || centre >= high - margin) && std::isfinite(row) && row > 0) {
        const double shift = std::round((centre - (low + high) * .5) / row) * row;
        low += shift;
        high += shift;
      }
      return std::make_pair(low, high);
    }
    // Solve from the raw candle bounds and check coverage for each candidate's
    // fitted window; publish its tick before setViewport notifies the renderer.
    const auto &settings = m_gpuLayer->settings();
    const auto fit = chart_raster::solveAutoPriceFit(lo, hi, H, m_gpuLayer->autoTickState(), m_gpuLayer->priceScale(),
        {settings.minRowPx, settings.hysteresis}, [&](int64_t units, double low, double high) {
          return m_gpuLayer->buildsTick(units, {drawnStart, drawnEnd, low, high});
        });
    if (m_gpuLayer->setAutoPriceTick(fit ? std::optional(fit->tickUnits) : std::nullopt)) update();
    if (fit) {
      sLog_Probe("zoom.fit", "raw=[" << lo << ".." << hi << "] fit=[" << fit->lo << ".." << fit->hi
                 << "] tick=" << fit->tick << " P=" << fit->rowPx);
      lo = fit->lo;
      hi = fit->hi;
    } else {
      sLog_Warning("auto price fit unavailable; publishing plain fit time=[" << start << ".." << end
                   << "] raw=[" << lo << ".." << hi << "]");
      chart_raster::fitPriceToRows(lo, hi, H, tick);
    }
  } else {
    // Manual tick and auto scale off keep rounded-centre placement
    // and the P=1 clamp, including the Manual maximum-span behavior.
    const double priceSpan = hi - lo;
    if (H > 0 && std::isfinite(tick) && tick > 0 && std::isfinite(priceSpan) && priceSpan > 0) {
      const double P = std::clamp(std::floor(double(H) * tick / priceSpan), 1.0, double(chart_raster::kMaxCellPx));
      const double fitted = double(H) * tick / P;
      const double top = std::round(((lo + hi) * .5 + fitted * .5) * P / tick) * tick / P;
      if (std::isfinite(top)) { hi = top; lo = top - fitted; }
    }
  }
  return std::make_pair(lo, hi);
}

bool UnifiedGridRenderer::autoPriceFit(qint64 start, qint64 end, double& priceMin, double& priceMax) {
  if (!m_gpuLayer || !getViewState()) return false;
  const auto window = gpuFitPriceWindow(start, end, true);
  if (!window) {
    if (m_gpuLayer->setAutoPriceTick(std::nullopt)) update();
    return false; // no visible candle: the given price stays
  }
  std::tie(priceMin, priceMax) = *window;
  m_view->flags().priceKnown = true; // a real price window (no book-top seed replaces it)
  m_view->flags().reseedPrice = false;
  m_priceCarry.reset();
  return true;
}

void UnifiedGridRenderer::refitAutoPrice() {
  if (!getViewState() || !getViewState()->autoPriceScale() || !getViewState()->isTimeWindowValid() ||
      m_view->flags().limitsDeferred)
    return;
  const bool known = m_view->flags().priceKnown;
  m_view->setGpuViewportSelf(getViewState()->getVisibleTimeStart(), getViewState()->getVisibleTimeEnd(), getViewState()->getMinPrice(),
                     getViewState()->getMaxPrice(), true);
  if (m_view->flags().priceKnown != known) m_view->syncGpuView();
}

void UnifiedGridRenderer::setAutoPriceScale(bool enabled) {
  if (!getViewState()) return;
  if (!enabled) {
    getViewState()->setAutoPriceScale(false); // the price range stays where it is
    return;
  }
  // On = the price-axis double-click: fit now (candles, else the live price); with
  // nothing to fit yet it is on and the next change in view fits.
  if (!fitView(false, true)) getViewState()->setAutoPriceScale(true);
}

bool UnifiedGridRenderer::fitView(bool time, bool price) {
  m_view->zoomGlide().endZoomGlide();
  m_view->glide().gesture = false;
  if (!m_gpuLayer || !getViewState() || !getViewState()->isTimeWindowValid() || (!time && !price) ||
      getViewState()->isDragging()) {
    sLog_Render("view fit skipped: time=" << time << " price=" << price);
    return false;
  }
  qint64 start = getViewState()->getVisibleTimeStart(), end = getViewState()->getVisibleTimeEnd();
  double lo = getViewState()->getMinPrice(), hi = getViewState()->getMaxPrice();
  // All or nothing: every requested part must fit, else nothing changes (a failed fit
  // leaves the view, follow-live and the auto price scale as they were).
  bool ok = true;
  if (time) {
    if (const auto window = gpuFitTimeWindow()) std::tie(start, end) = *window;
    else ok = false;
  }
  if (price && ok) {
    // Candles, else the live price (setViewport's auto fit takes the candles first),
    // over what the fitted window draws (a time fit anchors the view end).
    if (const auto window = gpuFitPriceWindow(start, end, false, time ? std::optional<double>(1.0) : std::nullopt))
      std::tie(lo, hi) = *window;
    else ok = false;
  }
  sLog_Render("view fit time=" << time << " price=" << price << " applied=" << ok << " time=[" << start << ".."
              << end << "] price=[" << lo << ".." << hi << "]");
  if (!ok) return false;
  if (price) {
    m_view->flags().priceKnown = true;
    m_view->flags().reseedPrice = false;
    getViewState()->setAutoPriceScale(true); // a price fit is the auto price scale
  }
  // Raster anchor of the fitted axes: the view end (live edge) and the price centre.
  const auto anchor = getViewState()->rasterAnchor();
  getViewState()->setRasterAnchor(time ? 1.0 : anchor.fracX, price ? 0.5 : anchor.fracY);
  getViewState()->setViewport(start, end, lo, hi); // one change; follow-live is kept
  m_view->syncGpuView();
  update();
  return true;
}

QString UnifiedGridRenderer::applyViewportRequest(const ViewportRequest& request) {
  if (!getViewState()) return QStringLiteral("viewport_unavailable");
  m_view->zoomGlide().endZoomGlide();
  m_view->glide().gesture = false;
  if (!request.fit.isEmpty()) {
    const bool time = request.fit != "price", price = request.fit != "time";
    const bool ok = request.fit == "default" ? resetView() : fitView(time, price);
    return ok ? QString() : QStringLiteral("fit_unavailable");
  }
  const bool explicitPrice = request.priceMin && request.priceMax;
  const bool explicitTime = request.startMs && request.endMs;
  const qint64 start0 = getViewState()->getVisibleTimeStart(), end0 = getViewState()->getVisibleTimeEnd();
  // The flags first (no viewport change of their own), then one commit.
  if (explicitPrice) {
    getViewState()->setAutoPriceScale(false); // explicit bounds are the user's price, equal or not
    m_priceCarry.reset();
  } else if (request.autoScale) {
    getViewState()->setAutoPriceScale(*request.autoScale);
  }
  const bool follow = (explicitTime || explicitPrice) ? false : request.followLive.value_or(autoScrollEnabled());
  getViewState()->enableAutoScroll(follow);
  qint64 start = request.startMs.value_or(start0), end = request.endMs.value_or(end0);
  if (follow && !explicitTime) {
    // Follow-live on: the live edge one padding inside, span kept (returnGpuToLive).
    const qint64 span = end - start;
    if (const qint64 liveEnd = gpuLiveEndMs(span); liveEnd > 0) {
      end = liveEnd;
      start = liveEnd - span;
    }
  }
  double lo = request.priceMin.value_or(getViewState()->getMinPrice());
  double hi = request.priceMax.value_or(getViewState()->getMaxPrice());
  if (explicitPrice) {
    m_view->flags().priceKnown = true;
    m_view->flags().reseedPrice = false;
  } else if (request.autoScale.value_or(false)) {
    // On: the price-axis double-click (candles; with none, the live price centred), over
    // what the requested window draws with the anchor it gets below.
    const double fracX = follow ? 1.0 : (start != start0 || end != end0) ? 0.5 : getViewState()->rasterAnchor().fracX;
    if (const auto window = gpuFitPriceWindow(start, end, false, fracX)) {
      std::tie(lo, hi) = *window;
      m_view->flags().priceKnown = true;
      m_view->flags().reseedPrice = false;
    }
  }
  sLog_Render("viewport request: time=[" << start << ".." << end << "] price=[" << lo << ".." << hi
              << "] follow=" << follow << " autoPriceScale=" << getViewState()->autoPriceScale());
  // Raster anchor: the centre of an explicitly moved axis (following live keeps the
  // live edge); an unchanged axis keeps its anchor (no re-snap).
  const auto anchor = getViewState()->rasterAnchor();
  const bool timeMoved = start != start0 || end != end0;
  const bool priceMoved = lo != getViewState()->getMinPrice() || hi != getViewState()->getMaxPrice();
  getViewState()->setRasterAnchor(follow ? 1.0 : timeMoved ? 0.5 : anchor.fracX, priceMoved ? 0.5 : anchor.fracY);
  getViewState()->setViewport(start, end, lo, hi); // one change (the auto fit applies inside)
  m_view->syncGpuView();
  if (follow) emit liveRenderTick();
  update();
  return {};
}

bool UnifiedGridRenderer::resetView() {
  m_view->zoomGlide().endZoomGlide();
  m_view->glide().gesture = false;
  if (!m_gpuLayer || !getViewState() || !getViewState()->isTimeWindowValid() ||
      getViewState()->isDragging())
    return false;
  int64_t span = gpuInitialSpanMs(width());
  if (const double maxTime = getViewState()->maxTimeSpanMs(); maxTime > 0)
    span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))));
  const int64_t end = gpuLiveEndMs(span);
  if (end <= 0) {
    sLog_Render("view reset skipped: no live anchor yet");
    return false;
  }
  getViewState()->setAutoPriceScale(true);
  getViewState()->enableAutoScroll(true); // the view state's flag only: the viewport moves once, below
  double lo = getViewState()->getMinPrice(), hi = getViewState()->getMaxPrice();
  if (const auto window = gpuFitPriceWindow(end - span, end, false, 1.0)) {
    std::tie(lo, hi) = *window;
    m_view->flags().priceKnown = true;
    m_view->flags().reseedPrice = false;
  }
  sLog_Render("view reset to default: time=[" << end - span << ".." << end << "] price=[" << lo << ".." << hi << "]");
  getViewState()->setRasterAnchor(1.0, 0.5);
  m_view->setGpuViewportSelf(end - span, end, lo, hi);
  m_view->flags().pristine = true; // the default span follows the chart's width again
  m_view->syncGpuView();
  emit liveRenderTick();
  update();
  return true;
}

void UnifiedGridRenderer::buildMsdfAtlas() {
  if (m_chartTextAtlasBuilt) {
    return;
  }
  ChartTextAtlas::BuildParams params;
  params.fontFamily = "Roboto Mono";
  params.fontPx = 64;
  params.pxRange = 4.0f;
  params.charset = QStringLiteral(
      " 0123456789.+-,:/"
      "$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  params.resourceFont =
      QStringLiteral(":/fonts/RobotoMono/RobotoMono-Regular.ttf");
  const QByteArray envFont = qgetenv("SENTINEL_MSDF_FONT");
  if (!envFont.isEmpty()) {
    params.fontPath = QString::fromUtf8(envFont);
  } else {
    const auto &client = GuiConfigStore::instance().clientConfig();
    if (!client.gui.msdfFontPath.empty()) {
      params.fontPath = QString::fromStdString(client.gui.msdfFontPath);
    }
  }
  envIntValue("SENTINEL_CHART_TEXT_FONT_PX", params.fontPx);
  envFloatValue("SENTINEL_CHART_TEXT_PX_RANGE", params.pxRange);
  sLog_Probe("text.atlas",
             "build fontPx=" << params.fontPx << " pxRange=" << params.pxRange
             << " charset=" << params.charset.size()
             << " fontPath=" << params.fontPath);
  if (!m_chartTextAtlas.build(params)) {
    sLog_Warning("Chart text atlas build failed; chart text disabled: fontPath="
                 << params.fontPath << " resourceFont=" << params.resourceFont
                 << " fontPx=" << params.fontPx);
  } else {
    if (qEnvironmentVariableIsSet("SENTINEL_DUMP_GLYPH_ATLAS")) {
      m_chartTextAtlas.image().save("/tmp/sentinel_msdf_atlas.png");
      const MsdfAtlas::Glyph &glyph = m_chartTextAtlas.glyph(QChar('$'));
      if (!glyph.uv.isNull()) {
        const QImage &atlas = m_chartTextAtlas.image();
        const QRect cropRect(
            static_cast<int>(std::floor(glyph.uv.x() * atlas.width())),
            static_cast<int>(std::floor(glyph.uv.y() * atlas.height())),
            std::max(1, static_cast<int>(
                            std::ceil(glyph.uv.width() * atlas.width()))),
            std::max(1, static_cast<int>(
                            std::ceil(glyph.uv.height() * atlas.height()))));
        atlas.copy(cropRect.intersected(atlas.rect()))
            .save("/tmp/sentinel_msdf_glyph_dollar.png");
      }
    }
    m_chartTextAtlasBuilt = true;
  }
}

void UnifiedGridRenderer::applyClientConfig(const ClientConfig &config) {
  setHeatmapGamma(config.heatmap.gamma);
  setHeatmapContrast(config.heatmap.contrast);
  setHeatmapShaderFloor(config.heatmap.shaderFloor);
  syncGpuTone();
  m_view->initialViewSettings().columnPx = std::clamp(config.heatmap.initialColumnPx, 2, 64);
  m_view->initialViewSettings().pricePct = std::clamp(config.heatmap.initialPricePct, 0, 100);
  if (m_axisTextService) {
    m_axisTextService->setAxisLabelPxOverride(config.gui.axisLabelPx);
    m_axisTextService->refreshAxisLayout();
  }
  applyTpoConfig(config.tpo);
  onViewportChanged();
  update();
}

void UnifiedGridRenderer::applyServerConfig(const ServerConfig &config) {
  int64_t forcedTf = config.chart.defaultTimeframeMs;
  if (forcedTf <= 0 && !config.chart.timeframesMs.empty()) {
    forcedTf = config.chart.timeframesMs.front();
  }
  if (forcedTf > 0) setTimeframe(static_cast<int>(forcedTf));
}

QString UnifiedGridRenderer::getMsdfAtlasMemory() const {
  if (!m_chartTextAtlas.isBuilt()) {
    return "MSDF atlas: N/A";
  }
  const QImage &image = m_chartTextAtlas.image();
  if (image.isNull()) {
    return "MSDF atlas: N/A";
  }
  const double mb =
      static_cast<double>(image.sizeInBytes()) / (1024.0 * 1024.0);
  return QString("MSDF atlas: %1 MB").arg(mb, 0, 'f', 2);
}

void UnifiedGridRenderer::mousePressEvent(QMouseEvent *event) {
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

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent *event) {
  if (getViewState()) {
    getViewState()->handlePanMove(event->position());
    event->accept();
    update();
  } else
    event->ignore();
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent *event) {
  if (getViewState()) {
    endPanAt();
    event->accept();
  }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent *event) {
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
  wheelZoom(angle, event->pixelDelta().isNull(), event->phase(),
            event->source() == Qt::MouseEventSynthesizedBySystem,
            at.x(), at.y(), !shift, true, WheelRoute::Chart);
  update();
  event->accept();
}

qint64 UnifiedGridRenderer::getVisibleTimeStart() const {
  return getViewState() ? getViewState()->getVisibleTimeStart() : 0;
}
qint64 UnifiedGridRenderer::getVisibleTimeEnd() const {
  return getViewState() ? getViewState()->getVisibleTimeEnd() : 0;
}
double UnifiedGridRenderer::getMinPrice() const {
  return getViewState() ? getViewState()->getMinPrice() : 0.0;
}
double UnifiedGridRenderer::getMaxPrice() const {
  return getViewState() ? getViewState()->getMaxPrice() : 0.0;
}
QPointF UnifiedGridRenderer::getPanVisualOffset() const {
  return getViewState() ? getViewState()->getPanVisualOffset() : QPointF(0, 0);
}

MappingFrameContext UnifiedGridRenderer::currentFrameContext() const {
  return m_published.current();
}

TimeAxisMapping UnifiedGridRenderer::currentTimeAxisMapping() const {
  return currentFrameContext().mapping;
}

// The last frame's TimeAxisMapping (viewport columns at the drawn tick).
QString UnifiedGridRenderer::getViewportMathDebug() const {
  const TimeAxisMapping m = currentTimeAxisMapping();
  if (!m.valid) return "Viewport Math\nmapping: N/A";
  QStringList lines;
  lines << "Viewport Math"
        << QString("view.time: %1 → %2 (%3 ms)")
               .arg(static_cast<qint64>(m.viewStartMs))
               .arg(static_cast<qint64>(m.viewEndMs))
               .arg(static_cast<qint64>(std::max(0.0, m.viewEndMs - m.viewStartMs)))
        << QString("view.price: %1 → %2 (Δ%3)")
               .arg(m.viewMinPrice, 0, 'f', 4)
               .arg(m.viewMaxPrice, 0, 'f', 4)
               .arg(std::max(0.0, m.viewMaxPrice - m.viewMinPrice), 0, 'f', 4)
        << QString("tick: %1  grid: %2x%3  append: %4")
               .arg(m.tickSize, 0, 'f', 6)
               .arg(m.gridWidth)
               .arg(m.gridHeight)
               .arg(static_cast<qint64>(m.appendMs))
        << QString("drawRect: x%1 y%2 w%3 h%4")
               .arg(m.drawRect.x(), 0, 'f', 1)
               .arg(m.drawRect.y(), 0, 'f', 1)
               .arg(m.drawRect.width(), 0, 'f', 1)
               .arg(m.drawRect.height(), 0, 'f', 1)
        << QString("srcRect: x%1 y%2 w%3 h%4")
               .arg(m.srcRect.x(), 0, 'f', 2)
               .arg(m.srcRect.y(), 0, 'f', 2)
               .arg(m.srcRect.width(), 0, 'f', 2)
               .arg(m.srcRect.height(), 0, 'f', 2)
        << QString("cell: %1 x %2 px").arg(m.cellW, 0, 'f', 2).arg(m.cellH, 0, 'f', 2)
        << QString("dragging: %1").arg(getViewState() && getViewState()->isDragging() ? "yes" : "no");
  return lines.join('\n');
}

// The GPU layer's live and draw state (heatmap/state in the Agent API has it all).
QString UnifiedGridRenderer::getDataPipelineDebug() const {
  QStringList lines;
  lines << "Data Pipeline";
  if (!m_gpuLayer) {
    lines << "layer: N/A";
    return lines.join('\n');
  }
  const qint64 received = m_gpuLayer->liveReceivedAtMs();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  lines << QString("symbol: %1  tf: %2 ms  tick: %3")
               .arg(QString::fromStdString(m_gpuLayer->symbol()))
               .arg(m_gpuLayer->tfMs())
               .arg(m_gpuLayer->tickPrice(), 0, 'f', 6)
        << QString("live open end: %1  age: %2 ms")
               .arg(m_gpuLayer->liveOpenEndMs())
               .arg(received > 0 ? now - received : -1)
        << QString("controller: %1").arg(m_gpuLayer->controller() ? "yes" : "no");
  return lines.join('\n');
}
QString UnifiedGridRenderer::getPerformanceStats() const {
  return PerformanceMonitor::instance().frameStatsText();
}

void UnifiedGridRenderer::addTrade(const Trade &trade) {
  onTradeReceived(trade);
}
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) {
  m_view->zoomGlide().endZoomGlide(); // an explicit view lands any zoom glide or gesture
  m_view->glide().gesture = false;
  if (priceMax > priceMin) {
    // An explicit viewport (Agent API, QML) is a real price window: no book-top
    // seed replaces it.
    m_view->flags().priceKnown = true;
    m_view->flags().reseedPrice = false;
    m_priceCarry.reset();
  }
  if (getViewState() && (priceMin != getViewState()->getMinPrice() ||
                      priceMax != getViewState()->getMaxPrice())) {
    // An explicit price range turns follow-live and the auto price scale off (before
    // the viewport moves: the auto fit would replace it).
    getViewState()->setAutoPriceScale(false);
    getViewState()->enableAutoScroll(false);
  }
  if (getViewState()) {
    // Raster anchor: the centre of a moved axis; an unchanged axis keeps its anchor.
    const auto anchor = getViewState()->rasterAnchor();
    const bool timeMoved =
        timeStart != getViewState()->getVisibleTimeStart() || timeEnd != getViewState()->getVisibleTimeEnd();
    const bool priceMoved = priceMin != getViewState()->getMinPrice() || priceMax != getViewState()->getMaxPrice();
    getViewState()->setRasterAnchor(timeMoved && !getViewState()->isAutoScrollEnabled() ? 0.5 : anchor.fracX,
                                 priceMoved ? 0.5 : anchor.fracY);
  }
  onViewChanged(timeStart, timeEnd, priceMin, priceMax);
  m_view->syncGpuView(); // also when the values were unchanged (price now known)
}
void UnifiedGridRenderer::zoomIn() { zoomClicks(1, width() / 2, height() / 2); }
void UnifiedGridRenderer::zoomOut() { zoomClicks(-1, width() / 2, height() / 2); }
void UnifiedGridRenderer::resetZoom() {
  if (getViewState()) {
    getViewState()->resetZoom();
    update();
  }
}
void UnifiedGridRenderer::beginPanAt(double x, double y) {
  if (!getViewState() || !isVisible()) {
    return;
  }
  if (getViewState()->isAutoScrollEnabled()) {
    getViewState()->enableAutoScroll(false);
    sLog_Render("auto-scroll enabled=false reason=pan");
  }
  getViewState()->handlePanStart(QPointF(x, y));
  update();
}
void UnifiedGridRenderer::updatePanAt(double x, double y) {
  if (!getViewState() || !isVisible()) {
    return;
  }
  getViewState()->handlePanMove(QPointF(x, y));
  update();
}
void UnifiedGridRenderer::endPanAt() {
  if (!getViewState()) {
    return;
  }
  // A zoom glide in flight: its start moves with the committed whole pixels too.
  const bool dragging = getViewState()->isDragging();
  const QPointF drag = getViewState()->getPanVisualOffset();
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  getViewState()->handlePanEnd(true);
  if (dragging) {
    const double dx = double(std::llround(drag.x() * dpr)), dy = double(std::llround(drag.y() * dpr));
    m_view->zoomGlide().shiftBoth(dx, dy);
  }
  update();
}
void UnifiedGridRenderer::panLeft() {
  if (getViewState()) {
    getViewState()->panLeft();
    update();
  }
}
void UnifiedGridRenderer::panRight() {
  if (getViewState()) {
    getViewState()->panRight();
    update();
  }
}
void UnifiedGridRenderer::panUp() {
  if (getViewState()) {
    getViewState()->panUp();
    update();
  }
}
void UnifiedGridRenderer::panDown() {
  if (getViewState()) {
    getViewState()->panDown();
    update();
  }
}
