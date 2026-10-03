// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "PerformanceMonitor.hpp"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include "config/GuiConfigStore.hpp"
#include "datasources/CandleSeriesBuffer.hpp"
#include "render/DataProcessor.hpp"
#include "render/GridViewState.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapLabelRenderer.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/RecordingBandPolicy.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/ViewportAutoScrollController.hpp"
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
        if (m_gpuHeatmap && m_gpuLayer && m_gpuLayer->labelSignature() != m_gpuLabelSignature.load()) update();
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
    if (m_tradeBubbleTape->append({ms, trade.price, trade.size, trade.side}) && m_showTrades && m_gpuHeatmap)
      update(); // Qt coalesces trade bursts into one scene synchronization
  }
  // The live price for auto-fit (no allocation per trade: product ids are ASCII).
  if (m_gpuHeatmap && std::isfinite(trade.price) && trade.price > 0 &&
      m_activeSymbol == QLatin1String(trade.product_id.data(), static_cast<qsizetype>(trade.product_id.size())))
    m_gpuLastTrade = trade.price;
  if (m_gpuHeatmap) {
    // gpu mode: the legacy price centring must not move the GPU chart's price
    // window; a trade seeds it only when no book top has (yet) for this symbol.
    if ((m_gpuReseedPrice || !m_gpuPriceKnown) && QString::fromStdString(trade.product_id) == m_activeSymbol)
      seedGpuViewport(trade.price, trade.price);
    return;
  }
  if (QString::fromStdString(trade.product_id) == m_activeSymbol && m_heatmapStreamService) {
    m_heatmapStreamService->setLastTrade(trade.price, m_viewState.get());
  }
}

void UnifiedGridRenderer::setLiveBookTop(double bestBid, double bestAsk) {
  if (m_gpuHeatmap) {
    // gpu mode: the book top seeds the first viewport (and the price window after
    // a symbol switch); the legacy stream's price centring stays out of it.
    if (std::isfinite(bestBid) && std::isfinite(bestAsk) && bestBid > 0 && bestAsk >= bestBid)
      m_gpuBookMid = (bestBid + bestAsk) * 0.5;
    seedGpuViewport(bestBid, bestAsk);
    return;
  }
  if (!m_heatmapStreamService) return;
  // Recording mode ignores legacy slices, so nothing else places the first
  // window: seed the viewport from the live mid (a kRows band at the finest
  // native tick, the geometry the legacy bootstrap used) before centering.
  if (m_recordingSource && m_viewState && !m_viewState->isTimeWindowValid() &&
      std::isfinite(bestBid) && std::isfinite(bestAsk) && bestBid > 0 && bestAsk >= bestBid) {
    const double tick = recording_view::kFinestNativeTick;
    const double half = tick * recording_view::kRows * 0.5;
    const double mid = std::round((bestBid + bestAsk) * 0.5 / tick) * tick;
    applyHeatmapRangeReset(std::max(0.0, mid - half), mid + half, tick, 0, recording_view::kRows);
  }
  m_heatmapStreamService->setLiveBook(bestBid, bestAsk, m_viewState.get());
}

void UnifiedGridRenderer::applyHeatmapRangeReset(double minPrice, double maxPrice, double tickSize,
                                                 int gridWidth, int gridHeight) {
  if (!m_useGpuHeatmap) {
    m_useGpuHeatmap = true;
    m_heatmapOverlay.requestFullTextureRebuild();
    m_heatmapStreamService->ensureClockStarted();
  }
  auto result = m_heatmapStreamService->handleRangeReset(
      minPrice, maxPrice, tickSize, gridWidth, gridHeight, m_viewState.get(), m_heatmapOverlay);
  if (result.tickSizeChanged) emit heatmapTickSizeChanged();
  if (m_axisTextService) {
    if (m_axisTextService->timeAxisModel()) m_axisTextService->timeAxisModel()->recalculateTicks();
    if (m_axisTextService->priceAxisModel()) m_axisTextService->priceAxisModel()->recalculateTicks();
  }
  update();
}

void UnifiedGridRenderer::resetLivePriceCenter() {
  if (m_heatmapStreamService) m_heatmapStreamService->resetPriceCenter();
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
  if (!m_viewState || !m_dataProcessor)
    return;
  update();
  if (m_gpuHeatmap) {
    if (!m_gpuSelfViewport) m_gpuViewPristine = false; // someone else moved the view
    // The controller plans from the committed view; the legacy band stream is
    // muted, so its viewport is re-published when the renderer flips back.
    syncGpuView();
    updateHistoryFloorState();
    return;
  }
  if (m_recordingSource && m_heatmapStreamService &&
      !m_heatmapStreamService->recordingViewportReady(m_viewState.get())) return;

  // The DataProcessor places the heatmap window over the view and fetches
  // what the cache lacks (INV-045). It ignores sub-bucket changes.
  const qint64 viewStart = m_viewState->getVisibleTimeStart();
  const qint64 viewEnd = m_viewState->getVisibleTimeEnd();
  const bool follow = m_viewState->isAutoScrollEnabled();
  const double minPrice = m_viewState->getMinPrice();
  const double maxPrice = m_viewState->getMaxPrice();
  const double widthPx = m_viewState->getViewportWidth();
  const double heightPx = m_viewState->getViewportHeight();
  if (m_viewState->isTimeWindowValid()) {
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        [this, viewStart, viewEnd, follow, minPrice, maxPrice, widthPx, heightPx]() {
          m_dataProcessor->setHeatmapViewport(viewStart, viewEnd, follow, minPrice, maxPrice, widthPx, heightPx);
        },
        Qt::QueuedConnection);
  }
  updateHistoryFloorState();
}

