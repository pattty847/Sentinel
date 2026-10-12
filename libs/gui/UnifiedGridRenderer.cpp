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

chart::SurfaceFacts chart::HeatmapChartPort::surface() const {
  if (!layer_) return {};
  return {item_.width(), item_.height(), item_.window() ? item_.window()->effectiveDevicePixelRatio() : 1.0};
}

bool chart::HeatmapChartPort::visible() const { return item_.isVisible(); }

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

  m_layers.heatmap.queueUpdate = [this] {
      QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
  };
  m_layers.heatmap.queueLabelCheck = [this] {
      QMetaObject::invokeMethod(this, [this] {
          if (m_layers.heatmap.labelsChangedSinceDraw()) update();
      }, Qt::QueuedConnection);
  };
  setAcceptHoverEvents(false); // Reduce event capture
  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* w) { bindWindow(w); });

  init();
  bindZoomGlide();
  bindViewReactions(ReactionPhase::Ready);
  // An item constructed with a parent already in a window (C++ hosts, tests)
  // saw windowChanged before the connection above.
  if (window()) bindWindow(window());
}

void UnifiedGridRenderer::bindViewReactions(ReactionPhase phase) {
  auto* commands = &m_view->commands();
  // Initial reactions must precede refits. Attach refits only after init finishes,
  // so construction-time signals retain their original listeners as well.
  if (phase == ReactionPhase::Heatmap) {
    connect(commands, &chart::ViewCommands::updateRequested, this, &QQuickItem::update, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::liveRenderTick, this, &UnifiedGridRenderer::liveRenderTick, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::heatmapTickSizeChanged, this, &UnifiedGridRenderer::heatmapTickSizeChanged, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::viewportChanged, this, &UnifiedGridRenderer::viewportChanged, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::autoPriceScaleChanged, this, &UnifiedGridRenderer::autoPriceScaleChanged, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, commands, &chart::ViewCommands::onHeatmapSnapshot, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, commands, &chart::ViewCommands::onHeatmapLive, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::tickChanged, commands, &chart::ViewCommands::onHeatmapTick, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::limitsChanged, commands, &chart::ViewCommands::onHeatmapLimits, Qt::DirectConnection);
  } else if (phase == ReactionPhase::ViewState) {
    connect(getViewState(), &GridViewState::autoPriceScaleChanged, commands, &chart::ViewCommands::onAutoPriceScaleChanged, Qt::DirectConnection);
  } else {
    connect(getViewState(), &GridViewState::autoPriceScaleChanged, commands, &chart::ViewCommands::clearAutoPriceTick, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, this, &UnifiedGridRenderer::refitAutoPrice);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, this, &UnifiedGridRenderer::refitAutoPrice);
  }
}

void UnifiedGridRenderer::bindLivePlacement() {
  m_view->priceFitter().setRequestUpdate([this] { update(); });
  auto& live = m_view->livePlacement();
  connect(&live, &chart::LivePlacement::liveRenderTick, this, &UnifiedGridRenderer::liveRenderTick, Qt::DirectConnection);
  connect(&live, &chart::LivePlacement::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged, Qt::DirectConnection);
  connect(&live, &chart::LivePlacement::updateRequested, this, &QQuickItem::update, Qt::DirectConnection);
}

void UnifiedGridRenderer::bindTimeframeSwitch() {
  m_view->timeframeSwitch().setHooks(
      [this](int tf) { m_timeAuthority.setActiveTimeframeMs(tf); },
      [this](int tf) { m_layers.heatmap.gpu->setTimeframeMs(tf); });
  connect(m_view.get(), &chart::ChartViewController::liveRenderTick, this,
          &UnifiedGridRenderer::liveRenderTick, Qt::DirectConnection);
  connect(m_view.get(), &chart::ChartViewController::timeframeChanged, this, [this](int tf) {
    if (getDataProcessor()) {
      QMetaObject::invokeMethod(getDataProcessor(),
          [this, tf]() { getDataProcessor()->setTimeframe(tf); }, Qt::QueuedConnection);
    }
    update();
    emit timeframeChanged();
  }, Qt::DirectConnection);
}

