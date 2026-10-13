// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "mainwindow/ChartAgentPort.hpp"
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
#include "chart/shell/ChartDebugText.hpp"
#include "chart/shell/ChartInputRouter.hpp"
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
  m_qml.view.setParent(this);
  m_qml.debug.setParent(this);
  m_visibility.setParent(this);
  m_look.setParent(this);
  setFlag(ItemHasContents, true);
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setFlag(ItemAcceptsInputMethod, true);

  m_visibility.report = [this](std::string_view field, bool enabled, bool unchanged) {
    if (field == "primaryField") {
      sLog_Render("layers: primaryField=" << m_visibility.primaryField()
                  << " -> heatmap=" << m_visibility.heatmapEnabled()
                  << " footprint=" << m_visibility.footprintEnabled()
                  << " tpo=" << m_visibility.tpoEnabled()
                  << " volumeProfile=" << m_visibility.volumeProfileEnabled());
    } else if (unchanged) {
      sLog_Render("layers: set " << field.data() << "=" << enabled << " unchanged");
    } else {
      sLog_Render("layers: set " << field.data() << "=" << enabled
                  << " -> heatmap=" << m_visibility.heatmapEnabled()
                  << " footprint=" << m_visibility.footprintEnabled()
                  << " tpo=" << m_visibility.tpoEnabled()
                  << " volumeProfile=" << m_visibility.volumeProfileEnabled());
    }
  };
  m_visibility.requestUpdate = [this] { update(); };
  m_look.requestUpdate = [this] { update(); };
  m_look.onViewportChanged = [this] { onViewportChanged(); };
  m_look.applyTone = [this](float gamma, float contrast, float floor) {
    if (m_layers.heatmap.gpu) m_layers.heatmap.gpu->setTone({gamma, contrast, floor});
  };


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
  m_qml.view.bind(*m_view, *m_heatmapPort);
  m_qml.debug.bind(m_layers, m_frames.published, getViewState());
  m_look.tradesAboveCandlesSource = [this] { return m_bubbles.tradesAboveCandles(); };
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
    connect(commands, &chart::ViewCommands::heatmapTickSizeChanged, view(), &chart::ChartViewQml::heatmapTickSizeChanged, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::viewportChanged, view(), &chart::ChartViewQml::viewportChanged, Qt::DirectConnection);
    connect(commands, &chart::ViewCommands::autoPriceScaleChanged, view(), &chart::ChartViewQml::autoPriceScaleChanged, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, commands, &chart::ViewCommands::onHeatmapSnapshot, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, commands, &chart::ViewCommands::onHeatmapLive, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::tickChanged, commands, &chart::ViewCommands::onHeatmapTick, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::limitsChanged, commands, &chart::ViewCommands::onHeatmapLimits, Qt::DirectConnection);
  } else if (phase == ReactionPhase::ViewState) {
    connect(getViewState(), &GridViewState::autoPriceScaleChanged, commands, &chart::ViewCommands::onAutoPriceScaleChanged, Qt::DirectConnection);
  } else {
    connect(getViewState(), &GridViewState::autoPriceScaleChanged, commands, &chart::ViewCommands::clearAutoPriceTick, Qt::DirectConnection);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, this, &UnifiedGridRenderer::refitAutoPrice);
    connect(m_layers.heatmap.gpu.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, commands, &chart::ViewCommands::refitAfterHeatmapLive);
  }
}

void UnifiedGridRenderer::bindLivePlacement() {
  m_view->priceFitter().setRequestUpdate([this] { update(); });
  auto& live = m_view->livePlacement();
  connect(&live, &chart::LivePlacement::liveRenderTick, this, &UnifiedGridRenderer::liveRenderTick, Qt::DirectConnection);
  connect(&live, &chart::LivePlacement::autoScrollEnabledChanged, view(), &chart::ChartViewQml::autoScrollEnabledChanged, Qt::DirectConnection);
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
    emit view()->timeframeChanged();
  }, Qt::DirectConnection);
}