void UnifiedGridRenderer::updateHistoryFloorState() {
  // INV-048: the floor shows only when a manual view reaches the storage floor.
  const bool atFloor = m_viewState && !m_viewState->isAutoScrollEnabled() &&
                       m_oldestHeatmapAvailableMs > 0 &&
                       m_viewState->getVisibleTimeStart() <= m_oldestHeatmapAvailableMs;
  setHistoryExhausted(atFloor);
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
    if (m_gpuHeatmap && m_gpuViewPristine && m_viewState && m_viewState->isTimeWindowValid() &&
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

void UnifiedGridRenderer::setIntensityScale(double scale) {
  if (m_intensityScale != scale) {
    m_intensityScale = scale;
    if (m_useGpuHeatmap && m_dataProcessor) {
      QMetaObject::invokeMethod(
          m_dataProcessor.get(),
          [this, scale]() { m_dataProcessor->setHeatmapIntensityScale(scale); },
          Qt::QueuedConnection);
    }
    update();
    emit intensityScaleChanged();
  }
}

void UnifiedGridRenderer::setMaxCells(int max) {
  if (m_maxCells != max) {
    m_maxCells = max;
    emit maxCellsChanged();
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
    if (m_heatmapStreamService) {
      m_heatmapStreamService->setAutoScrollPaddingFrac(clamped);
    }
    emit autoScrollPaddingFracChanged();
  }
}

void UnifiedGridRenderer::setAutoScrollSmoothEnabled(bool enabled) {
  if (m_smoothAutoScrollEnabled != enabled) {
    m_smoothAutoScrollEnabled = enabled;
    if (m_heatmapStreamService) {
      m_heatmapStreamService->setAutoScrollSmoothEnabled(enabled);
    }
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
  resetHeatmapHistoryStatus();
  setOldestHeatmapAvailableMs(0);
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
  m_heatmapStreamService->incrementGeneration();
  m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
  update();
}

void UnifiedGridRenderer::setHistoryRequestInFlight(bool inFlight) {
  if (m_historyRequestInFlight == inFlight) {
    return;
  }
  m_historyRequestInFlight = inFlight;
  emit heatmapHistoryStatusChanged();
}

void UnifiedGridRenderer::setHistoryExhausted(bool exhausted) {
  if (m_historyExhausted == exhausted) {
    return;
  }
  m_historyExhausted = exhausted;
  emit heatmapHistoryStatusChanged();
}

void UnifiedGridRenderer::setOldestHeatmapAvailableMs(int64_t oldestMs) {
  if (m_oldestHeatmapAvailableMs == oldestMs) {
    return;
  }
  m_oldestHeatmapAvailableMs = oldestMs;
  emit heatmapHistoryStatusChanged();
}

void UnifiedGridRenderer::resetHeatmapHistoryStatus() {
  if (!m_historyRequestInFlight && !m_historyExhausted) {
    return;
  }
  m_historyRequestInFlight = false;
  m_historyExhausted = false;
  emit heatmapHistoryStatusChanged();
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
  if (!m_gpuHeatmap || !m_viewState || m_viewState->autoPriceScale() || !m_gpuReseedPrice) m_priceCarry.reset();
  if (m_gpuHeatmap && m_gpuPriceKnown && !m_gpuReseedPrice && m_viewState && m_viewState->isTimeWindowValid() &&
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
  resetLivePriceCenter();
  // gpu mode: the controller serial switches the heatmap (the node holds the old
  // picture until the new spans are ready); clearData() resets only the legacy
  // stream and the trade overlays. The next book top centres the new price.
  if (m_carryWaitTimer) m_carryWaitTimer->stop(); // the live-only wait is per symbol
  // Reseed first: the layer's limitsChanged (setSymbol) must not apply a pending carry
  // before the new symbol has a price of its own.
  if (m_gpuHeatmap) m_gpuReseedPrice = true;
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
    resetHeatmapHistoryStatus();
    setOldestHeatmapAvailableMs(0);
    const int64_t previousTf = m_currentTimeframe_ms;
    m_currentTimeframe_ms = timeframe_ms;
    if (m_useGpuHeatmap && timeframe_ms > 0 && m_heatmapStreamService) {
      m_heatmapStreamService->handleTimeframeChange(static_cast<int64_t>(timeframe_ms),
                                                    m_heatmapOverlay);
    }
    m_manualTimeframeSet = true;
    m_manualTimeframeTimer.start();
    // Spec rule 1: a column is the timeframe, and a longer one is how to see further
    // back. Keep the columns on screen (S6d: keeping the time span gave four 1h columns
    // after 1m, and 1,600 hairline 1m columns after 1h): scale the span by the timeframe
    // ratio about the view end, inside the new timeframe's 1 column/px limit. The legacy
    // path resets to initial_column_px columns instead. previousTf >= 1 s skips the 100 ms
    // default before the server advertises its timeframe. The span is read before the new
    // limits apply (72 h of 1h is 72 columns of 1m, not 30.8 h of the clamp then /60), and
    // limits, scaled span and follow-live end are published as ONE viewport change: the
    // layer's limitsChanged is held back here, so the old view is never re-clamped first.
    const bool keepColumns = m_gpuHeatmap && m_gpuLayer && timeframe_ms > 0 && previousTf >= 1000 && m_viewState &&
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
          [this, timeframe_ms]() {
            // Update both the display timeframe and the slice filter so that
            // only slices belonging to the newly selected timeframe pass
            // through DataProcessor::onHeatmapSliceReceived.
            m_dataProcessor->setTimeframe(timeframe_ms);
            m_dataProcessor->setServerTimeframe(timeframe_ms);
          },
          Qt::QueuedConnection);
    }
    update();
    emit timeframeChanged();
  }
}

void UnifiedGridRenderer::setLiquidityLabelMode(int mode) {
  if (m_liquidityLabelMode == mode) {
    return;
  }
  m_liquidityLabelMode = mode;
  update();
  emit liquidityLabelModeChanged();
}

void UnifiedGridRenderer::setHeatmapLiquidityThreshold(double threshold) {
  const double clamped = std::max(0.0, threshold);
  if (std::abs(m_heatmapLiquidityThreshold - clamped) < 1e-9) {
    return;
  }
  m_heatmapLiquidityThreshold = clamped;
  // Debounce: defer the expensive ring rebuild until the slider stops moving.
  if (!m_thresholdRebuildTimer) {
      m_thresholdRebuildTimer = new QTimer(this);
      m_thresholdRebuildTimer->setSingleShot(true);
      m_thresholdRebuildTimer->setInterval(80);
      connect(m_thresholdRebuildTimer, &QTimer::timeout, this, [this] {
          rebuildHeatmapTextureFromRing();
          update();
      });
  }
  m_thresholdRebuildTimer->start(); // restarts if already running
  update();
  emit heatmapLiquidityThresholdChanged();
}

void UnifiedGridRenderer::rebuildHeatmapTextureFromRing() {
  m_heatmapStreamService->rebuildTextureFromRing(
      m_heatmapOverlay, m_heatmapLiquidityThreshold, m_liquidityLabelMode);
}

void UnifiedGridRenderer::setHeatmapBackgroundColor(const QColor &color) {
  if (m_heatmapBackgroundColor == color) {
    return;
  }
  m_heatmapBackgroundColor = color;
  m_heatmapOverlay.setBackgroundColor(color);
  emit heatmapBackgroundColorChanged();
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

void UnifiedGridRenderer::setHeatmapColorPreset(const QString& preset) {
    // The five legacy presets live in HeatmapPalette (shared with the GPU path).
    const auto gradients = heatmap::gpu::presetGradients(preset.toStdString());
    if (!gradients) {
        sLog_Warning("Unknown heatmap color preset ignored: preset=" << preset);
        return;
    }
    m_heatmapOverlay.setBidGradient(HeatmapOverlayRenderer::toColorStops(gradients->bid));
    m_heatmapOverlay.setAskGradient(HeatmapOverlayRenderer::toColorStops(gradients->ask));
    m_heatmapOverlay.setPaletteGamma(gradients->gamma);
    update();
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
  if (m_viewState && m_gpuHeatmap) {
    m_viewState->enableAutoScroll(enabled);
    if (enabled) returnGpuToLive(); // back to the live edge from history or the future, same span
    update();
    emit autoScrollEnabledChanged();
    sLog_Render("auto-scroll enabled=" << enabled << " reason=request renderer=gpu");
    return;
  }
  if (m_viewState) {
    const bool wasEnabled = m_viewState->isAutoScrollEnabled();
    m_viewState->enableAutoScroll(enabled);
    if (!enabled && m_heatmapStreamService) {
      m_heatmapStreamService->cancelPriceCenter();
    }
    if (enabled && !wasEnabled && m_heatmapStreamService) {
      m_heatmapStreamService->requestPriceCenter(m_viewState.get());
    }
    update();
    emit autoScrollEnabledChanged();
    sLog_Render("auto-scroll enabled=" << enabled << " reason=request");
    if (enabled && m_viewState->isTimeWindowValid() && m_heatmapStreamService) {
      m_heatmapStreamService->updateAutoScrollLag(
          *m_viewState,
          m_heatmapStreamService->timeAuthority().activeTimeframeMs());
    }
    // Following again pins the window to live from the cache; no refetch.
    onViewportChanged();
  }
}


// ── GPU heatmap renderer (S6b) ───────────────────────────────────────────────
double UnifiedGridRenderer::heatmapTickSize() const {
  if (m_gpuHeatmap && m_gpuLayer) return m_gpuLayer->tickPrice();
  return m_heatmapStreamService ? m_heatmapStreamService->tickSize() : 0.0;
}

void UnifiedGridRenderer::setHeatmapService(heatmap::HeatmapDataService* service) {
  if (m_gpuLayer) m_gpuLayer->setService(service);
}

void UnifiedGridRenderer::setHeatmapRenderer(const QString& renderer) {
  const bool gpu = renderer == QStringLiteral("gpu");
  if (gpu == m_gpuHeatmap || !m_gpuLayer) return;
  sLog_App("Heatmap renderer=" << (gpu ? "gpu" : "legacy") << " symbol=" << m_activeSymbol
           << " tfMs=" << m_currentTimeframe_ms);
  m_gpuHeatmap = gpu;
  m_gpuLayer->setSymbol(m_activeSymbol.toStdString());
  m_gpuLayer->setTimeframeMs(m_currentTimeframe_ms);
  syncGpuSurface();
  m_gpuLayer->setActive(gpu);
  if (m_dataProcessor) {
    // Mute or resume only the legacy band stream (INV-088): footprint, TPO and VP stay.
    QMetaObject::invokeMethod(m_dataProcessor.get(), [processor = m_dataProcessor.get(), gpu] {
      processor->setHeatmapEnabled(!gpu);
    }, Qt::QueuedConnection);
  }
  if (gpu) {
    // A legacy viewport carries on; a fresh one waits for the book-top seed.
    m_gpuPriceKnown = m_viewState && m_viewState->isTimeWindowValid() &&
                      m_viewState->getMaxPrice() > m_viewState->getMinPrice();
    m_gpuReseedPrice = false;
    if (m_viewState) m_viewState->setAutoPriceScale(true); // the default view model
    applyGpuLimits();
    syncGpuView();
    followGpuLive();
    bootstrapGpuTimeView();
  } else {
    m_priceCarry.reset();
    if (m_viewState) {
      m_viewState->setAutoPriceScale(false); // gpu only
      m_viewState->setMinSpans(0, 0);
      m_viewState->setMaxSpans(0, 0);
    }
    // Legacy resume adopts the current view before the stream unmutes: pending
    // bootstrap/initial centring (from startup or a gpu-mode symbol or tf
    // change) must not replace it on the next book, trade or window update.
    if (m_heatmapStreamService && m_viewState) m_heatmapStreamService->adoptViewport(*m_viewState);
    m_heatmapOverlay.requestFullTextureRebuild();
    onViewportChanged(); // the processor places its window over the current view again
  }
  emit heatmapTickSizeChanged();
  emit heatmapRendererChanged();
  update();
}

void UnifiedGridRenderer::setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings,
                                                  bool explicitManualTick) {
  if (!m_gpuLayer) return;
  m_gpuLayer->setSettings(settings, explicitManualTick);
  // Both renderers draw the chart's palette and colour range (A/B parity).
  const auto gradients = heatmap::gpu::gradientsFor(settings);
  m_showTrades = settings.showTrades;
  m_tradeMinNotional = settings.tradeMinNotional;
  const auto& bid = gradients.bid.back();
  const auto& ask = gradients.ask.back();
  m_tradeBuyColor = QColor(bid.r, bid.g, bid.b);
  m_tradeSellColor = QColor(ask.r, ask.g, ask.b);
  m_heatmapOverlay.setBidGradient(HeatmapOverlayRenderer::toColorStops(gradients.bid));
  m_heatmapOverlay.setAskGradient(HeatmapOverlayRenderer::toColorStops(gradients.ask));
  m_heatmapOverlay.setPaletteGamma(gradients.gamma);
  if (settings.sensitivityMin > 0.0 && settings.sensitivityMax > settings.sensitivityMin) {
    m_heatmapSensitivityMin = settings.sensitivityMin;
    m_heatmapSensitivityMax = settings.sensitivityMax;
    m_chartSensitivityApplied = true;
  }
  applyGpuLimits();
  update();
}

void UnifiedGridRenderer::setHeatmapTickMemory(const heatmap::ManualTickMemory& memory) {
  if (m_gpuLayer) m_gpuLayer->setTickMemory(memory);
}

// The GPU palette's tone mapping is the legacy shader's gamma/contrast/floor.
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
// API identically; legacy mode keeps its unclamped behaviour.
void UnifiedGridRenderer::applyGpuLimits() {
  if (!m_viewState || m_gpuLimitsDeferred) return;
  if (m_gpuHeatmap && m_gpuLayer) {
    m_viewState->setMinSpans(m_gpuLayer->minTimeSpanMs(), m_gpuLayer->minPriceSpan());
    m_viewState->setMaxSpans(m_gpuLayer->maxTimeSpanMs(), m_gpuLayer->maxPriceSpan());
  } else {
    m_viewState->setMinSpans(0, 0);
    m_viewState->setMaxSpans(0, 0);
  }
}

void UnifiedGridRenderer::syncGpuView() {
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid()) return;
  heatmap::gpu::ViewWindow view{static_cast<double>(m_viewState->getVisibleTimeStart()),
                                static_cast<double>(m_viewState->getVisibleTimeEnd()), m_viewState->getMinPrice(),
                                m_viewState->getMaxPrice()};
  m_gpuLayer->setView(view, m_gpuPriceKnown);
}

