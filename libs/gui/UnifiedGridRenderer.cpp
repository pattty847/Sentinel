// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "PerformanceMonitor.hpp"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include "config/GuiConfigStore.hpp"
#include "datasources/CandleSeriesBuffer.hpp"
#include "render/DataProcessor.hpp"
#include "render/GridViewState.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "../core/servermodel/SessionManager.hpp"
#include <QDateTime>
#include <tuple>
#include <QMetaObject>
#include <algorithm>
#include <cmath>
#include <QMetaType>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>


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

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem *parent)
    : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setFlag(ItemAcceptsInputMethod, true);

  setAcceptHoverEvents(false); // Reduce event capture
  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* w) { bindWindow(w); });

  init();
  // An item constructed with a parent already in a window (C++ hosts, tests)
  // saw windowChanged before the connection above.
  if (window()) bindWindow(window());
}

void UnifiedGridRenderer::bindWindow(QQuickWindow* w) {
  if (m_axisTextService) {
    m_axisTextService->bindAxisLayoutWindow(w);
  }
  syncGpuSurface(); // the device pixel ratio sets the 1 column/px clamp
  if (!w) return;
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
    // only if the labels would now draw differently (transition-driven, S7b).
    if (m_gpuLabelsIncomplete.exchange(false))
      QMetaObject::invokeMethod(this, [this] {
        if (m_gpuLayer && m_gpuLayer->labelSignature() != m_gpuLabelSignature.load()) update();
      }, Qt::QueuedConnection);
  }, Qt::DirectConnection);
}


UnifiedGridRenderer::~UnifiedGridRenderer() {
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
  if (m_activeSymbol == QLatin1String(trade.product_id.data(), qsizetype(trade.product_id.size()))) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    if (m_tradeBubbleTape->append({ms, trade.price, trade.size, trade.side, trade.trade_id}) && m_showTrades)
      update(); // Qt coalesces trade bursts into one scene synchronization
  }
  // The live price for auto-fit (no allocation per trade: product ids are ASCII).
  if (std::isfinite(trade.price) && trade.price > 0 &&
      m_activeSymbol == QLatin1String(trade.product_id.data(), static_cast<qsizetype>(trade.product_id.size())))
    m_gpuLastTrade = trade.price;
  // A trade seeds the price window only when no book top has (yet) for this symbol.
  if ((m_gpuReseedPrice || !m_gpuPriceKnown) && QString::fromStdString(trade.product_id) == m_activeSymbol)
    seedGpuViewport(trade.price, trade.price);
}

void UnifiedGridRenderer::setLiveBookTop(double bestBid, double bestAsk) {
  // The book top seeds the first viewport (and the price window after a symbol switch).
  if (std::isfinite(bestBid) && std::isfinite(bestAsk) && bestBid > 0 && bestAsk >= bestBid)
    m_gpuBookMid = (bestBid + bestAsk) * 0.5;
  seedGpuViewport(bestBid, bestAsk);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs,
                                        double minPrice, double maxPrice) {
  if (m_viewState) {
    m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
  }

  update();

  sLog_Probe("viewport.view",
             "t=[" << startTimeMs << ".." << endTimeMs << "]"
             << " price=[" << minPrice << ".." << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
  if (!m_viewState) return;
  update();
  if (!m_gpuSelfViewport) m_gpuViewPristine = false; // someone else moved the view
  // The controller plans from the committed view.
  syncGpuView();
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

    if (m_viewState) {
      m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
    }
    if (m_axisTextService) {
      m_axisTextService->refreshAxisLayout();
    }
    syncGpuSurface();
    if (m_gpuViewPristine && m_viewState && m_viewState->isTimeWindowValid() &&
        newGeometry.width() > 0) {
      // Untouched seeded view: keep initial_column_px on the laid-out width.
      const qint64 end = m_viewState->getVisibleTimeEnd();
      setGpuViewportSelf(end - gpuInitialSpanMs(newGeometry.width()), end, m_viewState->getMinPrice(),
                         m_viewState->getMaxPrice());
    }
    update();
  }
}

void UnifiedGridRenderer::componentComplete() {
  QQuickItem::componentComplete();

  if (m_axisTextService) {
    m_axisTextService->bindAxisLayoutWindow(window());
  }
  if (m_viewState && width() > 0 && height() > 0) {
    m_viewState->setViewportSize(width(), height());
  }
  if (m_axisTextService) {
    m_axisTextService->refreshAxisLayout();
  }
}

void UnifiedGridRenderer::setMinVolumeFilter(double minVolume) {
  if (m_minVolumeFilter != minVolume) {
    m_minVolumeFilter = minVolume;
    update();
    emit minVolumeFilterChanged();
  }
}

void UnifiedGridRenderer::setAutoScrollPaddingFrac(double fraction) {
  const double clamped = std::clamp(fraction, 0.0, 0.45);
  if (m_autoScrollPaddingFrac != clamped) {
    m_autoScrollPaddingFrac = clamped;
    emit autoScrollPaddingFracChanged();
  }
}