void UnifiedGridRenderer::bindWindow(QQuickWindow* w) {
  if (m_layers.text.axes) {
    m_layers.text.axes->bindAxisLayoutWindow(w);
  }
  syncGpuSurface(); // the device pixel ratio sets the 1 column/px clamp
  if (m_connections.screenChanged) disconnect(m_connections.screenChanged);
  if (m_connections.render.beforeSync) disconnect(m_connections.render.beforeSync);
  if (m_connections.render.afterRender) disconnect(m_connections.render.afterRender);
  if (!w) return;
  // The frame's raster camera is computed and published before any item syncs
  // (render thread, GUI blocked; prepareSyncFrame).
  m_connections.render.beforeSync = connect(w, &QQuickWindow::beforeSynchronizing, this, [this] { prepareSyncFrame(); },
                             Qt::DirectConnection);
  // A move to a screen of another device pixel ratio: the clamps follow it, and the
  // controller's drawn window (the raster camera re-rounds in the next frame).
  m_connections.screenChanged = connect(w, &QQuickWindow::screenChanged, this, [this] {
    syncGpuSurface();
    m_view->syncGpuView();
    update();
  });
  m_connections.render.afterRender = connect(w, &QQuickWindow::afterRendering, this, [this]() {
    m_frames.ack.rendered();
    m_layers.registry.afterRendering();
  }, Qt::DirectConnection);
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
  if (m_connections.render.beforeSync) disconnect(m_connections.render.beforeSync);
  if (m_connections.render.afterRender) disconnect(m_connections.render.afterRender);
  if (m_clock.glidePolish) {
    m_clock.glidePolish->stop();
    disconnect(m_clock.glidePolish, nullptr, this, nullptr);
  }
  if (m_view) {
    m_view->zoom().stop();
    disconnect(&m_view->zoom(), nullptr, this, nullptr);
    m_view->priceCarry().stop();
    disconnect(m_layers.heatmap.gpu.get(), nullptr, &m_view->commands(), nullptr);
    disconnect(getViewState(), nullptr, &m_view->commands(), nullptr);
    disconnect(&m_view->commands(), nullptr, this, nullptr);
    disconnect(&m_view->commands(), nullptr, view(), nullptr);
    disconnect(m_view.get(), nullptr, this, nullptr);
    m_view->livePlacement().stopBootstrap();
    disconnect(&m_view->livePlacement(), nullptr, this, nullptr);
    disconnect(&m_view->livePlacement(), nullptr, view(), nullptr);
    disconnect(&m_view->zoomGlide(), nullptr, this, nullptr);
    disconnect(&m_view->zoomGlide(), nullptr, view(), nullptr);
    disconnect(getViewState(), nullptr, this, nullptr);
    disconnect(getViewState(), nullptr, view(), nullptr);
    getViewState()->setPanShift({});
  }
  // ~QQuickItem leaves the window (windowChanged) after this class's members are gone:
  // unbind the window now, while they exist.
  disconnect(this, &QQuickItem::windowChanged, this, nullptr);
  if (m_connections.screenChanged) disconnect(m_connections.screenChanged);
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

// ── GPU heatmap renderer ──
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
  m_bubbles.setSettings(settings, [this] { emit candles()->tradeBubbleSettingsChanged(); });
  m_view->applyGpuLimits();
  update();
}

void UnifiedGridRenderer::setHeatmapTickMemory(const heatmap::ManualTickMemory& memory) {
  if (m_layers.heatmap.gpu) m_layers.heatmap.gpu->setTickMemory(memory);
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
  if (m_connections.candleDirty) disconnect(m_connections.candleDirty);
  m_candleSource.setBuffer(candles);
  m_view->priceFitter().setCandleSource(candles ? &m_candleSource : nullptr);
  if (candles) {
    // Auto price scale: a candle update or history page in view refits price (a few
    // compares when it is off or out of view; a bump only when the fit changed).
    m_connections.candleDirty = connect(candles, &CandleSeriesBuffer::candlesDirty, this,
                                [this](const QString& symbol, int64_t timeframeMs, qint64 dirtyStart,
                                       qint64 dirtyEnd) {
                                  m_view->priceFitter().candlesDirty(symbol, timeframeMs, dirtyStart, dirtyEnd);
                                });
  }
  refitAutoPrice();
  emit candleBufferChanged();
}
void UnifiedGridRenderer::refitAutoPrice() { m_view->priceFitter().refitAutoPrice(); }
void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { chart::ChartInputRouter(*this, *m_view).mousePress(event); }
void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) { chart::ChartInputRouter(*this, *m_view).mouseMove(event); }
void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) { chart::ChartInputRouter(*this, *m_view).mouseRelease(event); }
void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { chart::ChartInputRouter(*this, *m_view).wheel(event); }
MappingFrameContext UnifiedGridRenderer::currentFrameContext() const { return m_frames.published.current(); }
TimeAxisMapping UnifiedGridRenderer::currentTimeAxisMapping() const { return currentFrameContext().mapping; }
void UnifiedGridRenderer::addTrade(const Trade &trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::applyClientConfig(const ClientConfig& config) { m_look.applyClientConfig(config, *m_view, m_layers.text.axes.get()); }
void UnifiedGridRenderer::applyServerConfig(const ServerConfig& config) { m_look.applyServerConfig(config, *m_view); }

ChartAgentPort UnifiedGridRenderer::agentPort() {
  return {*m_view, m_frames.published, m_frames.ack,
          [this] { update(); }};
}