// Follow-live from LiveSnapshot::openEndMs (HeatmapStreamService::handleRenderTick
// is skipped in gpu mode): the view end stays one padding past the live bucket's end.
void UnifiedGridRenderer::followGpuLive() {
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() ||
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
// span lands. The price window uses initial_price_pct of the legacy band geometry
// (kRows rows at the finest native tick), as the legacy first view does.
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
  const auto* scroll = m_heatmapStreamService ? m_heatmapStreamService->autoScrollController() : nullptr;
  const int pct = scroll ? scroll->initialPricePct() : 5;
  const double full = recording_view::kFinestNativeTick * recording_view::kRows;
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
        if (m_gpuHeatmap && m_gpuLayer && m_priceCarry && !m_gpuReseedPrice && !m_gpuLayer->priceScaleCurrent())
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
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid()) return;
  const int64_t span = m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart();
  const int64_t end = gpuLiveEndMs(span);
  if (end <= 0) return; // nothing known yet: the next live frame steps forward
  sLog_Render("GPU heatmap returns to live: anchor=" << m_gpuLayer->liveAnchorMs() << " end=" << end);
  m_viewState->setViewport(end - span, end, m_viewState->getMinPrice(), m_viewState->getMaxPrice());
  syncGpuView(); // publishes the view even when it was already there
  emit liveRenderTick();
}