void UnifiedGridRenderer::setAutoScrollSmoothEnabled(bool enabled) {
  if (m_smoothAutoScrollEnabled != enabled) {
    m_smoothAutoScrollEnabled = enabled;
    emit autoScrollSmoothEnabledChanged();
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

void UnifiedGridRenderer::setShowRenderStrategyOverlay(bool show) {
  if (m_showRenderStrategyOverlay != show) {
    m_showRenderStrategyOverlay = show;
    emit showRenderStrategyOverlayChanged();
  }
}

void UnifiedGridRenderer::setShowViewportMathOverlay(bool show) {
  if (m_showViewportMathOverlay != show) {
    m_showViewportMathOverlay = show;
    emit showViewportMathOverlayChanged();
  }
}

void UnifiedGridRenderer::setShowMemoryCacheOverlay(bool show) {
  if (m_showMemoryCacheOverlay != show) {
    m_showMemoryCacheOverlay = show;
    emit showMemoryCacheOverlayChanged();
  }
}

void UnifiedGridRenderer::setShowModeFlagsOverlay(bool show) {
  if (m_showModeFlagsOverlay != show) {
    m_showModeFlagsOverlay = show;
    emit showModeFlagsOverlayChanged();
  }
}

void UnifiedGridRenderer::clearData() {
  m_tradeBubbleTape->clear();
  if (m_viewState) {
    m_viewState->resetZoom();
  }
  if (m_dataProcessor) {
    QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::clearData,
                              Qt::QueuedConnection);
  }
  m_footprintOverlay.clearPending();
  m_vpRenderer.clearPending();
  m_footprintOverlay.requestNeutralReset();
  m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
  update();
}

void UnifiedGridRenderer::setActiveSymbol(const QString& symbol) {
  const QString normalized = symbol.trimmed().toUpper();
  if (m_activeSymbol == normalized) {
    return;
  }
  sLog_Render("active symbol changed, clearing chart data: prev=" << m_activeSymbol
              << " symbol=" << normalized);
  // Auto price scale off: carry the zoom as a fraction of the price, with the current
  // price at the same height, onto the new symbol's first price (seedGpuViewport).
  // A symbol that never got its own price (m_gpuReseedPrice) still shows an earlier
  // symbol's bounds: its pending carry is kept, not recomputed against them.
  if (!m_viewState || m_viewState->autoPriceScale() || !m_gpuReseedPrice) m_priceCarry.reset();
  if (m_gpuPriceKnown && !m_gpuReseedPrice && m_viewState && m_viewState->isTimeWindowValid() &&
      !m_viewState->autoPriceScale()) {
    double now = gpuLivePrice();
    if (!(now > 0) && m_candleBuffer) {
      // No book, trade or decoded price: the newest visible candle's close.
      const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
      m_candleBuffer->getVisibleSlice(m_activeSymbol, std::max<int64_t>(1, (tf + 500) / 1000),
                                      recording::floorDiv(m_viewState->getVisibleTimeStart(), tf) * tf,
                                      m_viewState->getVisibleTimeEnd(), m_fitBars);
      for (auto it = m_fitBars.rbegin(); it != m_fitBars.rend() && !(now > 0); ++it)
        if (std::isfinite(it->close) && it->close > 0) now = it->close;
    }
    const double lo = m_viewState->getMinPrice(), span = m_viewState->getMaxPrice() - lo;
    const PriceCarry carry{span / now, (now - lo) / span};
    if (now > 0 && span > 0 && std::isfinite(carry.spanRatio) && carry.spanRatio > 0 && std::isfinite(carry.heightFrac))
      m_priceCarry = carry;
  }
  m_activeSymbol = normalized;
  m_gpuBookMid = m_gpuLastTrade = 0.0;
  // The controller serial switches the heatmap (the node holds the old picture
  // until the new spans are ready); clearData() resets the trade overlays. The
  // next book top centres the new price.
  if (m_carryWaitTimer) m_carryWaitTimer->stop(); // the live-only wait is per symbol
  // Reseed first: the layer's limitsChanged (setSymbol) must not apply a pending carry
  // before the new symbol has a price of its own.
  m_gpuReseedPrice = true;
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

void UnifiedGridRenderer::setPriceResolution(double resolution) {
  if (m_dataProcessor && resolution > 0) {
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        [this, resolution]() {
          m_dataProcessor->setPriceResolution(resolution);
        },
        Qt::QueuedConnection);
    update();
  }
}

void UnifiedGridRenderer::setGridResolutionPreset(int preset) {
  const double priceRes[] = {2.5, 5.0, 10.0};
  const int timeRes[] = {50, 100, 250};
  if (preset >= 0 && preset <= 2) {
    setPriceResolution(priceRes[preset]);
    setTimeframe(timeRes[preset]);
  }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
  if (m_currentTimeframe_ms != timeframe_ms) {
    sLog_Render("timeframe changed: prevMs=" << m_currentTimeframe_ms
                << " tfMs=" << timeframe_ms);
    const int64_t previousTf = m_currentTimeframe_ms;
    m_currentTimeframe_ms = timeframe_ms;
    if (timeframe_ms > 0) m_timeAuthority.setActiveTimeframeMs(timeframe_ms);
    m_manualTimeframeSet = true;
    m_manualTimeframeTimer.start();
    // Spec rule 1: a column is the timeframe, and a longer one is how to see further
    // back. Keep the columns on screen (S6d: keeping the time span gave four 1h columns
    // after 1m, and 1,600 hairline 1m columns after 1h): scale the span by the timeframe
    // ratio about the view end, inside the new timeframe's 1 column/px limit. previousTf >= 1 s skips the 100 ms
    // default before the server advertises its timeframe. The span is read before the new
    // limits apply (72 h of 1h is 72 columns of 1m, not 30.8 h of the clamp then /60), and
    // limits, scaled span and follow-live end are published as ONE viewport change: the
    // layer's limitsChanged is held back here, so the old view is never re-clamped first.
    const bool keepColumns = m_gpuLayer && timeframe_ms > 0 && previousTf >= 1000 && m_viewState &&
                             m_viewState->isTimeWindowValid();
    const int64_t oldStart = keepColumns ? m_viewState->getVisibleTimeStart() : 0;
    const int64_t oldEnd = keepColumns ? m_viewState->getVisibleTimeEnd() : 0;
    const double scaledSpan = keepColumns ? static_cast<double>(oldEnd - oldStart) * static_cast<double>(timeframe_ms) /
                                                static_cast<double>(previousTf)
                                          : 0.0;
    // Viewport model (docs/research/2026-10-viewport-autoscale.md): the Now column (the
    // live bucket) keeps its screen x when it is in view or the view follows live; a
    // historical view keeps its end. rightFrac: the view's part right of the Now centre.
    const int64_t anchor = keepColumns ? m_gpuLayer->liveAnchorMs() : 0;
    auto nowCentre = [anchor](int64_t tf) {
      return static_cast<double>(recording::floorDiv(anchor + tf - 1, tf) * tf) - static_cast<double>(tf) * 0.5;
    };
    std::optional<double> rightFrac;
    if (anchor > 0 && oldEnd > oldStart) {
      const double now = nowCentre(previousTf);
      if (m_viewState->isAutoScrollEnabled() || (now >= double(oldStart) && now <= double(oldEnd)))
        rightFrac = (static_cast<double>(oldEnd) - now) / static_cast<double>(oldEnd - oldStart);
    }
    if (m_gpuLayer && timeframe_ms > 0) {
      m_gpuLimitsDeferred = keepColumns;
      m_gpuLayer->setTimeframeMs(timeframe_ms); // new limits (applied below when keepColumns)
      m_gpuLimitsDeferred = false;
    }
    if (keepColumns) {
      const double maxTime = m_gpuLayer->maxTimeSpanMs();
      int64_t span = std::max<int64_t>(1, static_cast<int64_t>(std::llround(scaledSpan)));
      if (maxTime > 0) span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))));
      const int64_t end =
          rightFrac ? static_cast<int64_t>(std::llround(nowCentre(timeframe_ms) + *rightFrac * double(span))) : oldEnd;
      // Price: auto price scale off keeps it; on, setViewport fits the new timeframe's
      // candles held so far (later pages refit through candlesDirty). setViewport
      // applies the new max price span.
      m_viewState->setMinSpans(m_gpuLayer->minTimeSpanMs(), m_gpuLayer->minPriceSpan());
      m_viewState->setViewportAndMaxSpans(end - span, end, m_viewState->getMinPrice(), m_viewState->getMaxPrice(),
                                          maxTime, m_gpuLayer->maxPriceSpan());
      syncGpuView();
      if (m_viewState->isAutoScrollEnabled()) emit liveRenderTick();
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
  if (!m_viewState) return;
  m_viewState->enableAutoScroll(enabled);
  if (enabled) returnGpuToLive(); // back to the live edge from history or the future, same span
  update();
  emit autoScrollEnabledChanged();
  sLog_Render("auto-scroll enabled=" << enabled << " reason=request");
}


