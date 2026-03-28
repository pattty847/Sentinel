// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include "config/GuiConfigStore.hpp"
#include "render/DataProcessor.hpp"
#include "render/GridViewState.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapLabelRenderer.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/ViewportAutoScrollController.hpp"
#include <QMetaObject>
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
  connect(this, &QQuickItem::windowChanged, this,
          [this](QQuickWindow* w) {
              if (m_axisTextService) {
                  m_axisTextService->bindAxisLayoutWindow(w);
              }
          });

  init();
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
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
  Q_UNUSED(trade);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs,
                                        double minPrice, double maxPrice) {
  if (m_viewState) {
    m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
  }

  update();

  sLog_Debug("UNIFIED RENDERER VIEWPORT Time:["
             << startTimeMs << "-" << endTimeMs << "]"
             << "Price:[$" << minPrice << "-$" << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
  if (!m_viewState || !m_dataProcessor)
    return;
  update();
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
    sLog_Render("UNIFIED RENDERER GEOMETRY CHANGED: "
                << newGeometry.width() << "x" << newGeometry.height());

    if (m_viewState) {
      m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
    }
    if (m_axisTextService) {
      m_axisTextService->refreshAxisLayout();
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

void UnifiedGridRenderer::setTpoTimeframeMs(int timeframeMs) {
  const int clamped = (timeframeMs == 1800000) ? 1800000 : 900000;
  if (m_tpoTimeframeMs == clamped) {
    return;
  }
  m_tpoTimeframeMs = clamped;
  emit tpoConfigChanged();
}

void UnifiedGridRenderer::setTpoSessionType(int sessionType) {
  const int clamped = (sessionType >= 0 && sessionType <= 5) ? sessionType : 4;
  if (m_tpoSessionType == clamped) {
    return;
  }
  m_tpoSessionType = clamped;
  emit tpoConfigChanged();
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
    m_currentTimeframe_ms = timeframe_ms;
    if (m_useGpuHeatmap && timeframe_ms > 0 && m_heatmapStreamService) {
      m_heatmapStreamService->handleTimeframeChange(static_cast<int64_t>(timeframe_ms));
    }
    m_manualTimeframeSet = true;
    m_manualTimeframeTimer.start();
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
  update();
  emit heatmapGammaChanged();
}

void UnifiedGridRenderer::setHeatmapContrast(double contrast) {
  const double clamped = std::clamp(contrast, 0.1, 5.0);
  if (std::abs(m_heatmapContrast - clamped) < 1e-6) {
    return;
  }
  m_heatmapContrast = clamped;
  update();
  emit heatmapContrastChanged();
}

void UnifiedGridRenderer::setHeatmapShaderFloor(double floor) {
  const double clamped = std::clamp(floor, 0.0, 0.5);
  if (std::abs(m_heatmapShaderFloor - clamped) < 1e-6) {
    return;
  }
  m_heatmapShaderFloor = clamped;
  update();
  emit heatmapShaderFloorChanged();
}

void UnifiedGridRenderer::setCandleStyle(int style) {
    style = std::clamp(style, 0, 2);
    if (m_candleStyle == style) return;
    m_candleStyle = style;
    emit candleStyleChanged();
}

void UnifiedGridRenderer::setHeatmapColorPreset(const QString& preset) {
    using CS = HeatmapOverlayRenderer::ColorStop;
    struct Preset {
        std::vector<CS> bid;
        std::vector<CS> ask;
        double gamma = 2.0;
    };

    static const QHash<QString, Preset> kPresets = {
        // Electric (default): cyan bids, orange asks
        {"Electric", {
            {{0.0f, QColor(0,0,0)}, {0.3f, QColor(0,30,60)}, {0.7f, QColor(0,180,220)}, {1.0f, QColor(0,255,255)}},
            {{0.0f, QColor(0,0,0)}, {0.3f, QColor(60,20,0)}, {0.7f, QColor(220,120,0)}, {1.0f, QColor(255,200,0)}},
            2.0
        }},
        // Fire: dark red → orange → yellow asks; cyan bids stay subtle
        {"Fire", {
            {{0.0f, QColor(0,0,0)}, {0.5f, QColor(20,40,80)}, {1.0f, QColor(60,120,200)}},
            {{0.0f, QColor(0,0,0)}, {0.3f, QColor(80,0,0)}, {0.6f, QColor(200,60,0)}, {0.85f, QColor(255,140,0)}, {1.0f, QColor(255,255,0)}},
            1.8
        }},
        // Ocean: deep blue → teal → white bids; green asks
        {"Ocean", {
            {{0.0f, QColor(0,0,0)}, {0.3f, QColor(0,20,80)}, {0.7f, QColor(0,100,180)}, {1.0f, QColor(0,220,255)}},
            {{0.0f, QColor(0,0,0)}, {0.4f, QColor(0,60,40)}, {0.8f, QColor(0,180,100)}, {1.0f, QColor(0,255,160)}},
            2.2
        }},
        // Monochrome: white bids, gray asks
        {"Monochrome", {
            {{0.0f, QColor(0,0,0)}, {0.5f, QColor(80,80,80)}, {1.0f, QColor(220,220,220)}},
            {{0.0f, QColor(0,0,0)}, {0.5f, QColor(50,50,50)}, {1.0f, QColor(140,140,140)}},
            2.0
        }},
        // Matrix: green only
        {"Matrix", {
            {{0.0f, QColor(0,0,0)}, {0.4f, QColor(0,40,0)}, {1.0f, QColor(0,255,0)}},
            {{0.0f, QColor(0,0,0)}, {0.4f, QColor(0,20,0)}, {1.0f, QColor(0,180,0)}},
            1.6
        }},
    };

    auto it = kPresets.find(preset);
    if (it == kPresets.end()) {
        return;
    }
    m_heatmapOverlay.setBidGradient(it->bid);
    m_heatmapOverlay.setAskGradient(it->ask);
    m_heatmapOverlay.setPaletteGamma(it->gamma);
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
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Render("PrimaryField set to " << m_primaryField);
  }
  update();
  emit primaryFieldChanged();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setHeatmapLayerEnabled(bool enabled) {
  const bool newTpoEnabled = enabled ? false : m_tpoLayerEnabled;
  const bool newVpEnabled = enabled ? false : m_volumeProfileLayerEnabled;
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Debug(QString("setHeatmapLayerEnabled request: enabled=%1 "
                       "current_hm=%2 current_fp=%3 current_tpo=%4")
                   .arg(enabled ? 1 : 0)
                   .arg(m_heatmapLayerEnabled ? 1 : 0)
                   .arg(m_footprintLayerEnabled ? 1 : 0)
                   .arg(m_tpoLayerEnabled ? 1 : 0));
  }
  if (m_heatmapLayerEnabled == enabled && m_tpoLayerEnabled == newTpoEnabled &&
      m_volumeProfileLayerEnabled == newVpEnabled) {
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
      sLog_Debug("setHeatmapLayerEnabled no-op");
    }
    return;
  }
  m_heatmapLayerEnabled = enabled;
  if (enabled) {
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Render("Heatmap layer " << (enabled ? "enabled" : "disabled"));
  }
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setFootprintLayerEnabled(bool enabled) {
  const bool newTpoEnabled = enabled ? false : m_tpoLayerEnabled;
  const bool newVpEnabled = enabled ? false : m_volumeProfileLayerEnabled;
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Debug(QString("setFootprintLayerEnabled request: enabled=%1 "
                       "current_hm=%2 current_fp=%3 current_tpo=%4")
                   .arg(enabled ? 1 : 0)
                   .arg(m_heatmapLayerEnabled ? 1 : 0)
                   .arg(m_footprintLayerEnabled ? 1 : 0)
                   .arg(m_tpoLayerEnabled ? 1 : 0));
  }
  if (m_footprintLayerEnabled == enabled &&
      m_tpoLayerEnabled == newTpoEnabled &&
      m_volumeProfileLayerEnabled == newVpEnabled) {
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
      sLog_Debug("setFootprintLayerEnabled no-op");
    }
    return;
  }
  m_footprintLayerEnabled = enabled;
  if (enabled) {
    m_tpoLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Render("Footprint layer " << (enabled ? "enabled" : "disabled"));
  }
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::setTpoLayerEnabled(bool enabled) {
  const bool newHeatmapEnabled = enabled ? false : m_heatmapLayerEnabled;
  const bool newFootprintEnabled = enabled ? false : m_footprintLayerEnabled;
  const bool newVolumeProfileEnabled =
      enabled ? false : m_volumeProfileLayerEnabled;
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Debug(QString("setTpoLayerEnabled request: enabled=%1 current_hm=%2 "
                       "current_fp=%3 current_tpo=%4")
                   .arg(enabled ? 1 : 0)
                   .arg(m_heatmapLayerEnabled ? 1 : 0)
                   .arg(m_footprintLayerEnabled ? 1 : 0)
                   .arg(m_tpoLayerEnabled ? 1 : 0));
  }
  if (m_tpoLayerEnabled == enabled &&
      m_heatmapLayerEnabled == newHeatmapEnabled &&
      m_footprintLayerEnabled == newFootprintEnabled &&
      m_volumeProfileLayerEnabled == newVolumeProfileEnabled) {
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
      sLog_Debug("setTpoLayerEnabled no-op");
    }
    return;
  }
  m_tpoLayerEnabled = enabled;
  if (enabled) {
    m_heatmapLayerEnabled = false;
    m_footprintLayerEnabled = false;
    m_volumeProfileLayerEnabled = false;
  }
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
    sLog_Render("TPO layer " << (enabled ? "enabled" : "disabled"));
  }
  update();
  emit layerVisibilityChanged();
}

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
  if (m_viewState) {
    m_viewState->enableAutoScroll(enabled);
    update();
    emit autoScrollEnabledChanged();
    sLog_Render("Auto-scroll: " << (enabled ? "ENABLED" : "DISABLED"));
    if (enabled && m_viewState->isTimeWindowValid() && m_heatmapStreamService) {
      m_heatmapStreamService->updateAutoScrollLag(
          *m_viewState,
          m_heatmapStreamService->timeAuthority().activeTimeframeMs());
    }
  }
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
  if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG")) {
    sLog_Debug(
        QString("Chart text atlas build: fontPx=%1 pxRange=%2 charset=%3")
            .arg(params.fontPx)
            .arg(params.pxRange, 0, 'f', 2)
            .arg(params.charset.size()));
  }
  if (m_chartTextAtlas.build(params)) {
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
  if (m_heatmapStreamService) {
    m_heatmapStreamService->setInitialViewportPct(config.heatmap.initialViewportPct);
    m_heatmapStreamService->setInitialPricePct(config.heatmap.initialPricePct);
  }
  if (m_axisTextService) {
    m_axisTextService->setAxisLabelPxOverride(config.gui.axisLabelPx);
    m_axisTextService->refreshAxisLayout();
  }
  if (config.heatmap.labelPx > 0 && config.heatmap.labelPx <= 128) {
    m_heatmapLabelPx = config.heatmap.labelPx;
  } else if (config.heatmap.labelPx > 128) {
    qWarning("Heatmap labelPx=%d exceeds sane maximum (128), using default %d",
             config.heatmap.labelPx, m_heatmapLabelPx);
  }
  update();
  if (m_dataProcessor && config.heatmap.clientCacheColumns > 0) {
    const int capacity = config.heatmap.clientCacheColumns;
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        [this, capacity]() {
          m_dataProcessor->setCacheCapacityOverride(capacity);
        },
        Qt::QueuedConnection);
  }
}

void UnifiedGridRenderer::applyServerConfig(const ServerConfig &config) {
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

  if (m_dataProcessor && config.heatmap.recenterDelta > 0.0) {
    const double recenter = config.heatmap.recenterDelta;
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        [this, recenter]() {
          m_dataProcessor->setHeatmapRecenterFraction(recenter);
        },
        Qt::QueuedConnection);
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
double UnifiedGridRenderer::getZoomFactor() const {
  return m_viewState ? m_viewState->getZoomFactor() : 1.0;
}
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
  return "N/A (SentinelMonitor removed)";
}
double UnifiedGridRenderer::getCurrentFPS() const {
  return m_currentFps.load();
}
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

void UnifiedGridRenderer::addTrade(const Trade &trade) {
  onTradeReceived(trade);
}
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) {
  onViewChanged(timeStart, timeEnd, priceMin, priceMax);
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