void UnifiedGridRenderer::bootstrapGpuTimeView() {
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || m_viewState->isTimeWindowValid()) {
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

// The legacy first view's time span: initial_column_px per column (16 columns min).
qint64 UnifiedGridRenderer::gpuInitialSpanMs(double widthPx) const {
  const int64_t tf = std::max<int64_t>(1, m_currentTimeframe_ms);
  const auto* scroll = m_heatmapStreamService ? m_heatmapStreamService->autoScrollController() : nullptr;
  const int gridWidth = m_heatmapStreamService ? m_heatmapStreamService->gridWidth() : 5120;
  return scroll ? scroll->initialSpanMs(widthPx, gridWidth, tf) : 256 * tf;
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
                                  if (!m_gpuHeatmap || !m_viewState || !m_viewState->autoPriceScale() ||
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
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState) return false;
  const auto window = gpuFitPriceWindow(start, end, true);
  if (!window) return false; // no visible candle: the given price stays
  std::tie(priceMin, priceMax) = *window;
  m_gpuPriceKnown = true; // a real price window (no book-top seed replaces it)
  m_gpuReseedPrice = false;
  m_priceCarry.reset();
  return true;
}

void UnifiedGridRenderer::refitAutoPrice() {
  if (!m_gpuHeatmap || !m_viewState || !m_viewState->autoPriceScale() || !m_viewState->isTimeWindowValid() ||
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
  if (!m_gpuHeatmap) return; // the legacy renderer has no auto price scale
  // On = the price-axis double-click: fit now (candles, else the live price); with
  // nothing to fit yet it is on and the next change in view fits.
  if (!fitView(false, true)) m_viewState->setAutoPriceScale(true);
}

bool UnifiedGridRenderer::fitView(bool time, bool price) {
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() || (!time && !price) ||
      m_viewState->isDragging()) {
    sLog_Render("view fit skipped: renderer=" << (m_gpuHeatmap ? "gpu" : "legacy") << " time=" << time
                << " price=" << price);
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
  if (request.autoScale.value_or(false) && !m_gpuHeatmap) return QStringLiteral("auto_scale_unavailable");
  const bool explicitPrice = request.priceMin && request.priceMax;
  const bool explicitTime = request.startMs && request.endMs;
  const qint64 start0 = m_viewState->getVisibleTimeStart(), end0 = m_viewState->getVisibleTimeEnd();
  if (!m_gpuHeatmap) {
    // Legacy: its follow mode and price centring live in enableAutoScroll.
    if (explicitTime || explicitPrice) {
      enableAutoScroll(false);
      setViewport(request.startMs.value_or(start0), request.endMs.value_or(end0),
                  request.priceMin.value_or(m_viewState->getMinPrice()),
                  request.priceMax.value_or(m_viewState->getMaxPrice()));
    } else if (request.followLive) {
      enableAutoScroll(*request.followLive);
    }
    return {};
  }
  // gpu: the flags first (no viewport change of their own), then one commit.
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
  if (!m_gpuHeatmap || !m_gpuLayer || !m_viewState || !m_viewState->isTimeWindowValid() ||
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
  m_recordingSource = config.heatmap.source == "recording";
  if (m_heatmapStreamService) m_heatmapStreamService->setRecordingMode(m_recordingSource);
  if (m_dataProcessor) {
    const auto heatmap = config.heatmap;
    QMetaObject::invokeMethod(m_dataProcessor.get(), [this, heatmap] {
      m_dataProcessor->setRecordingConfig(heatmap.source == "recording", heatmap.targetRowPx);
    }, Qt::QueuedConnection);
  }
  setHeatmapGamma(config.heatmap.gamma);
  setHeatmapContrast(config.heatmap.contrast);
  setHeatmapShaderFloor(config.heatmap.shaderFloor);
  syncGpuTone();
  m_heatmapTargetRowPx = std::clamp(config.heatmap.targetRowPx, 1, 64);
  m_heatmapCellAspect = std::clamp(config.heatmap.cellAspect, 0.05, 4.0);
  if (!m_chartSensitivityApplied && config.heatmap.sensitivityMin > 0.0 &&
      config.heatmap.sensitivityMax > config.heatmap.sensitivityMin) {
    m_heatmapSensitivityMin = config.heatmap.sensitivityMin;
    m_heatmapSensitivityMax = config.heatmap.sensitivityMax;
  }
  if (m_heatmapStreamService) {
    m_heatmapStreamService->setInitialColumnPx(config.heatmap.initialColumnPx);
    m_heatmapStreamService->setInitialPricePct(config.heatmap.initialPricePct);
  }
  if (m_axisTextService) {
    m_axisTextService->setAxisLabelPxOverride(config.gui.axisLabelPx);
    m_axisTextService->refreshAxisLayout();
  }
  applyTpoConfig(config.tpo);
  if (config.heatmap.labelPx > 0 && config.heatmap.labelPx <= 128) {
    m_heatmapLabelPx = config.heatmap.labelPx;
  } else if (config.heatmap.labelPx > 128) {
    sLog_Warning("Heatmap labelPx exceeds max 128, keeping current: labelPx="
                 << config.heatmap.labelPx << " using=" << m_heatmapLabelPx);
  }
  onViewportChanged();
  update();
}

void UnifiedGridRenderer::applyServerConfig(const ServerConfig &config) {
  if (m_dataProcessor) {
    const bool available = config.wasAdvertised("recording.available") && config.recording.available;
    const int gridWidth = config.heatmap.gridWidth;
    const int gridHeight = config.heatmap.gridHeight;
    QMetaObject::invokeMethod(m_dataProcessor.get(), [this, available, gridWidth, gridHeight] {
      m_dataProcessor->setHeatmapGridDimensions(gridWidth, gridHeight);
      m_dataProcessor->setRecordingCapability(available);
    }, Qt::QueuedConnection);
  }
  if (m_heatmapStreamService) {
    m_heatmapStreamService->setGridDimensions(
        config.heatmap.gridWidth, config.heatmap.gridHeight, m_heatmapOverlay);
  }

  int64_t forcedTf = config.heatmap.activeTimeframeMs;
  if (forcedTf <= 0 && !config.heatmap.timeframesMs.empty()) {
    forcedTf = config.heatmap.timeframesMs.front();
  }
  if (forcedTf > 0) {
    setTimeframe(static_cast<int>(forcedTf));
    if (m_dataProcessor) {
      QMetaObject::invokeMethod(
          m_dataProcessor.get(),
          [this, forcedTf]() { m_dataProcessor->setServerTimeframe(forcedTf); },
          Qt::QueuedConnection);
    }
  }
}

void UnifiedGridRenderer::fitHeatmapToDataRange() {
  if (m_heatmapStreamService->fitToDataRange(m_viewState.get())) {
    update();
  }
}

QString UnifiedGridRenderer::getTextureSize() const {
  if (m_useGpuHeatmap && m_heatmapStreamService->stream()) {
    const auto snapshot = m_heatmapStreamService->stream()->snapshot();
    if (snapshot.gridWidth > 0 && snapshot.gridHeight > 0) {
      return QString("%1x%2").arg(snapshot.gridWidth).arg(snapshot.gridHeight);
    }
  }
  return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
  if (m_useGpuHeatmap && m_heatmapStreamService->stream()) {
    const auto snapshot = m_heatmapStreamService->stream()->snapshot();
    if (snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0) {
      return "N/A";
    }
    const int bytesPerPixel =
        (m_heatmapStreamService->intensityBytesPerCell() > 0) ? m_heatmapStreamService->intensityBytesPerCell() : 1;
    qint64 bytes = static_cast<qint64>(snapshot.gridWidth) *
                   snapshot.gridHeight * bytesPerPixel;
    double mb = bytes / (1024.0 * 1024.0);
    return QString("%1 MB").arg(mb, 0, 'f', 1);
  }
  return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
  if (m_useGpuHeatmap) {
    return (m_heatmapStreamService->intensityBytesPerCell() == 2) ? "Grayscale16" : "Grayscale8";
  }
  return "N/A";
}

QString UnifiedGridRenderer::getLabelRingMemory() const {
  if (!m_heatmapStreamService->stream()) {
    return "Label ring: N/A";
  }
  HeatmapStreamState::LabelSnapshot labels;
  if (!m_heatmapStreamService->stream()->copyLabelSnapshot(labels)) {
    return "Label ring: N/A";
  }
  const int gridWidth = labels.snapshot.gridWidth;
  const int gridHeight = labels.snapshot.gridHeight;
  if (gridWidth <= 0 || gridHeight <= 0) {
    return "Label ring: N/A";
  }
  const qint64 cells = static_cast<qint64>(gridWidth) * gridHeight;
  const qint64 bytesIntensity = cells * static_cast<qint64>(sizeof(uint16_t));
  const qint64 bytesLiquidity = cells * static_cast<qint64>(sizeof(uint16_t));
  const qint64 bytesScales =
      static_cast<qint64>(gridWidth) * static_cast<qint64>(sizeof(double));
  const qint64 totalBytes = bytesIntensity + bytesLiquidity + bytesScales;
  const double mb = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
  return QString("Label ring: %1 MB").arg(mb, 0, 'f', 2);
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

double UnifiedGridRenderer::getUploadBandwidth() const {
  return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
  if (m_useGpuHeatmap && m_heatmapStreamService->stream()) {
    const auto snapshot = m_heatmapStreamService->stream()->snapshot();
    if (snapshot.gridWidth > 0) {
      return QString("%1/%2")
          .arg(m_heatmapStreamService->stream()->writeColumn())
          .arg(snapshot.gridWidth);
    }
  }
  return "N/A";
}

int UnifiedGridRenderer::getDirtyRegionCount() const {
  if (m_useGpuHeatmap) {
    if (m_heatmapStreamService->stream()) {
      return m_heatmapStreamService->stream()->pendingUploadCount();
    }
  }
  return 0;
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
    m_panSyncPending = false;
    update();
  }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent *event) {
  if (m_gpuHeatmap && m_viewState && isVisible() && m_viewState->isTimeWindowValid() &&
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
  return m_dataProcessor ? m_dataProcessor->getPriceResolution() : 1.0;
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

bool UnifiedGridRenderer::heatmapDataPriceRange(double &outMin,
                                                double &outMax) const {
  if (!m_heatmapStreamService->stream()) {
    return false;
  }
  const auto snapshot = m_heatmapStreamService->stream()->snapshot();
  if (snapshot.tickSize <= 0.0 || snapshot.maxPrice <= snapshot.minPrice) {
    return false;
  }
  outMin = snapshot.minPrice;
  outMax = snapshot.maxPrice;
  return true;
}

bool UnifiedGridRenderer::heatmapDataTimeRange(qint64 &outStart,
                                               qint64 &outEnd) const {
  if (!m_heatmapStreamService->stream()) {
    return false;
  }
  const auto snapshot = m_heatmapStreamService->stream()->snapshot();
  const int64_t cadenceMs = (m_heatmapStreamService->timeAuthority().activeTimeframeMs() > 0)
                                ? m_heatmapStreamService->timeAuthority().activeTimeframeMs()
                                : static_cast<int64_t>(snapshot.appendMs);
  if (cadenceMs <= 0 || snapshot.gridWidth <= 0) {
    return false;
  }
  const int64_t bufferSpanMs =
      static_cast<int64_t>(snapshot.gridWidth) * cadenceMs;
  if (bufferSpanMs <= 0) {
    return false;
  }
  int64_t dataEnd = 0;
  if (snapshot.lastSliceStartMs != std::numeric_limits<int64_t>::min()) {
    dataEnd = snapshot.lastSliceStartMs + cadenceMs;
  } else if (snapshot.timeOriginMs != 0) {
    dataEnd = snapshot.timeOriginMs + bufferSpanMs;
  } else {
    return false;
  }
  const int64_t dataStart = dataEnd - bufferSpanMs;
  if (dataEnd <= dataStart) {
    return false;
  }
  outStart = dataStart;
  outEnd = dataEnd;
  return true;
}

QString UnifiedGridRenderer::getGridDebugInfo() const {
  return QString("Size:%1x%2").arg(width()).arg(height());
}
QString UnifiedGridRenderer::getDetailedGridDebug() const {
  return getGridDebugInfo() +
         QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO");
}
QString UnifiedGridRenderer::getViewportMathDebug() const {
  if (!m_viewState) {
    return "Viewport: N/A";
  }
  const auto snapshot = m_heatmapStreamService->stream() ? m_heatmapStreamService->stream()->snapshot()
                                        : HeatmapStreamState::Snapshot{};
  const QRectF bounds = boundingRect();
  const bool forceFull =
      qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
  const int gridWidth =
      (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapStreamService->gridWidth();
  const int gridHeight =
      (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapStreamService->gridHeight();
  const int64_t cadenceMs = (m_heatmapStreamService->timeAuthority().activeTimeframeMs() > 0)
                                ? m_heatmapStreamService->timeAuthority().activeTimeframeMs()
                                : static_cast<int64_t>(snapshot.appendMs);

  UgrFrameMath::ViewportState viewportState;
  viewportState.valid = m_viewState->isTimeWindowValid();
  viewportState.timeStart =
      static_cast<double>(m_viewState->getVisibleTimeStart());
  viewportState.timeEnd = static_cast<double>(m_viewState->getVisibleTimeEnd());
  viewportState.minPrice = m_viewState->getMinPrice();
  viewportState.maxPrice = m_viewState->getMaxPrice();
  viewportState.panVisualOffset = m_viewState->getPanVisualOffset();
  viewportState.dragging = m_viewState->isDragging();
  viewportState = UgrFrameMath::applyDragPan(viewportState, bounds);

  UgrFrameMath::GridState gridState;
  gridState.gridWidth = gridWidth;
  gridState.gridHeight = gridHeight;
  gridState.cadenceMs = cadenceMs;
  gridState.timeOriginMs = snapshot.timeOriginMs;
  gridState.lastSliceStartMs = snapshot.lastSliceStartMs;
  gridState.filledColumns = snapshot.filledColumns;
  gridState.tickSize = snapshot.tickSize;
  gridState.dataMinPrice = snapshot.minPrice;
  gridState.dataMaxPrice = snapshot.maxPrice;
  gridState.forceFull = forceFull;

  const UgrFrameMath::RenderRects renderRects =
      UgrFrameMath::computeRenderRects(bounds, viewportState, gridState);
  const double viewTimeSpan =
      renderRects.viewTimeEnd - renderRects.viewTimeStart;
  const double viewPriceSpan =
      renderRects.viewMaxPrice - renderRects.viewMinPrice;

  QStringList lines;
  lines << "Viewport Math"
        << QString("view.time: %1 → %2 (%3 ms)")
               .arg(static_cast<qint64>(renderRects.viewTimeStart))
               .arg(static_cast<qint64>(renderRects.viewTimeEnd))
               .arg(static_cast<qint64>(std::max(0.0, viewTimeSpan)))
        << QString("view.price: %1 → %2 (Δ%3)")
               .arg(renderRects.viewMinPrice, 0, 'f', 4)
               .arg(renderRects.viewMaxPrice, 0, 'f', 4)
               .arg(std::max(0.0, viewPriceSpan), 0, 'f', 4)
        << QString("tick: %1  grid: %2x%3  append: %4")
               .arg(snapshot.tickSize, 0, 'f', 6)
               .arg(gridWidth)
               .arg(gridHeight)
               .arg(cadenceMs);

  if (cadenceMs > 0 && snapshot.tickSize > 0.0 && snapshot.timeOriginMs != 0 &&
      viewTimeSpan > 0.0 && viewPriceSpan > 0.0) {
    const double overlapStart =
        std::max(renderRects.viewTimeStart, renderRects.dataStart);
    const double overlapEnd =
        std::min(renderRects.viewTimeEnd, renderRects.dataEnd);
    const double overlapMin =
        std::max(renderRects.viewMinPrice, snapshot.minPrice);
    const double overlapMax =
        std::min(renderRects.viewMaxPrice, snapshot.maxPrice);

    lines << QString("data.time: %1 → %2")
                 .arg(static_cast<qint64>(renderRects.dataStart))
                 .arg(static_cast<qint64>(renderRects.dataEnd))
          << QString("data.price: %1 → %2")
                 .arg(snapshot.minPrice, 0, 'f', 4)
                 .arg(snapshot.maxPrice, 0, 'f', 4)
          << QString("overlap.time: %1 → %2")
                 .arg(static_cast<qint64>(overlapStart))
                 .arg(static_cast<qint64>(overlapEnd))
          << QString("overlap.price: %1 → %2")
                 .arg(overlapMin, 0, 'f', 4)
                 .arg(overlapMax, 0, 'f', 4);
    const double cellW =
        (renderRects.srcRect.width() > 0.0)
            ? (renderRects.drawRect.width() / renderRects.srcRect.width())
            : 0.0;
    const double cellH =
        (renderRects.srcRect.height() > 0.0)
            ? (renderRects.drawRect.height() / renderRects.srcRect.height())
            : 0.0;

    lines << QString("drawRect: x%1 y%2 w%3 h%4")
                 .arg(renderRects.drawRect.x(), 0, 'f', 1)
                 .arg(renderRects.drawRect.y(), 0, 'f', 1)
                 .arg(renderRects.drawRect.width(), 0, 'f', 1)
                 .arg(renderRects.drawRect.height(), 0, 'f', 1)
          << QString("srcRect: x%1 y%2 w%3 h%4")
                 .arg(renderRects.srcRect.x(), 0, 'f', 2)
                 .arg(renderRects.srcRect.y(), 0, 'f', 2)
                 .arg(renderRects.srcRect.width(), 0, 'f', 2)
                 .arg(renderRects.srcRect.height(), 0, 'f', 2)
          << QString("cell: %1 x %2 px")
                 .arg(cellW, 0, 'f', 2)
                 .arg(cellH, 0, 'f', 2)
          << QString("forceFull: %1  dragging: %2")
                 .arg(forceFull ? "yes" : "no")
                 .arg(m_viewState->isDragging() ? "yes" : "no");
  } else {
    lines << "data: N/A";
  }

  return lines.join('\n');
}

QString UnifiedGridRenderer::getDataPipelineDebug() const {
  QStringList lines;
  lines << "Data Pipeline";

  if (!m_heatmapStreamService->stream()) {
    lines << "stream: N/A";
    return lines.join('\n');
  }

  const auto snapshot = m_heatmapStreamService->stream()->snapshot();
  const int gridWidth =
      (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapStreamService->gridWidth();
  const int gridHeight =
      (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapStreamService->gridHeight();
  const int pendingUploads = m_heatmapStreamService->stream()->pendingUploadCount();
  const int writeColumn = m_heatmapStreamService->stream()->writeColumn();
  const int64_t cadenceMs = (m_heatmapStreamService->timeAuthority().activeTimeframeMs() > 0)
                                ? m_heatmapStreamService->timeAuthority().activeTimeframeMs()
                                : static_cast<int64_t>(snapshot.appendMs);
  const qint64 lastAppendMs = m_heatmapStreamService->stream()->lastAppendMs();
  const qint64 nowMs = m_heatmapStreamService->clock().isValid() ? m_heatmapStreamService->clock().elapsed() : 0;
  const qint64 ageMs =
      (lastAppendMs > 0 && nowMs >= lastAppendMs) ? (nowMs - lastAppendMs) : -1;

  lines << QString("grid: %1x%2  append: %3 ms")
               .arg(gridWidth)
               .arg(gridHeight)
               .arg(cadenceMs)
        << QString("tick: %1  range: %2 → %3")
               .arg(snapshot.tickSize, 0, 'f', 6)
               .arg(snapshot.minPrice, 0, 'f', 4)
               .arg(snapshot.maxPrice, 0, 'f', 4)
        << QString("last slice: %1  age: %2 ms")
               .arg(snapshot.lastSliceStartMs)
               .arg(ageMs)
        << QString("pending uploads: %1  ring cursor: %2/%3")
               .arg(pendingUploads)
               .arg(writeColumn)
               .arg(gridWidth)
        << QString("liquidity labels: %1")
               .arg(snapshot.liquidityAvailable ? "yes" : "no");

  if (snapshot.timeOriginMs != 0) {
    lines << QString("time origin: %1").arg(snapshot.timeOriginMs);
  }
  if (snapshot.streamBaseMs != std::numeric_limits<int64_t>::min()) {
    lines << QString("stream base: %1").arg(snapshot.streamBaseMs);
  }

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
  if (m_gpuHeatmap && priceMax > priceMin) {
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
    if (m_heatmapStreamService) m_heatmapStreamService->cancelPriceCenter();
  }
  onViewChanged(timeStart, timeEnd, priceMin, priceMax);
  if (m_gpuHeatmap) syncGpuView(); // also when the values were unchanged (price now known)
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
  m_panSyncPending = false;
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