// ── GPU heatmap renderer (S6b) ───────────────────────────────────────────────
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
  applyGpuLimits();
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
}

// Spec rules 1, 2 and 9: GridViewState clamps wheel, axis drags and the Agent
// API identically.
void UnifiedGridRenderer::applyGpuLimits() {
  if (!m_viewState || m_gpuLimitsDeferred || !m_gpuLayer) return;
  m_viewState->setMinSpans(m_gpuLayer->minTimeSpanMs(), m_gpuLayer->minPriceSpan());
  m_viewState->setMaxSpans(m_gpuLayer->maxTimeSpanMs(), m_gpuLayer->maxPriceSpan());
}

void UnifiedGridRenderer::syncGpuView() {
  if (!m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid()) return;
  heatmap::gpu::ViewWindow view{static_cast<double>(m_viewState->getVisibleTimeStart()),
                                static_cast<double>(m_viewState->getVisibleTimeEnd()), m_viewState->getMinPrice(),
                                m_viewState->getMaxPrice()};
  m_gpuLayer->setView(view, m_gpuPriceKnown);
}

// Follow-live from LiveSnapshot::openEndMs: the view end stays one padding past
// the live bucket's end.
void UnifiedGridRenderer::followGpuLive() {
  if (!m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() ||
      !m_viewState->isAutoScrollEnabled() || m_viewState->isDragging())
    return;
  const int64_t openEnd = m_gpuLayer->liveOpenEndMs();
  const int64_t tf = m_currentTimeframe_ms;
  if (openEnd <= 0 || tf <= 0) return;
  const int64_t start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  const int64_t span = end - start;
  const int64_t liveEnd = recording::floorDiv(openEnd + tf - 1, tf) * tf;
  const int64_t pad = std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * m_autoScrollPaddingFrac));
  const int64_t target = liveEnd + pad;
  if (target <= end) return; // the live bucket is inside the padded view
  const int64_t shift = target - end;
  setGpuViewportSelf(start + shift, end + shift, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
  emit liveRenderTick();
}