void UnifiedGridRenderer::bindWindow(QQuickWindow* w) {
  if (m_layers.text.axes) {
    m_layers.text.axes->bindAxisLayoutWindow(w);
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
  connect(w, &QQuickWindow::afterRendering, this, [this]() {
    m_frameAck.rendered();
    m_layers.registry.afterRendering();
  }, Qt::DirectConnection);
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
  if (m_glidePolishTimer) {
    m_glidePolishTimer->stop();
    disconnect(m_glidePolishTimer, nullptr, this, nullptr);
  }
  if (m_view) {
    m_view->zoom().stop();
    disconnect(&m_view->zoom(), nullptr, this, nullptr);
    m_view->priceCarry().stop();
    disconnect(m_layers.heatmap.gpu.get(), nullptr, &m_view->commands(), nullptr);
    disconnect(getViewState(), nullptr, &m_view->commands(), nullptr);
    disconnect(&m_view->commands(), nullptr, this, nullptr);
    disconnect(m_view.get(), nullptr, this, nullptr);
    m_view->livePlacement().stopBootstrap();
    disconnect(&m_view->livePlacement(), nullptr, this, nullptr);
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
  m_layers.heatmap.gpu.reset(); // destroys its controller on the heatmap-data thread
  // Queued processor callbacks may still use getDataProcessor() until the join.
  m_feeds->shutdown();
  m_feeds.reset();
}

void UnifiedGridRenderer::onTradeReceived(const Trade &trade) {
  if (m_view->session().symbol == QLatin1String(trade.product_id.data(), qsizetype(trade.product_id.size()))) {
    if (m_bubbles.append(trade))
      update(); // Qt coalesces trade bursts into one scene synchronization
  }
  m_view->livePlacement().onTrade(trade.product_id, trade.price);
}

void UnifiedGridRenderer::setLiveBookTop(double bestBid, double bestAsk) {
  m_view->livePlacement().setLiveBookTop(bestBid, bestAsk);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs,
                                        double minPrice, double maxPrice) { m_view->commands().onViewChanged(startTimeMs, endTimeMs, minPrice, maxPrice); }

void UnifiedGridRenderer::onViewportChanged() { m_view->commands().onViewportChanged(); }

void UnifiedGridRenderer::setPriceAxisSource(QObject *source) {
  if (m_layers.text.axes) {
    m_layers.text.axes->setPriceAxisSource(source);
  }
}

void UnifiedGridRenderer::setTimeAxisSource(QObject *source) {
  if (m_layers.text.axes) {
    m_layers.text.axes->setTimeAxisSource(source);
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
    if (m_layers.text.axes) {
      m_layers.text.axes->refreshAxisLayout();
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
    m_view->livePlacement().resize(newGeometry.width());
    update();
  }
}

void UnifiedGridRenderer::componentComplete() {
  QQuickItem::componentComplete();

  if (m_layers.text.axes) {
    m_layers.text.axes->bindAxisLayoutWindow(window());
  }
  if (getViewState() && width() > 0 && height() > 0) {
    getViewState()->setViewportSize(width(), height());
  }
  if (m_layers.text.axes) {
    m_layers.text.axes->refreshAxisLayout();
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
  m_bubbles.clear();
  if (getViewState()) {
    getViewState()->resetZoom();
  }
  m_feeds->clearData();
  update();
}

void UnifiedGridRenderer::setActiveSymbol(const QString& symbol) {
  const QString normalized = symbol.trimmed().toUpper();
  if (m_view->session().symbol == normalized) {
    return;
  }
  sLog_Render("active symbol changed, clearing chart data: prev=" << m_view->session().symbol
              << " symbol=" << normalized);
  m_view->zoomGlide().land();
  m_view->priceCarry().capture();
  m_view->session().symbol = normalized;
  m_view->session().bookMid = m_view->session().lastTrade = 0.0;
  // The controller serial switches the heatmap (the node holds the old picture
  // until the new spans are ready); clearData() resets the trade overlays. The
  // next book top centres the new price.
  m_view->priceCarry().stop(); // the live-only wait is per symbol
  // Reseed first: the layer's limitsChanged (setSymbol) must not apply a pending carry
  // before the new symbol has a price of its own.
  m_view->flags().reseedPrice = true;
  if (m_layers.heatmap.gpu) m_layers.heatmap.gpu->setSymbol(normalized.toStdString());
  clearData();
  refitAutoPrice(); // auto price scale on: the new symbol's candles when already held
  if (getDataProcessor()) {
    QMetaObject::invokeMethod(getDataProcessor(),
                              [processor = getDataProcessor(), normalized]() {
                                processor->setActiveSymbol(normalized);
                              },
                              Qt::QueuedConnection);
  }
}

// The configured TPO style, session and bracket (startup, and the settings
// dialog's TPO reset).
void UnifiedGridRenderer::applyTpoConfig(const ClientTpoConfig& config) {
  TpoOverlayRenderer::Style style = m_layers.tpo.renderer.style();
  style.layout = tpo::parseLayout(config.layout, tpo::Layout::Collapsed);
  style.theme = tpo::parseTheme(config.theme, tpo::Theme::Rainbow);
  style.rowPx = config.rowPx;
  style.maxSessions = config.sessions;
  m_layers.tpo.renderer.setStyle(style);
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
              << " periodMs=" << period << " sessions=" << m_layers.tpo.renderer.style().maxSessions
              << " rowPx=" << m_layers.tpo.renderer.style().rowPx);
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
  auto style = m_layers.tpo.renderer.style();
  const auto next = tpo::parseLayout(layout.toStdString(), style.layout);
  if (next == style.layout) return;
  style.layout = next;
  m_layers.tpo.renderer.setStyle(style);
  sLog_Render("TPO layout=" << tpo::layoutName(next));
  update();
  emit tpoStyleChanged();
}

void UnifiedGridRenderer::setTpoTheme(const QString& theme) {
  auto style = m_layers.tpo.renderer.style();
  const auto next = tpo::parseTheme(theme.toStdString(), style.theme);
  if (next == style.theme) return;
  style.theme = next;
  m_layers.tpo.renderer.setStyle(style);
  sLog_Render("TPO theme=" << tpo::themeName(next));
  update();
  emit tpoStyleChanged();
}

QString UnifiedGridRenderer::tpoLayout() const {
  return QString::fromLatin1(tpo::layoutName(m_layers.tpo.renderer.style().layout));
}

QString UnifiedGridRenderer::tpoTheme() const {
  return QString::fromLatin1(tpo::themeName(m_layers.tpo.renderer.style().theme));
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
  m_view->setTimeframe(timeframe_ms);
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
  m_view->livePlacement().enableAutoScroll(enabled);
}

// ── GPU heatmap renderer ──
double UnifiedGridRenderer::heatmapTickSize() const {
  return m_layers.heatmap.gpu ? m_layers.heatmap.gpu->tickPrice() : 0.0;
}

void UnifiedGridRenderer::setHeatmapService(heatmap::HeatmapDataService* service) {
  if (!m_layers.heatmap.gpu) return;
  m_layers.heatmap.gpu->setService(service);
  m_view->livePlacement().bootstrapGpuTimeView(); // no view yet: wait for this service's availability
}

void UnifiedGridRenderer::setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings,
                                                  bool explicitManualTick) {
  if (!m_layers.heatmap.gpu) return;
  m_layers.heatmap.gpu->setSettings(settings, explicitManualTick);
  refitAutoPrice();
  m_bubbles.setSettings(settings, [this] { emit tradeBubbleSettingsChanged(); });
  m_view->applyGpuLimits();
  update();
}

void UnifiedGridRenderer::setHeatmapTickMemory(const heatmap::ManualTickMemory& memory) {
  if (m_layers.heatmap.gpu) m_layers.heatmap.gpu->setTickMemory(memory);
}

// The GPU palette's tone mapping: gamma, contrast and shader floor.
void UnifiedGridRenderer::syncGpuTone() {
  if (m_layers.heatmap.gpu) {
    m_layers.heatmap.gpu->setTone({static_cast<float>(m_heatmapGamma), static_cast<float>(m_heatmapContrast),
                         static_cast<float>(m_heatmapShaderFloor)});
  }
}

void UnifiedGridRenderer::syncGpuSurface() {
  if (!m_layers.heatmap.gpu) return;
  const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  m_layers.heatmap.gpu->setSurface(width(), height(), dpr);
  refitAutoPrice(); // raw row height and candidate pixel phases follow the surface
}

// ── Auto-fit ────────────────────────────────────────────────────────────────
QObject* UnifiedGridRenderer::candleBuffer() const { return m_candleSource.buffer(); }

void UnifiedGridRenderer::setCandleBuffer(QObject* buffer) {
  auto* candles = qobject_cast<CandleSeriesBuffer*>(buffer);
  if (m_candleSource.buffer() == candles) return;
  if (m_candleDirtyConn) disconnect(m_candleDirtyConn);
  m_candleSource.setBuffer(candles);
  m_view->priceFitter().setCandleSource(candles ? &m_candleSource : nullptr);
  if (candles) {
    // Auto price scale: a candle update or history page in view refits price (a few
    // compares when it is off or out of view; a bump only when the fit changed).
    m_candleDirtyConn = connect(candles, &CandleSeriesBuffer::candlesDirty, this,
                                [this](const QString& symbol, int64_t timeframeSec, qint64 dirtyStart,
                                       qint64 dirtyEnd) {
                                  m_view->priceFitter().candlesDirty(symbol, timeframeSec, dirtyStart, dirtyEnd);
                                });
  }
  refitAutoPrice();
  emit candleBufferChanged();
}

void UnifiedGridRenderer::refitAutoPrice() { m_view->priceFitter().refitAutoPrice(); }

void UnifiedGridRenderer::setAutoPriceScale(bool enabled) { m_view->commands().setAutoPriceScale(enabled); }

bool UnifiedGridRenderer::fitView(bool time, bool price) { return m_view->commands().fitView(time, price); }

QString UnifiedGridRenderer::applyViewportRequest(const ViewportRequest& request) { return m_view->commands().applyViewportRequest(request); }

bool UnifiedGridRenderer::resetView() { return m_view->commands().resetView(); }


void UnifiedGridRenderer::applyClientConfig(const ClientConfig &config) {
  setHeatmapGamma(config.heatmap.gamma);
  setHeatmapContrast(config.heatmap.contrast);
  setHeatmapShaderFloor(config.heatmap.shaderFloor);
  syncGpuTone();
  m_view->initialViewSettings().columnPx = std::clamp(config.heatmap.initialColumnPx, 2, 64);
  m_view->initialViewSettings().pricePct = std::clamp(config.heatmap.initialPricePct, 0, 100);
  if (m_layers.text.axes) {
    m_layers.text.axes->setAxisLabelPxOverride(config.gui.axisLabelPx);
    m_layers.text.axes->refreshAxisLayout();
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

QString UnifiedGridRenderer::getMsdfAtlasMemory() const { return m_layers.text.getMsdfAtlasMemory(); }

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
  m_view->zoom().wheelZoom(angle, event->pixelDelta().isNull(), event->phase(),
            event->source() == Qt::MouseEventSynthesizedBySystem,
            at.x(), at.y(), !shift, true, chart::ZoomController::WheelRoute::Chart);
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
  if (!m_layers.heatmap.gpu) {
    lines << "layer: N/A";
    return lines.join('\n');
  }
  const qint64 received = m_layers.heatmap.gpu->liveReceivedAtMs();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  lines << QString("symbol: %1  tf: %2 ms  tick: %3")
               .arg(QString::fromStdString(m_layers.heatmap.gpu->symbol()))
               .arg(m_layers.heatmap.gpu->tfMs())
               .arg(m_layers.heatmap.gpu->tickPrice(), 0, 'f', 6)
        << QString("live open end: %1  age: %2 ms")
               .arg(m_layers.heatmap.gpu->liveOpenEndMs())
               .arg(received > 0 ? now - received : -1)
        << QString("controller: %1").arg(m_layers.heatmap.gpu->controller() ? "yes" : "no");
  return lines.join('\n');
}
QString UnifiedGridRenderer::getPerformanceStats() const {
  return PerformanceMonitor::instance().frameStatsText();
}

void UnifiedGridRenderer::addTrade(const Trade &trade) {
  onTradeReceived(trade);
}
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) { m_view->commands().setViewport(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::zoomIn() { zoomClicks(1, width() / 2, height() / 2); }
void UnifiedGridRenderer::zoomOut() { zoomClicks(-1, width() / 2, height() / 2); }
void UnifiedGridRenderer::resetZoom() { m_view->commands().resetZoom(); }
void UnifiedGridRenderer::beginPanAt(double x, double y) { m_view->commands().beginPanAt(x, y); }
void UnifiedGridRenderer::updatePanAt(double x, double y) { m_view->commands().updatePanAt(x, y); }
void UnifiedGridRenderer::endPanAt() { m_view->commands().endPanAt(); }
void UnifiedGridRenderer::panLeft() { m_view->commands().panLeft(); }
void UnifiedGridRenderer::panRight() { m_view->commands().panRight(); }
void UnifiedGridRenderer::panUp() { m_view->commands().panUp(); }
void UnifiedGridRenderer::panDown() { m_view->commands().panDown(); }