// Cold start (plan section 2): the first book top places the first viewport and
// the view is requested at once; the node draws the loading hatch until the newest
// span lands. The price window uses initial_price_pct of kSeedBandRows rows at
// kSeedBandTick.
void UnifiedGridRenderer::seedGpuViewport(double bestBid, double bestAsk) {
  if (!m_viewState || !std::isfinite(bestBid) || !std::isfinite(bestAsk) || bestBid <= 0 || bestAsk < bestBid) return;
  const bool timeValid = m_viewState->isTimeWindowValid();
  if (timeValid && m_gpuPriceKnown && !m_gpuReseedPrice) return;
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
  qint64 start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  if (!timeValid) {
    const int64_t span = gpuInitialSpanMs(width());
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    m_gpuViewPristine = true;
    end = recording::floorDiv(now, tf) * tf + tf +
          std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * m_autoScrollPaddingFrac));
    start = end - span;
  }
  const int pct = m_initialPricePct;
  const double full = kSeedBandTick * kSeedBandRows;
  const double span = pct > 0 && pct < 100 ? full * pct / 100.0 : full;
  const double mid = (bestBid + bestAsk) * 0.5;
  const double lo = mid - span * 0.5, hi = mid + span * 0.5;
  const bool carry = m_priceCarry && timeValid && m_gpuReseedPrice;
  if (!carry) m_priceCarry.reset();
  m_gpuPriceKnown = true;
  m_gpuReseedPrice = false;
  sLog_Render("GPU heatmap viewport seeded from the book top: mid=" << mid << " time=[" << start << ".." << end
              << "] carry=" << carry << " autoPriceScale=" << autoPriceScale());
  // A symbol switch with auto price scale off: the carried zoom; else the default band
  // (auto price scale on: the visible candles win inside setViewport).
  if (!carry || !applyPriceCarry(mid)) setGpuViewportSelf(start, end, lo, hi);
  syncGpuView(); // also when the viewport did not change (priceKnown flips)
  update();
}

bool UnifiedGridRenderer::applyPriceCarry(double now, bool liveOnly) {
  if (!m_priceCarry || !m_viewState || !(now > 0) || !std::isfinite(now)) return false;
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
        if (m_gpuLayer && m_priceCarry && !m_gpuReseedPrice && !m_gpuLayer->priceScaleCurrent())
          applyPriceCarry(gpuLivePrice(), true);
      });
    }
    if (!m_carryWaitTimer->isActive()) m_carryWaitTimer->start(kCarryLiveOnlyWaitMs);
  }
  setGpuViewportSelf(m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), lo, hi);
  return true;
}

qint64 UnifiedGridRenderer::gpuLiveEndMs(qint64 spanMs) const {
  const int64_t anchor = m_gpuLayer ? m_gpuLayer->liveAnchorMs() : 0;
  const int64_t tf = m_currentTimeframe_ms;
  if (anchor <= 0 || tf <= 0) return 0;
  const int64_t liveEnd = recording::floorDiv(anchor + tf - 1, tf) * tf;
  return liveEnd + std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(spanMs) * m_autoScrollPaddingFrac));
}

void UnifiedGridRenderer::returnGpuToLive() {
  if (!m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid()) return;
  const int64_t span = m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart();
  const int64_t end = gpuLiveEndMs(span);
  if (end <= 0) return; // nothing known yet: the next live frame steps forward
  sLog_Render("GPU heatmap returns to live: anchor=" << m_gpuLayer->liveAnchorMs() << " end=" << end);
  m_viewState->setViewport(end - span, end, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
  syncGpuView(); // publishes the view even when it was already there
  emit liveRenderTick();
}

void UnifiedGridRenderer::bootstrapGpuTimeView() {
  if (!m_gpuLayer || !m_viewState || m_viewState->isTimeWindowValid()) {
    if (m_gpuBootstrapTimer) m_gpuBootstrapTimer->stop();
    return;
  }
  const int64_t anchor = m_gpuLayer->liveAnchorMs();
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
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
                      std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * m_autoScrollPaddingFrac));
  sLog_Render("GPU heatmap time-only bootstrap from availability: anchor=" << anchor << " time=[" << end - span
              << ".." << end << "] (price from the first decoded data)");
  m_gpuPriceKnown = false;
  m_gpuViewPristine = true;
  setGpuViewportSelf(end - span, end, 0.0, 1.0); // placeholder price: nothing draws until it is known
  syncGpuView();
}

// The first view's time span: initial_column_px per column (16 columns min).
qint64 UnifiedGridRenderer::gpuInitialSpanMs(double widthPx) const {
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
  const double width = widthPx > 0.0 ? widthPx : 800.0;
  return static_cast<int64_t>(std::max(16, static_cast<int>(width / m_initialColumnPx))) * tf;
}

void UnifiedGridRenderer::setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax) {
  m_gpuSelfViewport = true;
  m_viewState->setViewport(start, end, priceMin, priceMax);
  m_gpuSelfViewport = false;
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
                                  if (!m_viewState || !m_viewState->autoPriceScale() ||
                                      !m_viewState->isTimeWindowValid() || symbol != m_activeSymbol)
                                    return;
                                  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
                                  if (timeframeSec != std::max<int64_t>(1, (tf + 500) / 1000)) return;
                                  // The window on screen (a drag commits only at release).
                                  const auto [shownStart, shownEnd] = m_viewState->displayedTimeWindow();
                                  const qint64 viewStart = recording::floorDiv(shownStart, tf) * tf;
                                  if (std::max(dirtyEnd, dirtyStart + tf) <= viewStart || dirtyStart >= shownEnd)
                                    return;
                                  refitAutoPrice();
                                });
  }
  refitAutoPrice();
  emit candleBufferChanged();
}

double UnifiedGridRenderer::gpuLivePrice() const {
  if (m_gpuBookMid > 0) return m_gpuBookMid;
  if (m_gpuLastTrade > 0) return m_gpuLastTrade;
  return m_gpuLayer ? m_gpuLayer->recentMidPrice() : 0.0;
}

std::optional<std::pair<qint64, qint64>> UnifiedGridRenderer::gpuFitTimeWindow() const {
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
  const int64_t anchor = m_gpuLayer->liveAnchorMs();
  if (anchor <= 0) return std::nullopt;
  int64_t oldest = m_gpuLayer->oldestAvailableMs();
  if (oldest <= 0 && m_candleBuffer)
    oldest = m_candleBuffer->oldestTimeMs(m_activeSymbol, std::max<int64_t>(1, (tf + 500) / 1000));
  const int64_t dataEnd = recording::floorDiv(anchor + tf - 1, tf) * tf; // the live bucket's end
  const int64_t dataStart = oldest > 0 ? recording::floorDiv(oldest, tf) * tf : 0;
  const double maxTime = m_viewState->maxTimeSpanMs();
  const int64_t maxSpan = maxTime > 0 ? std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))) : INT64_MAX / 4;
  const int64_t minSpan = std::max<int64_t>(tf, static_cast<int64_t>(std::ceil(m_viewState->minTimeSpanMs())));
  const double pad = m_autoScrollPaddingFrac;
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
  const bool following = m_viewState->isAutoScrollEnabled();
  if (fits || following || dataStart <= 0) {
    const int64_t end = gpuLiveEndMs(span);
    if (end <= 0) return std::nullopt;
    return std::make_pair<qint64, qint64>(end - span, qint64(end));
  }
  // History that does not fit: keep the view centre, inside the data.
  const int64_t centre = m_viewState->getVisibleTimeStart() +
                         (m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart()) / 2;
  const int64_t lastStart = gpuLiveEndMs(span) - span;
  const int64_t start = std::clamp<int64_t>(centre - span / 2, dataStart, std::max(dataStart, lastStart));
  return std::make_pair<qint64, qint64>(qint64(start), start + span);
}

std::optional<std::pair<double, double>> UnifiedGridRenderer::gpuFitPriceWindow(qint64 start, qint64 end,
                                                                                bool candlesOnly) const {
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
  double lo = 0, hi = 0, centre = 0, span = 0;
  // From the bucket that contains the view start: getVisibleSlice selects by bar
  // start, and the candle that starts before the view and ends inside it counts.
  const int64_t alignedStart = recording::floorDiv(start, tf) * tf;
  auto& bars = m_fitBars; // reused: the auto price scale runs this on every change in view
  bars.clear();
  if (m_candleBuffer && end > start)
    m_candleBuffer->getVisibleSlice(m_activeSymbol, std::max<int64_t>(1, (tf + 500) / 1000), alignedStart, end, bars);
  size_t used = 0;
  double newestClose = 0;
  qint64 newestStart = 0;
  for (const auto& bar : bars) {
    const qint64 barEnd = bar.timeEndMs > bar.timeStartMs ? bar.timeEndMs : bar.timeStartMs + tf;
    if (bar.timeStartMs >= end || barEnd <= start || !std::isfinite(bar.high) || !std::isfinite(bar.low) ||
        !(bar.low > 0) || bar.high < bar.low)
      continue;
    lo = used ? std::min(lo, bar.low) : bar.low;
    hi = used ? std::max(hi, bar.high) : bar.high;
    if (!used || bar.timeStartMs >= newestStart) {
      newestStart = bar.timeStartMs;
      newestClose = std::isfinite(bar.close) && bar.close > 0 ? bar.close : (bar.low + bar.high) * 0.5;
    }
    ++used;
  }
  const double current = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
  if (used) {
    centre = (lo + hi) * 0.5;
    span = hi > lo ? (hi - lo) * (1.0 + 2.0 * kFitPriceMargin) : current;
  } else {
    if (candlesOnly) return std::nullopt;
    centre = gpuLivePrice();
    span = current;
  }
  if (!(centre > 0) || !std::isfinite(centre)) return std::nullopt;
  if (!(span > 0) || !std::isfinite(span) || span > centre * 4) span = centre * 0.01; // e.g. a span from another symbol
  // A minimum span wider than the price itself is another symbol's (the layer's limits
  // follow the new symbol's data a moment after a switch): not applied.
  if (const double minSpan = m_viewState->minPriceSpan(); minSpan < centre) span = std::max(span, minSpan);
  const double maxSpan = m_viewState->maxPriceSpan();
  if (maxSpan > 0 && span > maxSpan) {
    // Manual tick (owner decision 2026-10-02): the user's tick stays (never Auto,
    // never coarsened); the candles need more than one row per pixel, so show what
    // the limit can, centred on the current price: book mid, last trade, newest close.
    span = maxSpan;
    if (used) {
      const double now = m_gpuBookMid > 0 ? m_gpuBookMid : m_gpuLastTrade > 0 ? m_gpuLastTrade : newestClose;
      if (now > 0) centre = now;
    }
  }
  return std::make_pair(centre - span * 0.5, centre + span * 0.5);
}

bool UnifiedGridRenderer::autoPriceFit(qint64 start, qint64 end, double& priceMin, double& priceMax) {
  if (!m_gpuLayer || !m_viewState) return false;
  const auto window = gpuFitPriceWindow(start, end, true);
  if (!window) return false; // no visible candle: the given price stays
  std::tie(priceMin, priceMax) = *window;
  m_gpuPriceKnown = true; // a real price window (no book-top seed replaces it)
  m_gpuReseedPrice = false;
  m_priceCarry.reset();
  return true;
}

void UnifiedGridRenderer::refitAutoPrice() {
  if (!m_viewState || !m_viewState->autoPriceScale() || !m_viewState->isTimeWindowValid() ||
      m_gpuLimitsDeferred)
    return;
  const bool known = m_gpuPriceKnown;
  setGpuViewportSelf(m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), m_viewState->getMinPrice(),
                     m_viewState->getMaxPrice());
  if (m_gpuPriceKnown != known) syncGpuView();
}

void UnifiedGridRenderer::setAutoPriceScale(bool enabled) {
  if (!m_viewState) return;
  if (!enabled) {
    m_viewState->setAutoPriceScale(false); // the price range stays where it is
    return;
  }
  // On = the price-axis double-click: fit now (candles, else the live price); with
  // nothing to fit yet it is on and the next change in view fits.
  if (!fitView(false, true)) m_viewState->setAutoPriceScale(true);
}

bool UnifiedGridRenderer::fitView(bool time, bool price) {
  if (!m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() || (!time && !price) ||
      m_viewState->isDragging()) {
    sLog_Render("view fit skipped: time=" << time << " price=" << price);
    return false;
  }
  qint64 start = m_viewState->getVisibleTimeStart(), end = m_viewState->getVisibleTimeEnd();
  double lo = m_viewState->getMinPrice(), hi = m_viewState->getMaxPrice();
  // All or nothing: every requested part must fit, else nothing changes (a failed fit
  // leaves the view, follow-live and the auto price scale as they were).
  bool ok = true;
  if (time) {
    if (const auto window = gpuFitTimeWindow()) std::tie(start, end) = *window;
    else ok = false;
  }
  if (price && ok) {
    // Candles, else the live price (setViewport's auto fit takes the candles first).
    if (const auto window = gpuFitPriceWindow(start, end, false)) std::tie(lo, hi) = *window;
    else ok = false;
  }
  sLog_Render("view fit time=" << time << " price=" << price << " applied=" << ok << " time=[" << start << ".."
              << end << "] price=[" << lo << ".." << hi << "]");
  if (!ok) return false;
  if (price) {
    m_gpuPriceKnown = true;
    m_gpuReseedPrice = false;
    m_viewState->setAutoPriceScale(true); // a price fit is the auto price scale
  }
  m_viewState->setViewport(start, end, lo, hi); // one change; follow-live is kept
  syncGpuView();
  update();
  return true;
}

QString UnifiedGridRenderer::applyViewportRequest(const ViewportRequest& request) {
  if (!m_viewState) return QStringLiteral("viewport_unavailable");
  if (!request.fit.isEmpty()) {
    const bool time = request.fit != "price", price = request.fit != "time";
    const bool ok = request.fit == "default" ? resetView() : fitView(time, price);
    return ok ? QString() : QStringLiteral("fit_unavailable");
  }
  const bool explicitPrice = request.priceMin && request.priceMax;
  const bool explicitTime = request.startMs && request.endMs;
  const qint64 start0 = m_viewState->getVisibleTimeStart(), end0 = m_viewState->getVisibleTimeEnd();
  // The flags first (no viewport change of their own), then one commit.
  if (explicitPrice) {
    m_viewState->setAutoPriceScale(false); // explicit bounds are the user's price, equal or not
    m_priceCarry.reset();
  } else if (request.autoScale) {
    m_viewState->setAutoPriceScale(*request.autoScale);
  }
  const bool follow = (explicitTime || explicitPrice) ? false : request.followLive.value_or(autoScrollEnabled());
  m_viewState->enableAutoScroll(follow);
  qint64 start = request.startMs.value_or(start0), end = request.endMs.value_or(end0);
  if (follow && !explicitTime) {
    // Follow-live on: the live edge one padding inside, span kept (returnGpuToLive).
    const qint64 span = end - start;
    if (const qint64 liveEnd = gpuLiveEndMs(span); liveEnd > 0) {
      end = liveEnd;
      start = liveEnd - span;
    }
  }
  double lo = request.priceMin.value_or(m_viewState->getMinPrice());
  double hi = request.priceMax.value_or(m_viewState->getMaxPrice());
  if (explicitPrice) {
    m_gpuPriceKnown = true;
    m_gpuReseedPrice = false;
  } else if (request.autoScale.value_or(false)) {
    // On: the price-axis double-click (candles; with none, the live price centred).
    if (const auto window = gpuFitPriceWindow(start, end, false)) {
      std::tie(lo, hi) = *window;
      m_gpuPriceKnown = true;
      m_gpuReseedPrice = false;
    }
  }
  sLog_Render("viewport request: time=[" << start << ".." << end << "] price=[" << lo << ".." << hi
              << "] follow=" << follow << " autoPriceScale=" << m_viewState->autoPriceScale());
  m_viewState->setViewport(start, end, lo, hi); // one change (the auto fit applies inside)
  syncGpuView();
  if (follow) emit liveRenderTick();
  update();
  return {};
}

bool UnifiedGridRenderer::resetView() {
  if (!m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() ||
      m_viewState->isDragging())
    return false;
  int64_t span = gpuInitialSpanMs(width());
  if (const double maxTime = m_viewState->maxTimeSpanMs(); maxTime > 0)
    span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))));
  const int64_t end = gpuLiveEndMs(span);
  if (end <= 0) {
    sLog_Render("view reset skipped: no live anchor yet");
    return false;
  }
  m_viewState->setAutoPriceScale(true);
  m_viewState->enableAutoScroll(true); // the view state's flag only: the viewport moves once, below
  double lo = m_viewState->getMinPrice(), hi = m_viewState->getMaxPrice();
  if (const auto window = gpuFitPriceWindow(end - span, end, false)) {
    std::tie(lo, hi) = *window;
    m_gpuPriceKnown = true;
    m_gpuReseedPrice = false;
  }
  sLog_Render("view reset to default: time=[" << end - span << ".." << end << "] price=[" << lo << ".." << hi << "]");
  setGpuViewportSelf(end - span, end, lo, hi);
  m_gpuViewPristine = true; // the default span follows the chart's width again
  syncGpuView();
  emit liveRenderTick();
  update();
  return true;
}

//  COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms,
                                           double price) const {
  if (!m_viewState)
    return QPointF();

  Viewport viewport;
  viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
  viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
  viewport.priceMin = m_viewState->getMinPrice();
  viewport.priceMax = m_viewState->getMaxPrice();
  viewport.width = width();
  viewport.height = height();
  return CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
}

QPointF UnifiedGridRenderer::screenToWorld(double screenX,
                                           double screenY) const {
  if (!m_viewState)
    return QPointF();

  Viewport viewport;
  viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
  viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
  viewport.priceMin = m_viewState->getMinPrice();
  viewport.priceMax = m_viewState->getMaxPrice();
  viewport.width = width();
  viewport.height = height();
  return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
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
  m_initialColumnPx = std::clamp(config.heatmap.initialColumnPx, 2, 64);
  m_initialPricePct = std::clamp(config.heatmap.initialPricePct, 0, 100);
  if (m_axisTextService) {
    m_axisTextService->setAxisLabelPxOverride(config.gui.axisLabelPx);
    m_axisTextService->refreshAxisLayout();
  }
  applyTpoConfig(config.tpo);
  onViewportChanged();
  update();
}

void UnifiedGridRenderer::applyServerConfig(const ServerConfig &config) {
  int64_t forcedTf = config.heatmap.activeTimeframeMs;
  if (forcedTf <= 0 && !config.heatmap.timeframesMs.empty()) {
    forcedTf = config.heatmap.timeframesMs.front();
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
  if (m_viewState && isVisible() && event->button() == Qt::LeftButton) {
    if (m_viewState->isAutoScrollEnabled()) {
      m_viewState->enableAutoScroll(false);
      sLog_Render("auto-scroll enabled=false reason=mouse_pan");
    }
    m_viewState->handlePanStart(event->position());
    event->accept();
  } else
    event->ignore();
}

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent *event) {
  if (m_viewState) {
    m_viewState->handlePanMove(event->position());
    event->accept();
    update();
  } else
    event->ignore();
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent *event) {
  if (m_viewState) {
    m_viewState->handlePanEnd(true);
    event->accept();
    update();
  }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent *event) {
  if (m_viewState && isVisible() && m_viewState->isTimeWindowValid() &&
      (event->modifiers() & Qt::ShiftModifier)) {
    // Shift+wheel scales price only (spec rule 2, as the lab); macOS delivers it
    // as a horizontal delta, so whichever axis moved is used.
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    if (delta != 0) m_viewState->handlePriceZoomWithSensitivity(delta, event->position().y(), height());
    update();
    event->accept();
    return;
  }
  if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) {
    m_viewState->handleZoomWithSensitivity(
        event->angleDelta().y(), event->position(), QSizeF(width(), height()));
    update();
    event->accept();
  } else
    event->ignore();
}

int UnifiedGridRenderer::getCurrentTimeResolution() const {
  return static_cast<int>(m_currentTimeframe_ms);
}
double UnifiedGridRenderer::getCurrentPriceResolution() const {
  return 1.0;
}
double UnifiedGridRenderer::getScreenWidth() const { return width(); }
double UnifiedGridRenderer::getScreenHeight() const { return height(); }
qint64 UnifiedGridRenderer::getVisibleTimeStart() const {
  return m_viewState ? m_viewState->getVisibleTimeStart() : 0;
}
qint64 UnifiedGridRenderer::getVisibleTimeEnd() const {
  return m_viewState ? m_viewState->getVisibleTimeEnd() : 0;
}
double UnifiedGridRenderer::getMinPrice() const {
  return m_viewState ? m_viewState->getMinPrice() : 0.0;
}
double UnifiedGridRenderer::getMaxPrice() const {
  return m_viewState ? m_viewState->getMaxPrice() : 0.0;
}
QPointF UnifiedGridRenderer::getPanVisualOffset() const {
  return m_viewState ? m_viewState->getPanVisualOffset() : QPointF(0, 0);
}

MappingFrameContext UnifiedGridRenderer::currentFrameContext() const {
  std::lock_guard<std::mutex> lock(m_frameContextMutex);
  return m_lastFrameContext;
}

TimeAxisMapping UnifiedGridRenderer::currentTimeAxisMapping() const {
  return currentFrameContext().mapping;
}

QString UnifiedGridRenderer::getGridDebugInfo() const {
  return QString("Size:%1x%2").arg(width()).arg(height());
}
QString UnifiedGridRenderer::getDetailedGridDebug() const {
  return getGridDebugInfo() +
         QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO");
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
        << QString("dragging: %1").arg(m_viewState && m_viewState->isDragging() ? "yes" : "no");
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
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

void UnifiedGridRenderer::addTrade(const Trade &trade) {
  onTradeReceived(trade);
}
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) {
  if (priceMax > priceMin) {
    // An explicit viewport (Agent API, QML) is a real price window: no book-top
    // seed replaces it.
    m_gpuPriceKnown = true;
    m_gpuReseedPrice = false;
    m_priceCarry.reset();
  }
  if (m_viewState && (priceMin != m_viewState->getMinPrice() ||
                      priceMax != m_viewState->getMaxPrice())) {
    // An explicit price range turns follow-live and the auto price scale off (before
    // the viewport moves: the auto fit would replace it).
    m_viewState->setAutoPriceScale(false);
    m_viewState->enableAutoScroll(false);
  }
  onViewChanged(timeStart, timeEnd, priceMin, priceMax);
  syncGpuView(); // also when the values were unchanged (price now known)
}
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) {
  setPriceResolution(priceRes);
}
void UnifiedGridRenderer::togglePerformanceOverlay() {}

void UnifiedGridRenderer::zoomIn() {
  if (m_viewState) {
    m_viewState->handleZoomWithViewport(0.1, QPointF(width() / 2, height() / 2),
                                        QSizeF(width(), height()));
    update();
  }
}
void UnifiedGridRenderer::zoomOut() {
  if (m_viewState) {
    m_viewState->handleZoomWithViewport(
        -0.1, QPointF(width() / 2, height() / 2), QSizeF(width(), height()));
    update();
  }
}
void UnifiedGridRenderer::zoomAt(double rawDelta, double centerX, double centerY,
                                 double viewportWidth, double viewportHeight) {
  if (!m_viewState || !isVisible() || !m_viewState->isTimeWindowValid()) {
    return;
  }
  const double effectiveWidth = (viewportWidth > 0.0) ? viewportWidth : width();
  const double effectiveHeight =
      (viewportHeight > 0.0) ? viewportHeight : height();
  if (effectiveWidth <= 0.0 || effectiveHeight <= 0.0) {
    return;
  }
  m_viewState->handleZoomWithSensitivity(
      rawDelta, QPointF(centerX, centerY),
      QSizeF(effectiveWidth, effectiveHeight));
  update();
}
void UnifiedGridRenderer::zoomTimeAt(double rawDelta, double centerX,
                                     double viewportWidth) {
  if (!m_viewState || !isVisible() || !m_viewState->isTimeWindowValid()) {
    return;
  }
  const double effectiveWidth = (viewportWidth > 0.0) ? viewportWidth : width();
  if (effectiveWidth <= 0.0) {
    return;
  }
  m_viewState->handleTimeZoomWithSensitivity(rawDelta, centerX, effectiveWidth);
  update();
}
void UnifiedGridRenderer::zoomPriceAt(double rawDelta, double centerY,
                                      double viewportHeight) {
  if (!m_viewState || !isVisible() || !m_viewState->isTimeWindowValid()) {
    return;
  }
  const double effectiveHeight =
      (viewportHeight > 0.0) ? viewportHeight : height();
  if (effectiveHeight <= 0.0) {
    return;
  }
  m_viewState->handlePriceZoomWithSensitivity(rawDelta, centerY, effectiveHeight);
  update();
}
void UnifiedGridRenderer::resetZoom() {
  if (m_viewState) {
    m_viewState->resetZoom();
    update();
  }
}
void UnifiedGridRenderer::beginPanAt(double x, double y) {
  if (!m_viewState || !isVisible()) {
    return;
  }
  if (m_viewState->isAutoScrollEnabled()) {
    m_viewState->enableAutoScroll(false);
    sLog_Render("auto-scroll enabled=false reason=pan");
  }
  m_viewState->handlePanStart(QPointF(x, y));
  update();
}
void UnifiedGridRenderer::updatePanAt(double x, double y) {
  if (!m_viewState || !isVisible()) {
    return;
  }
  m_viewState->handlePanMove(QPointF(x, y));
  update();
}
void UnifiedGridRenderer::endPanAt() {
  if (!m_viewState) {
    return;
  }
  m_viewState->handlePanEnd(true);
  update();
}
void UnifiedGridRenderer::panLeft() {
  if (m_viewState) {
    m_viewState->panLeft();
    update();
  }
}
void UnifiedGridRenderer::panRight() {
  if (m_viewState) {
    m_viewState->panRight();
    update();
  }
}
void UnifiedGridRenderer::panUp() {
  if (m_viewState) {
    m_viewState->panUp();
    update();
  }
}
void UnifiedGridRenderer::panDown() {
  if (m_viewState) {
    m_viewState->panDown();
    update();
  }
}
