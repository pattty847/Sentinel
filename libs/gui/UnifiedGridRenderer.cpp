// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "datasources/IGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QThread>
#include <QMetaObject>
#include <QMetaType>
#include <QTimer>
#include <QtEndian>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include "render/GridViewState.hpp"
#include "render/DataProcessor.hpp"
#include "render/MsdfAtlas.hpp"
#include "render/MsdfGlyphNode.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/ViewportAutoScrollController.hpp"
#include "render/HeatmapLabelRenderer.hpp"

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    setAcceptHoverEvents(false);  // Reduce event capture

    init();
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
    if (m_dataProcessor) {
        if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::stopProcessing, Qt::BlockingQueuedConnection);
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

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    Q_UNUSED(trade);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    if (m_viewState) {
        m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
    }
    
    update();
    
    sLog_Debug("UNIFIED RENDERER VIEWPORT Time:[" << startTimeMs << "-" << endTimeMs << "]"
               << "Price:[$" << minPrice << "-$" << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
    if (!m_viewState || !m_dataProcessor) return;
    update();
}


void UnifiedGridRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    
    if (newGeometry.size() != oldGeometry.size()) {
        sLog_Render("UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        if (m_viewState) {
            m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
        }
        update();
    }
}

void UnifiedGridRenderer::componentComplete() {
    QQuickItem::componentComplete();

    if (m_viewState && width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
    }
}

void UnifiedGridRenderer::setIntensityScale(double scale) {
    if (m_intensityScale != scale) {
        m_intensityScale = scale;
        if (m_useGpuHeatmap && m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, scale]() {
                m_dataProcessor->setHeatmapIntensityScale(scale);
            }, Qt::QueuedConnection);
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
        if (m_autoScrollController) {
            m_autoScrollController->setPaddingFrac(clamped);
            m_autoScrollController->resetSpan();
        }
        emit autoScrollPaddingFracChanged();
    }
}

void UnifiedGridRenderer::setAutoScrollSmoothEnabled(bool enabled) {
    if (m_smoothAutoScrollEnabled != enabled) {
        m_smoothAutoScrollEnabled = enabled;
        if (m_autoScrollController) {
            m_autoScrollController->setSmoothEnabled(enabled);
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
        QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::clearData, Qt::QueuedConnection);
    }
    update();
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_dataProcessor && resolution > 0) {
        QMetaObject::invokeMethod(m_dataProcessor.get(),
                                  [this, resolution]() { m_dataProcessor->setPriceResolution(resolution); },
                                  Qt::QueuedConnection);
        update();
    }
}

void UnifiedGridRenderer::setGridMode(int mode) {
    double priceRes[] = {2.5, 5.0, 10.0};
    int timeRes[] = {50, 100, 250};
    if (mode >= 0 && mode <= 2) {
        setPriceResolution(priceRes[mode]);
        setTimeframe(timeRes[mode]);
    }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
    if (m_currentTimeframe_ms != timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        if (m_useGpuHeatmap && timeframe_ms > 0) {
            if (m_heatmapStream) {
                m_heatmapStream->setAppendMs(timeframe_ms);
            }
        }
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        if (m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(),
                                      [this, timeframe_ms]() { m_dataProcessor->addTimeframe(timeframe_ms); },
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
    update();
    emit heatmapLiquidityThresholdChanged();
}

void UnifiedGridRenderer::setHeatmapBackgroundColor(const QColor& color) {
    if (m_heatmapBackgroundColor == color) {
        return;
    }
    m_heatmapBackgroundColor = color;
    m_heatmapTextureDirty = true;
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

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        update();
        emit autoScrollEnabledChanged();
        sLog_Render("Auto-scroll: "<< (enabled ? "ENABLED" : "DISABLED"));
        if (enabled && m_viewState->isTimeWindowValid() && m_heatmapStream && m_autoScrollController) {
            m_autoScrollController->updateLagFromView(*m_viewState, *m_heatmapStream);
        }
    }
}

//  COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport;
    viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
    viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
    viewport.priceMin = m_viewState->getMinPrice();
    viewport.priceMax = m_viewState->getMaxPrice();
    viewport.width = width();
    viewport.height = height();
    return CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
}

QPointF UnifiedGridRenderer::screenToWorld(double screenX, double screenY) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport;
    viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
    viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
    viewport.priceMin = m_viewState->getMinPrice();
    viewport.priceMax = m_viewState->getMaxPrice();
    viewport.width = width();
    viewport.height = height();
    return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
}

void UnifiedGridRenderer::init() {
    m_useGpuHeatmap = true;
    m_heatmapClock.start();

    bool ok = false;
    const int envWidth = qgetenv("SENTINEL_HEATMAP_GRID_WIDTH").toInt(&ok);
    if (ok && envWidth > 0) {
        m_heatmapGridWidth = envWidth;
    }
    ok = false;
    const int envHeight = qgetenv("SENTINEL_HEATMAP_GRID_HEIGHT").toInt(&ok);
    if (ok && envHeight > 0) {
        m_heatmapGridHeight = envHeight;
    } else {
        ok = false;
        const int envHeightLegacy = qgetenv("SENTINEL_HEATMAP_GRID").toInt(&ok);
        if (ok && envHeightLegacy > 0) {
            m_heatmapGridHeight = envHeightLegacy;
        }
    }
    qRegisterMetaType<Trade>("Trade");
    
    m_viewState = std::make_unique<GridViewState>(this);
    m_heatmapStream = std::make_unique<HeatmapStreamState>();
    m_heatmapStream->setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
    m_heatmapStream->setAppendMs(100);
    m_heatmapStream->setIntensityBytesPerCell(m_intensityBytesPerCell);
    m_autoScrollController = std::make_unique<ViewportAutoScrollController>();
    m_autoScrollController->setPaddingFrac(m_autoScrollPaddingFrac);
    m_autoScrollController->setSmoothEnabled(m_smoothAutoScrollEnabled);
    buildMsdfAtlas();
    m_bidGradient = ColorGradient{
        {0.0f, QColor(10, 40, 0)},      // Almost black green
        {0.5f, QColor(60, 160, 30)},    // Medium green
        {1.0f, QColor(100, 255, 50)}    // Electric neon green
    };
    m_askGradient = ColorGradient{
        {0.0f, QColor(40, 0, 0)},       // Dark red (almost black)
        {0.5f, QColor(180, 40, 20)},    // Medium red
        {0.85f, QColor(255, 100, 30)},  // Bright red-orange
        {1.0f, QColor(255, 200, 50)}    // Hot orange/yellow
    };
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessor = std::make_unique<DataProcessor>();  // No parent - will be moved to thread
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    if (m_useGpuHeatmap) {
        ok = false;
        const double recenter = qgetenv("SENTINEL_HEATMAP_RECENTER").toDouble(&ok);
        if (ok && recenter > 0.0) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, recenter]() {
                m_dataProcessor->setHeatmapRecenterFraction(recenter);
            }, Qt::QueuedConnection);
        }
        ok = false;
        const double gamma = qgetenv("SENTINEL_HEATMAP_GAMMA").toDouble(&ok);
        if (ok && gamma > 0.0) {
            m_heatmapGamma = gamma;
        }
        ok = false;
        const double contrast = qgetenv("SENTINEL_HEATMAP_CONTRAST").toDouble(&ok);
        if (ok && contrast > 0.0) {
            m_heatmapContrast = contrast;
        }
        ok = false;
        const double floorVal = qgetenv("SENTINEL_HEATMAP_SHADER_FLOOR").toDouble(&ok);
        if (ok && floorVal >= 0.0 && floorVal <= 1.0) {
            m_heatmapShaderFloor = floorVal;
        }
    }
    
    connect(m_dataProcessor.get(), &DataProcessor::heatmapColumnReady,
            this,
            [this](int64_t sliceStartMs,
                   int64_t sliceEndMs,
                   int64_t timeframeMs,
                   double minPrice,
                   double maxPrice,
                   double tickSize,
                   const QByteArray& column,
                   const QByteArray& liquidityColumn,
                   double liquidityScale,
                   int intensityBytesPerCell) {
                Q_UNUSED(sliceEndMs);
                const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapTextureDirty = true;
                    m_heatmapClock.start();
                }
                if (column.isEmpty()) {
                    if (debug) {
                        sLog_Render("GPU HEATMAP DROP: enabled=" << m_useGpuHeatmap
                                    << " grid=" << m_heatmapGridWidth << "x" << m_heatmapGridHeight
                                    << " bytes=" << column.size());
                    }
                    return;
                }
                const int bytesPerCell = (intensityBytesPerCell > 0) ? intensityBytesPerCell : 1;
                if (bytesPerCell != m_intensityBytesPerCell) {
                    m_intensityBytesPerCell = bytesPerCell;
                    m_heatmapTextureDirty = true;
                    if (m_heatmapStream) {
                        m_heatmapStream->setIntensityBytesPerCell(bytesPerCell);
                    }
                }
                if ((column.size() % bytesPerCell) != 0) {
                    return;
                }
                const int columnHeight = column.size() / bytesPerCell;
                if (columnHeight <= 0) {
                    return;
                }
                if (columnHeight != m_heatmapGridHeight) {
                    m_heatmapGridHeight = columnHeight;
                    m_heatmapTextureDirty = true;
                    if (m_heatmapStream) {
                        m_heatmapStream->reset(m_heatmapGridWidth, m_heatmapGridHeight,
                                               minPrice, maxPrice, tickSize);
                    }
                    m_heatmapViewportInitialized = false;
                }

                if (debug) {
                    static int debugCount = 0;
                    ++debugCount;
                    if (debugCount <= 5 || debugCount % 50 == 0) {
                        int bidCount = 0;
                        int askCount = 0;
                        uint16_t minValue = std::numeric_limits<uint16_t>::max();
                        uint16_t maxValue = 0;
                        if (bytesPerCell == 1) {
                            const auto* bytes = reinterpret_cast<const uint8_t*>(column.constData());
                            for (int i = 0; i < column.size(); ++i) {
                                const uint8_t v = bytes[i];
                                if (v == 0) {
                                    continue;
                                }
                                minValue = std::min<uint16_t>(minValue, static_cast<uint16_t>(v) * 257);
                                maxValue = std::max<uint16_t>(maxValue, static_cast<uint16_t>(v) * 257);
                                if (v >= 128) {
                                    ++askCount;
                                } else {
                                    ++bidCount;
                                }
                            }
                        } else if (bytesPerCell == 2) {
                            const auto* values = reinterpret_cast<const uint16_t*>(column.constData());
                            for (int i = 0; i < columnHeight; ++i) {
                                const uint16_t v = qFromLittleEndian(values[i]);
                                if (v == 0) {
                                    continue;
                                }
                                minValue = std::min(minValue, v);
                                maxValue = std::max(maxValue, v);
                                if (v >= 0x8000u) {
                                    ++askCount;
                                } else {
                                    ++bidCount;
                                }
                            }
                        }
                        sLog_Render("GPU HEATMAP BYTES: bids=" << bidCount
                                    << " asks=" << askCount
                                    << " min=" << minValue
                                    << " max=" << maxValue
                                    << " bpp=" << bytesPerCell);
                    }
                }

                if (tickSize > 0.0 && tickSize != m_heatmapTickSize) {
                    m_heatmapTickSize = tickSize;
                    emit heatmapTickSizeChanged();
                }
                if (m_heatmapStream) {
                    m_heatmapStream->updateRange(minPrice, maxPrice, tickSize);
                    if (timeframeMs > 0) {
                        m_heatmapStream->setAppendMs(static_cast<int>(timeframeMs));
                    }
                }

                const int expectedLiquidityBytes = m_heatmapGridHeight * static_cast<int>(sizeof(uint16_t));
                const bool haveLiquidityColumn = (liquidityColumn.size() == expectedLiquidityBytes);
                QByteArray intensityColumn = column;
                if (m_heatmapLiquidityThreshold > 0.0 && haveLiquidityColumn && liquidityScale > 0.0 &&
                    intensityColumn.size() == m_heatmapGridHeight * bytesPerCell) {
                    const auto* raw = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
                    const double threshold = m_heatmapLiquidityThreshold;
                    if (bytesPerCell == 1) {
                        auto* dst = reinterpret_cast<uint8_t*>(intensityColumn.data());
                        for (int y = 0; y < m_heatmapGridHeight; ++y) {
                            const uint16_t packed = qFromLittleEndian(raw[y]);
                            if (packed == 0) {
                                dst[y] = 0;
                                continue;
                            }
                            double value = static_cast<double>(packed) * liquidityScale;
                            if (m_liquidityLabelMode != 0) {
                                const double price = maxPrice - (static_cast<double>(y) * tickSize);
                                value *= price;
                            }
                            if (value < threshold) {
                                dst[y] = 0;
                            }
                        }
                    } else if (bytesPerCell == 2) {
                        auto* dst = reinterpret_cast<uint16_t*>(intensityColumn.data());
                        for (int y = 0; y < m_heatmapGridHeight; ++y) {
                            const uint16_t packed = qFromLittleEndian(raw[y]);
                            if (packed == 0) {
                                dst[y] = 0;
                                continue;
                            }
                            double value = static_cast<double>(packed) * liquidityScale;
                            if (m_liquidityLabelMode != 0) {
                                const double price = maxPrice - (static_cast<double>(y) * tickSize);
                                value *= price;
                            }
                            if (value < threshold) {
                                dst[y] = 0;
                            }
                        }
                    }
                }
                if (m_heatmapStream) {
                    const qint64 nowMs = m_heatmapClock.elapsed();
                    m_heatmapStream->ingestSlice(sliceStartMs,
                                                 static_cast<int>(timeframeMs),
                                                 intensityColumn,
                                                 liquidityColumn,
                                                 liquidityScale,
                                                 nowMs);
                    m_heatmapStream->updateTimeOffset(0.0f);
                }
                if (debug) {
                    const int writeColumn = m_heatmapStream ? m_heatmapStream->writeColumn() : 0;
                    sLog_Render("GPU HEATMAP ENQUEUE: col=" << writeColumn
                                << " tf=" << timeframeMs
                                << " range=$" << minPrice << "-$" << maxPrice);
                }
                if (!m_heatmapViewportInitialized && m_viewState && m_heatmapStream && m_autoScrollController) {
                    if (m_autoScrollController->initializeViewport(*m_viewState,
                                                                   *m_heatmapStream,
                                                                   sliceStartMs,
                                                                   static_cast<int>(timeframeMs))) {
                        m_heatmapViewportInitialized = true;
                        if (debug) {
                            const auto snapshot = m_heatmapStream->snapshot();
                            sLog_Render("GPU HEATMAP VIEWPORT INIT: [" << m_viewState->getVisibleTimeStart()
                                        << "-" << m_viewState->getVisibleTimeEnd()
                                        << "] $" << snapshot.minPrice << "-$" << snapshot.maxPrice);
                        }
                    }
                }

                if (m_viewState && m_viewState->isAutoScrollEnabled() && m_heatmapStream && m_autoScrollController) {
                    if (!m_autoScrollController->smoothEnabled()) {
                        const bool applied = m_autoScrollController->applySliceAutoScroll(*m_viewState,
                                                                                          *m_heatmapStream,
                                                                                          sliceStartMs,
                                                                                          static_cast<int>(timeframeMs));
                        if (applied && m_panSyncPending) {
                            m_viewState->clearPanVisualOffset();
                            m_panSyncPending = false;
                        }
                    }
                }
                update();
            },
            Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::heatmapRangeReset,
            this,
            [this](double minPrice, double maxPrice, double tickSize, int gridWidth, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapTextureDirty = true;
                    m_heatmapClock.start();
                }
                if (gridWidth > 0) {
                    m_heatmapGridWidth = gridWidth;
                }
                if (gridHeight > 0) {
                    m_heatmapGridHeight = gridHeight;
                }
                m_heatmapTextureDirty = true;
                if (tickSize > 0.0 && tickSize != m_heatmapTickSize) {
                    m_heatmapTickSize = tickSize;
                    emit heatmapTickSizeChanged();
                }
                if (m_heatmapStream) {
                    m_heatmapStream->reset(m_heatmapGridWidth, m_heatmapGridHeight,
                                           minPrice, maxPrice, tickSize);
                }
                if (m_autoScrollController) {
                    m_autoScrollController->resetSpan();
                }
                if (m_viewState && m_viewState->isAutoScrollEnabled()) {
                    m_heatmapViewportInitialized = false;
                }
                update();
            },
            Qt::QueuedConnection);
    m_dataProcessorThread->start();
    if (width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
    }
    QMetaObject::invokeMethod(m_dataProcessor.get(), [this]() {
        m_dataProcessor->setGridViewState(m_viewState.get());
    }, Qt::QueuedConnection);
    
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::viewportChanged);
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::onViewportChanged);
    connect(m_viewState.get(), &GridViewState::panVisualOffsetChanged, this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(m_viewState.get(), &GridViewState::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        &DataProcessor::startProcessing,
        Qt::QueuedConnection);

    if (m_useGpuHeatmap && m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this]() {
            m_dataProcessor->setHeatmapGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
            m_dataProcessor->setHeatmapIntensityScale(m_intensityScale);
        }, Qt::QueuedConnection);

        m_heatmapRenderTimer = new QTimer(this);
        connect(m_heatmapRenderTimer, &QTimer::timeout, this, [this]() {
            const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
            if (!m_useGpuHeatmap || snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0 ||
                snapshot.appendMs <= 0) {
                return;
            }
            const bool dragging = (m_viewState && m_viewState->isDragging());
            const qint64 nowMs = m_heatmapClock.elapsed();
            const qint64 lastAppendMs = m_heatmapStream ? m_heatmapStream->lastAppendMs() : 0;
            const qint64 delta = nowMs - lastAppendMs;
            const bool useFractionalOffset = (m_viewState && m_viewState->isAutoScrollEnabled() &&
                                              m_autoScrollController && !m_autoScrollController->smoothEnabled());
            const float frac = useFractionalOffset
                ? std::clamp(static_cast<float>(delta) / static_cast<float>(snapshot.appendMs), 0.0f, 1.0f)
                : 0.0f;
            if (m_heatmapStream) {
                m_heatmapStream->updateTimeOffset(frac);
            }
            if (!dragging && m_autoScrollController && m_autoScrollController->smoothEnabled() &&
                m_viewState && m_viewState->isAutoScrollEnabled() && m_heatmapStream) {
                const bool applied = m_autoScrollController->applySmoothAutoScroll(*m_viewState,
                                                                                   *m_heatmapStream,
                                                                                   nowMs);
                if (applied && m_panSyncPending) {
                    m_viewState->clearPanVisualOffset();
                    m_panSyncPending = false;
                }
            }
            update();
        });
        m_heatmapRenderTimer->start(16);
    }
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0) { 
        return oldNode;
    }

    if (m_useGpuHeatmap) {
        const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
        const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
        const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
        if ((snapshot.gridWidth > 0 && snapshot.gridWidth != m_heatmapGridWidth) ||
            (snapshot.gridHeight > 0 && snapshot.gridHeight != m_heatmapGridHeight)) {
            if (snapshot.gridWidth > 0) {
                m_heatmapGridWidth = snapshot.gridWidth;
            }
            if (snapshot.gridHeight > 0) {
                m_heatmapGridHeight = snapshot.gridHeight;
            }
            m_heatmapTextureDirty = true;
            sLog_Render("GPU HEATMAP GRID SIZE: " << m_heatmapGridWidth << "x"
                        << m_heatmapGridHeight << " (server authoritative)");
        }
        auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
        if (!texNode) {
            texNode = new HeatmapIntensityNode();
            m_heatmapTextureDirty = true;
            m_whiteGlyphNode = nullptr;
            m_blackGlyphNode = nullptr;
        }

        if (m_heatmapTextureDirty) {
            ensureHeatmapImage();
            ensureHeatmapPaletteImage();
            auto* intensityTexture = window()->createTextureFromImage(m_heatmapImage);
            if (!intensityTexture) {
                const QImage::Format fallbackFormat =
                    (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
                QImage fallback = m_heatmapImage.convertToFormat(fallbackFormat);
                intensityTexture = window()->createTextureFromImage(fallback);
            }
            auto* paletteTexture = window()->createTextureFromImage(m_heatmapPaletteImage);
            if (!intensityTexture || !paletteTexture) {
                delete intensityTexture;
                delete paletteTexture;
            } else {
                intensityTexture->setFiltering(QSGTexture::Nearest);
                paletteTexture->setFiltering(QSGTexture::Linear);
                texNode->setTextures(intensityTexture, paletteTexture);
                m_heatmapTextureDirty = false;
            }
        }
        if (m_heatmapPaletteDirty && !m_heatmapImage.isNull()) {
            m_heatmapPaletteImage = QImage();  // Force regeneration
            ensureHeatmapPaletteImage();
            m_heatmapTextureDirty = true;  // Trigger full texture update
        }

        const QRectF bounds = boundingRect();
        QRectF drawRect = bounds;
        QRectF srcRect(0, 0, gridWidth, gridHeight);
        texNode->setRect(drawRect);
        texNode->setGamma(static_cast<float>(m_heatmapGamma));
        texNode->setContrast(static_cast<float>(m_heatmapContrast));
        texNode->setShaderFloor(static_cast<float>(m_heatmapShaderFloor));
        const int64_t lastSlice = snapshot.lastSliceStartMs;
        double dataStart = 0.0;
        bool dataStartValid = false;
        if (snapshot.appendMs > 0 && gridWidth > 0) {
            const int64_t bufferSpanMs = static_cast<int64_t>(gridWidth) * snapshot.appendMs;
            const double dataEnd = (lastSlice != std::numeric_limits<int64_t>::min() && bufferSpanMs > 0)
                ? static_cast<double>(lastSlice + snapshot.appendMs)
                : static_cast<double>(snapshot.timeOriginMs + bufferSpanMs);
            dataStart = dataEnd - static_cast<double>(bufferSpanMs);
            dataStartValid = (dataEnd > dataStart);
        }

        if (m_viewState && m_viewState->isTimeWindowValid() &&
            snapshot.appendMs > 0 && snapshot.tickSize > 0.0 && snapshot.timeOriginMs != 0) {
            const qint64 timeStart = m_viewState->getVisibleTimeStart();
            const qint64 timeEnd = m_viewState->getVisibleTimeEnd();
            const double minPrice = m_viewState->getMinPrice();
            const double maxPrice = m_viewState->getMaxPrice();

            double timeStartF = static_cast<double>(timeStart);
            double timeEndF = static_cast<double>(timeEnd);
            double minPriceF = minPrice;
            double maxPriceF = maxPrice;

            const QPointF pan = m_viewState->getPanVisualOffset();
            const double timeRange = static_cast<double>(timeEnd - timeStart);
            const double priceRange = maxPrice - minPrice;
            if (!pan.isNull() && bounds.width() > 0.0 && bounds.height() > 0.0 &&
                timeRange > 0.0 && priceRange > 0.0 && m_viewState->isDragging()) {
                const double timePixelsToUnits = timeRange / bounds.width();
                const double pricePixelsToUnits = priceRange / bounds.height();
                const double timeDelta = -pan.x() * timePixelsToUnits;
                const double priceDelta = pan.y() * pricePixelsToUnits;
                timeStartF += timeDelta;
                timeEndF += timeDelta;
                minPriceF += priceDelta;
                maxPriceF += priceDelta;
            }

            if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL")) {
                drawRect = bounds;
                texNode->setRect(drawRect);
                srcRect = QRectF(0, 0, gridWidth, gridHeight);
                texNode->setSourceRect(srcRect);
                texNode->setTimeOffset(0.0f);
            } else if (timeEndF > timeStartF && maxPriceF > minPriceF) {
                const double maxCoordX = static_cast<double>(gridWidth);
                const double maxCoordY = static_cast<double>(gridHeight);
                const double viewTimeSpan = timeEndF - timeStartF;
                const double viewPriceSpan = maxPriceF - minPriceF;
                if (viewTimeSpan <= 0.0 || viewPriceSpan <= 0.0) {
                    drawRect = QRectF();
                    texNode->setRect(drawRect);
                    srcRect = QRectF();
                    texNode->setSourceRect(srcRect);
                } else {
                    const double dataMin = snapshot.minPrice;
                    const double dataMax = snapshot.maxPrice;

                    const double overlapStart = std::max(timeStartF, dataStart);
                    const double overlapEnd = std::min(timeEndF, dataEnd);
                    const double overlapMin = std::max(minPriceF, dataMin);
                    const double overlapMax = std::min(maxPriceF, dataMax);

                    if (overlapEnd <= overlapStart || overlapMax <= overlapMin) {
                        drawRect = QRectF();
                        texNode->setRect(drawRect);
                        srcRect = QRectF();
                        texNode->setSourceRect(srcRect);
                    } else {
                        const double overlapTimeSpan = overlapEnd - overlapStart;
                        const double overlapPriceSpan = overlapMax - overlapMin;

                        const double timeRatioStart = (overlapStart - timeStartF) / viewTimeSpan;
                        const double timeRatioEnd = (overlapEnd - timeStartF) / viewTimeSpan;
                        const double priceRatioTop = (maxPriceF - overlapMax) / viewPriceSpan;
                        const double priceRatioBottom = (maxPriceF - overlapMin) / viewPriceSpan;

                        drawRect = QRectF(
                            bounds.x() + bounds.width() * timeRatioStart,
                            bounds.y() + bounds.height() * priceRatioTop,
                            bounds.width() * (timeRatioEnd - timeRatioStart),
                            bounds.height() * (priceRatioBottom - priceRatioTop));
                        texNode->setRect(drawRect);

                        const double srcW = std::clamp(overlapTimeSpan / snapshot.appendMs, 1.0, maxCoordX);
                        const double srcH = std::clamp(overlapPriceSpan / snapshot.tickSize, 1.0, maxCoordY);
                        double srcX = (overlapStart - dataStart) / snapshot.appendMs;
                        double srcY = (snapshot.maxPrice - overlapMax) / snapshot.tickSize;
                        srcX = std::clamp(srcX, 0.0, maxCoordX - srcW);
                        srcY = std::clamp(srcY, 0.0, maxCoordY - srcH);
                        srcRect = QRectF(srcX, srcY, srcW, srcH);
                        texNode->setSourceRect(srcRect);
                    }
                }
            } else {
                drawRect = bounds;
                texNode->setRect(drawRect);
                srcRect = QRectF(0, 0, gridWidth, gridHeight);
                texNode->setSourceRect(srcRect);
            }
        } else {
            drawRect = bounds;
            texNode->setRect(drawRect);
            srcRect = QRectF(0, 0, gridWidth, gridHeight);
            texNode->setSourceRect(srcRect);
        }

        const bool forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
        if (!forceFull) {
            texNode->setTimeOffset(snapshot.timeOffset);
        }
        m_lastMapping.drawRect = drawRect;
        m_lastMapping.srcRect = srcRect;
        m_lastMapping.dataStartMs = dataStart;
        m_lastMapping.appendMs = static_cast<double>(snapshot.appendMs);
        m_lastMapping.gridWidth = gridWidth;
        m_lastMapping.timeOffset = forceFull ? 0.0f : snapshot.timeOffset;
        m_lastMapping.valid = (dataStartValid &&
                               snapshot.timeOriginMs != 0 &&
                               snapshot.appendMs > 0 &&
                               gridWidth > 0 &&
                               drawRect.width() > 0.0 &&
                               srcRect.width() > 0.0);
        m_lastMapping.cellW = m_lastMapping.valid ? (drawRect.width() / srcRect.width()) : 0.0;

        if ((m_labelRingGridWidth != gridWidth || m_labelRingGridHeight != gridHeight) &&
            gridWidth > 0 && gridHeight > 0) {
            m_labelRingGridWidth = gridWidth;
            m_labelRingGridHeight = gridHeight;
            m_labelLiquidityRing.assign(static_cast<size_t>(gridWidth) * gridHeight, 0);
            m_labelIntensityRing.assign(static_cast<size_t>(gridWidth) * gridHeight, 0);
            m_labelLiquidityScales.assign(gridWidth, 1.0);
        }

        if (m_heatmapStream) {
            std::vector<HeatmapStreamState::PendingLabelColumn> pendingLabelUploads;
            m_heatmapStream->takePendingLabelUploads(pendingLabelUploads);
            if (!pendingLabelUploads.empty()) {
                applyLabelUploads(pendingLabelUploads, gridWidth, gridHeight);
            }
        }

        const QRectF srcRect = texNode->getSourceRect();
        const bool labelVisible = (!drawRect.isEmpty() && !bounds.isEmpty() &&
                                   srcRect.width() > 0.0 && srcRect.height() > 0.0 &&
                                   snapshot.liquidityAvailable &&
                                   m_labelRingGridWidth == gridWidth &&
                                   m_labelRingGridHeight == gridHeight);
        const float cellW = (srcRect.width() > 0.0f)
            ? static_cast<float>(drawRect.width()) / static_cast<float>(srcRect.width())
            : 0.0f;
        const float cellH = (srcRect.height() > 0.0f)
            ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRect.height())
            : 0.0f;
        int labelPx = 14;
        const int envLabelPx = qEnvironmentVariableIntValue("SENTINEL_HEATMAP_LABEL_PX");
        if (envLabelPx > 0) {
            labelPx = envLabelPx;
        }
        const float labelThreshold = static_cast<float>(labelPx);

        if (labelVisible && cellH >= labelThreshold && m_msdfAtlasBuilt && window()) {
            const float fontPx = static_cast<float>(m_msdfAtlas.fontPx());
            const float scale = (fontPx > 0.0f) ? std::clamp(cellH / fontPx, 0.25f, 2.5f) : 1.0f;
            const float timeOffset = forceFull ? 0.0f : snapshot.timeOffset;
            const float baseX = static_cast<float>(srcRect.x()) + (timeOffset * gridWidth);
            const int startX = static_cast<int>(std::floor(baseX));
            const float fracX = baseX - static_cast<float>(startX);
            const float baseY = static_cast<float>(srcRect.y());
            const int startY = static_cast<int>(std::floor(baseY));
            const float fracY = baseY - static_cast<float>(startY);

            if (m_labelWhiteQuads.capacity() < 32000) {
                m_labelWhiteQuads.reserve(32000);
            }
            if (m_labelBlackQuads.capacity() < 32000) {
                m_labelBlackQuads.reserve(32000);
            }

            const bool dollars = (m_liquidityLabelMode != 0);
            HeatmapLabelRenderer::buildLabelQuads(snapshot,
                                                  m_msdfAtlas,
                                                  m_labelLiquidityRing,
                                                  m_labelIntensityRing,
                                                  m_labelLiquidityScales,
                                                  srcRect,
                                                  drawRect,
                                                  startX,
                                                  startY,
                                                  fracX,
                                                  fracY,
                                                  cellW,
                                                  cellH,
                                                  scale,
                                                  dollars,
                                                  m_labelWhiteQuads,
                                                  m_labelBlackQuads);

            if (!m_whiteGlyphNode) {
                m_whiteGlyphNode = new MsdfGlyphNode();
                m_whiteGlyphNode->setColor(Qt::white);
                m_whiteGlyphNode->ensureCapacity(32000);
                texNode->appendChildNode(m_whiteGlyphNode);
            }
            if (!m_blackGlyphNode) {
                m_blackGlyphNode = new MsdfGlyphNode();
                m_blackGlyphNode->setColor(Qt::black);
                m_blackGlyphNode->ensureCapacity(32000);
                texNode->appendChildNode(m_blackGlyphNode);
            }

            m_whiteGlyphNode->setAtlas(m_msdfAtlas.image(), window());
            m_whiteGlyphNode->setPxRange(m_msdfAtlas.pxRange());
            m_blackGlyphNode->setAtlas(m_msdfAtlas.image(), window());
            m_blackGlyphNode->setPxRange(m_msdfAtlas.pxRange());
            m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
            m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
        } else {
            if (m_whiteGlyphNode) {
                m_labelWhiteQuads.clear();
                m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
            }
            if (m_blackGlyphNode) {
                m_labelBlackQuads.clear();
                m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
            }
        }

        std::vector<HeatmapStreamState::PendingColumn> pendingUploads;
        if (m_heatmapStream) {
            m_heatmapStream->takePendingUploads(pendingUploads);
        }
        for (auto& upload : pendingUploads) {
            texNode->enqueueColumn(upload.x, std::move(upload.data));
        }

        if (!m_fpsTimer.isValid()) {
            m_fpsTimer.start();
            m_fpsFrameCount = 0;
        }

        ++m_fpsFrameCount;
        const qint64 elapsedMs = m_fpsTimer.elapsed();
        if (elapsedMs >= 1000) {
            m_currentFps.store((static_cast<double>(m_fpsFrameCount) * 1000.0) / elapsedMs);
            m_fpsFrameCount = 0;
            m_fpsTimer.restart();
        }

        return texNode;
    }

    return oldNode;
}

void UnifiedGridRenderer::ensureHeatmapImage() {
    if (!m_heatmapImage.isNull() && m_heatmapImage.width() == m_heatmapGridWidth &&
        m_heatmapImage.height() == m_heatmapGridHeight) {
        const QImage::Format expectedFormat =
            (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
        if (m_heatmapImage.format() == expectedFormat) {
            return;
        }
    }

    const QImage::Format format =
        (m_intensityBytesPerCell == 2) ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8;
    m_heatmapImage = QImage(m_heatmapGridWidth, m_heatmapGridHeight, format);
    if (m_heatmapImage.isNull()) {
        return;
    }
    m_heatmapImage.fill(m_heatmapBackgroundColor);
}

void UnifiedGridRenderer::ensureHeatmapPaletteImage() {
    if (!m_heatmapPaletteImage.isNull() && !m_heatmapPaletteDirty) {
        return;
    }

    const int width = 512;
    const int height = 1;
    m_heatmapPaletteImage = QImage(width, height, QImage::Format_ARGB32);
    if (m_heatmapPaletteImage.isNull()) {
        return;
    }

    auto* row = reinterpret_cast<QRgb*>(m_heatmapPaletteImage.scanLine(0));
    const float gamma = static_cast<float>(m_heatmapPaletteGamma);

    for (int i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const bool isAsk = (i >= width / 2);
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float x = std::clamp(localT, 0.0f, 1.0f);
        const float curve = std::pow(x, gamma);
        const QColor color = isAsk ? m_askGradient.interpolate(curve) : m_bidGradient.interpolate(curve);

        row[i] = qRgba(color.red(), color.green(), color.blue(), 255);
    }

    m_heatmapPaletteDirty = false;
    sLog_Render("Heatmap palette regenerated with gamma=" << gamma
                << " bid_stops=" << m_bidGradient.stops.size()
                << " ask_stops=" << m_askGradient.stops.size());
}

void UnifiedGridRenderer::buildMsdfAtlas() {
    if (m_msdfAtlasBuilt) {
        return;
    }
    MsdfAtlas::BuildParams params;
    params.fontFamily = "Roboto Mono";
    params.fontPx = 64;
    params.pxRange = 8.0f;
    params.charset = QStringLiteral("0123456789.kMB+-");
    if (m_msdfAtlas.build(params)) {
        if (qEnvironmentVariableIsSet("SENTINEL_DUMP_GLYPH_ATLAS")) {
            m_msdfAtlas.image().save("/tmp/sentinel_msdf_atlas.png");
        }
        m_msdfAtlasBuilt = true;
    }
}

void UnifiedGridRenderer::applyLabelUploads(
    const std::vector<HeatmapStreamState::PendingLabelColumn>& uploads,
    int gridWidth,
    int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0 || uploads.empty()) {
        return;
    }
    const size_t expectedSize = static_cast<size_t>(gridWidth) * gridHeight;
    if (m_labelLiquidityRing.size() != expectedSize) {
        m_labelLiquidityRing.assign(expectedSize, 0);
    }
    if (m_labelIntensityRing.size() != expectedSize) {
        m_labelIntensityRing.assign(expectedSize, 0);
    }
    if (m_labelLiquidityScales.size() != static_cast<size_t>(gridWidth)) {
        m_labelLiquidityScales.assign(gridWidth, 1.0);
    }

    const int expectedLiquidityBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
    const int expectedIntensityBytes = gridHeight * m_intensityBytesPerCell;
    for (const auto& upload : uploads) {
        const int column = upload.x;
        if (column < 0 || column >= gridWidth) {
            continue;
        }
        if (upload.intensity.size() == expectedIntensityBytes) {
            if (m_intensityBytesPerCell == 1) {
                const auto* src = reinterpret_cast<const uint8_t*>(upload.intensity.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    m_labelIntensityRing[static_cast<size_t>(y) * gridWidth + column] =
                        static_cast<uint16_t>(src[y]) * 257;
                }
            } else if (m_intensityBytesPerCell == 2) {
                const auto* src = reinterpret_cast<const uint16_t*>(upload.intensity.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    const uint16_t raw = qFromLittleEndian(src[y]);
                    m_labelIntensityRing[static_cast<size_t>(y) * gridWidth + column] = raw;
                }
            }
        }
        if (upload.liquidity.size() == expectedLiquidityBytes) {
            const auto* src = reinterpret_cast<const uint16_t*>(upload.liquidity.constData());
            for (int y = 0; y < gridHeight; ++y) {
                const uint16_t raw = qFromLittleEndian(src[y]);
                m_labelLiquidityRing[static_cast<size_t>(y) * gridWidth + column] = raw;
            }
            m_labelLiquidityScales[column] = upload.liquidityScale;
        }
    }
}

void UnifiedGridRenderer::fitHeatmapToDataRange() {
    const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    if (!m_viewState || snapshot.appendMs <= 0 || snapshot.gridWidth <= 0) {
        return;
    }
    if (snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return;
    }
    const int64_t bufferSpanMs = std::max<int64_t>(
        1, static_cast<int64_t>(snapshot.gridWidth) * snapshot.appendMs);
    const int64_t dataEnd = snapshot.lastSliceStartMs + snapshot.appendMs;
    const int64_t dataStart = dataEnd - bufferSpanMs;
    if (dataEnd <= dataStart) {
        return;
    }
    if (m_viewState->isAutoScrollEnabled()) {
        m_viewState->enableAutoScroll(false);
    }
    m_viewState->setViewport(dataStart, dataEnd, snapshot.minPrice, snapshot.maxPrice);
    update();
}

QString UnifiedGridRenderer::getTextureSize() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth > 0 && snapshot.gridHeight > 0) {
            return QString("%1x%2").arg(snapshot.gridWidth).arg(snapshot.gridHeight);
        }
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0) {
            return "N/A";
        }
        const int bytesPerPixel = (m_intensityBytesPerCell > 0) ? m_intensityBytesPerCell : 1;
        qint64 bytes = static_cast<qint64>(snapshot.gridWidth) * snapshot.gridHeight * bytesPerPixel;
        double mb = bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
    if (m_useGpuHeatmap) {
        return (m_intensityBytesPerCell == 2) ? "Grayscale16" : "Grayscale8";
    }
    return "N/A";
}

QString UnifiedGridRenderer::getLabelRingMemory() const {
    if (m_labelRingGridWidth <= 0 || m_labelRingGridHeight <= 0) {
        return "Label ring: N/A";
    }
    const qint64 cells = static_cast<qint64>(m_labelRingGridWidth) * m_labelRingGridHeight;
    const qint64 bytesIntensity = cells * static_cast<qint64>(sizeof(uint16_t));
    const qint64 bytesLiquidity = cells * static_cast<qint64>(sizeof(uint16_t));
    const qint64 bytesScales = static_cast<qint64>(m_labelRingGridWidth) * static_cast<qint64>(sizeof(double));
    const qint64 totalBytes = bytesIntensity + bytesLiquidity + bytesScales;
    const double mb = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    return QString("Label ring: %1 MB").arg(mb, 0, 'f', 2);
}

QString UnifiedGridRenderer::getMsdfAtlasMemory() const {
    if (!m_msdfAtlas.isBuilt()) {
        return "MSDF atlas: N/A";
    }
    const QImage& image = m_msdfAtlas.image();
    if (image.isNull()) {
        return "MSDF atlas: N/A";
    }
    const double mb = static_cast<double>(image.sizeInBytes()) / (1024.0 * 1024.0);
    return QString("MSDF atlas: %1 MB").arg(mb, 0, 'f', 2);
}

double UnifiedGridRenderer::getUploadBandwidth() const {
    return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth > 0) {
            return QString("%1/%2").arg(m_heatmapStream->writeColumn()).arg(snapshot.gridWidth);
        }
    }
    return "N/A";
}

int UnifiedGridRenderer::getDirtyRegionCount() const {
    if (m_useGpuHeatmap) {
        if (m_heatmapStream) {
            return m_heatmapStream->pendingUploadCount();
        }
    }
    return 0;
}

void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { 
    if (m_viewState && isVisible() && event->button() == Qt::LeftButton) { 
        m_viewState->handlePanStart(event->position()); 
        event->accept(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) { 
    if (m_viewState) { 
        m_viewState->handlePanMove(event->position()); 
        event->accept(); 
        update(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) {
    if (m_viewState) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const bool autoScroll = m_viewState->isAutoScrollEnabled();
        bool panAppliedToAuto = false;
        if (autoScroll && !pan.isNull() && width() > 0.0 && height() > 0.0 &&
            m_viewState->isTimeWindowValid() && m_autoScrollController) {
            panAppliedToAuto = m_autoScrollController->applyPanToAutoScroll(*m_viewState, width(), height());
            m_viewState->handlePanEnd(false);
            m_panSyncPending = true;
            update();
        } else {
            m_viewState->handlePanEnd(true);
        }
        event->accept();
        if (m_viewState->isAutoScrollEnabled() && m_heatmapStream && m_autoScrollController && !panAppliedToAuto) {
            m_autoScrollController->updateLagFromView(*m_viewState, *m_heatmapStream);
        }
        if (autoScroll) {
            m_panSyncPending = true;
        }
        update();
    }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { 
    if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) { 
        m_viewState->handleZoomWithSensitivity(event->angleDelta().y(), event->position(), QSizeF(width(), height())); 
        update(); event->accept(); 
    } else event->ignore(); 
}

int UnifiedGridRenderer::getCurrentTimeResolution() const { return static_cast<int>(m_currentTimeframe_ms); }
double UnifiedGridRenderer::getCurrentPriceResolution() const { return m_dataProcessor ? m_dataProcessor->getPriceResolution() : 1.0; }
double UnifiedGridRenderer::getScreenWidth() const { return width(); }
double UnifiedGridRenderer::getScreenHeight() const { return height(); }
double UnifiedGridRenderer::getZoomFactor() const { return m_viewState ? m_viewState->getZoomFactor() : 1.0; }
qint64 UnifiedGridRenderer::getVisibleTimeStart() const { return m_viewState ? m_viewState->getVisibleTimeStart() : 0; }
qint64 UnifiedGridRenderer::getVisibleTimeEnd() const { return m_viewState ? m_viewState->getVisibleTimeEnd() : 0; }
double UnifiedGridRenderer::getMinPrice() const { return m_viewState ? m_viewState->getMinPrice() : 0.0; }
double UnifiedGridRenderer::getMaxPrice() const { return m_viewState ? m_viewState->getMaxPrice() : 0.0; }
QPointF UnifiedGridRenderer::getPanVisualOffset() const { return m_viewState ? m_viewState->getPanVisualOffset() : QPointF(0, 0); }

bool UnifiedGridRenderer::heatmapDataPriceRange(double& outMin, double& outMax) const {
    if (!m_heatmapStream) {
        return false;
    }
    const auto snapshot = m_heatmapStream->snapshot();
    if (snapshot.tickSize <= 0.0 || snapshot.maxPrice <= snapshot.minPrice) {
        return false;
    }
    outMin = snapshot.minPrice;
    outMax = snapshot.maxPrice;
    return true;
}

bool UnifiedGridRenderer::heatmapDataTimeRange(qint64& outStart, qint64& outEnd) const {
    if (!m_heatmapStream) {
        return false;
    }
    const auto snapshot = m_heatmapStream->snapshot();
    if (snapshot.appendMs <= 0 || snapshot.gridWidth <= 0) {
        return false;
    }
    const int64_t bufferSpanMs = static_cast<int64_t>(snapshot.gridWidth) * snapshot.appendMs;
    if (bufferSpanMs <= 0) {
        return false;
    }
    int64_t dataEnd = 0;
    if (snapshot.lastSliceStartMs != std::numeric_limits<int64_t>::min()) {
        dataEnd = snapshot.lastSliceStartMs + snapshot.appendMs;
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

QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Size:%1x%2").arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
QString UnifiedGridRenderer::getViewportMathDebug() const {
    if (!m_viewState) {
        return "Viewport: N/A";
    }
    const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    const QRectF bounds = boundingRect();
    const double widthF = bounds.width();
    const double heightF = bounds.height();

    qint64 timeStart = m_viewState->getVisibleTimeStart();
    qint64 timeEnd = m_viewState->getVisibleTimeEnd();
    double minPrice = m_viewState->getMinPrice();
    double maxPrice = m_viewState->getMaxPrice();

    double timeStartF = static_cast<double>(timeStart);
    double timeEndF = static_cast<double>(timeEnd);
    double minPriceF = minPrice;
    double maxPriceF = maxPrice;

    const QPointF pan = m_viewState->getPanVisualOffset();
    const double timeRange = static_cast<double>(timeEnd - timeStart);
    const double priceRange = maxPrice - minPrice;
    if (!pan.isNull() && widthF > 0.0 && heightF > 0.0 &&
        timeRange > 0.0 && priceRange > 0.0 && m_viewState->isDragging()) {
        const double timePixelsToUnits = timeRange / widthF;
        const double pricePixelsToUnits = priceRange / heightF;
        const double timeDelta = -pan.x() * timePixelsToUnits;
        const double priceDelta = pan.y() * pricePixelsToUnits;
        timeStartF += timeDelta;
        timeEndF += timeDelta;
        minPriceF += priceDelta;
        maxPriceF += priceDelta;
    }

    const bool forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
    const double viewTimeSpan = timeEndF - timeStartF;
    const double viewPriceSpan = maxPriceF - minPriceF;

    QStringList lines;
    lines << "Viewport Math"
          << QString("view.time: %1 → %2 (%3 ms)")
                 .arg(static_cast<qint64>(timeStartF))
                 .arg(static_cast<qint64>(timeEndF))
                 .arg(static_cast<qint64>(std::max(0.0, viewTimeSpan)))
          << QString("view.price: %1 → %2 (Δ%3)")
                 .arg(minPriceF, 0, 'f', 4)
                 .arg(maxPriceF, 0, 'f', 4)
                 .arg(std::max(0.0, viewPriceSpan), 0, 'f', 4)
          << QString("tick: %1  grid: %2x%3  append: %4")
                 .arg(snapshot.tickSize, 0, 'f', 6)
                 .arg(gridWidth)
                 .arg(gridHeight)
                 .arg(snapshot.appendMs);

    if (snapshot.appendMs > 0 && snapshot.tickSize > 0.0 &&
        snapshot.timeOriginMs != 0 && viewTimeSpan > 0.0 && viewPriceSpan > 0.0) {
        const int64_t bufferSpanMs = static_cast<int64_t>(gridWidth) * snapshot.appendMs;
        const int64_t lastSlice = snapshot.lastSliceStartMs;
        double dataEnd = (lastSlice != std::numeric_limits<int64_t>::min() && bufferSpanMs > 0)
            ? static_cast<double>(lastSlice + snapshot.appendMs)
            : static_cast<double>(snapshot.timeOriginMs + bufferSpanMs);
        double dataStart = dataEnd - static_cast<double>(bufferSpanMs);
        const double dataMin = snapshot.minPrice;
        const double dataMax = snapshot.maxPrice;

        const double overlapStart = std::max(timeStartF, dataStart);
        const double overlapEnd = std::min(timeEndF, dataEnd);
        const double overlapMin = std::max(minPriceF, dataMin);
        const double overlapMax = std::min(maxPriceF, dataMax);

        lines << QString("data.time: %1 → %2").arg(static_cast<qint64>(dataStart))
                                               .arg(static_cast<qint64>(dataEnd))
              << QString("data.price: %1 → %2").arg(dataMin, 0, 'f', 4)
                                                .arg(dataMax, 0, 'f', 4)
              << QString("overlap.time: %1 → %2")
                     .arg(static_cast<qint64>(overlapStart))
                     .arg(static_cast<qint64>(overlapEnd))
              << QString("overlap.price: %1 → %2")
                     .arg(overlapMin, 0, 'f', 4)
                     .arg(overlapMax, 0, 'f', 4);

        QRectF drawRect = bounds;
        QRectF srcRect(0, 0, gridWidth, gridHeight);
        if (!forceFull && overlapEnd > overlapStart && overlapMax > overlapMin) {
            const double overlapTimeSpan = overlapEnd - overlapStart;
            const double overlapPriceSpan = overlapMax - overlapMin;
            const double timeRatioStart = (overlapStart - timeStartF) / viewTimeSpan;
            const double timeRatioEnd = (overlapEnd - timeStartF) / viewTimeSpan;
            const double priceRatioTop = (maxPriceF - overlapMax) / viewPriceSpan;
            const double priceRatioBottom = (maxPriceF - overlapMin) / viewPriceSpan;

            drawRect = QRectF(
                bounds.x() + widthF * timeRatioStart,
                bounds.y() + heightF * priceRatioTop,
                widthF * (timeRatioEnd - timeRatioStart),
                heightF * (priceRatioBottom - priceRatioTop));

            const double maxCoordX = static_cast<double>(gridWidth);
            const double maxCoordY = static_cast<double>(gridHeight);
            const double srcW = std::clamp(overlapTimeSpan / snapshot.appendMs, 1.0, maxCoordX);
            const double srcH = std::clamp(overlapPriceSpan / snapshot.tickSize, 1.0, maxCoordY);
            double srcX = (overlapStart - dataStart) / snapshot.appendMs;
            double srcY = (snapshot.maxPrice - overlapMax) / snapshot.tickSize;
            srcX = std::clamp(srcX, 0.0, maxCoordX - srcW);
            srcY = std::clamp(srcY, 0.0, maxCoordY - srcH);
            srcRect = QRectF(srcX, srcY, srcW, srcH);
        }

        const double cellW = (srcRect.width() > 0.0) ? (drawRect.width() / srcRect.width()) : 0.0;
        const double cellH = (srcRect.height() > 0.0) ? (drawRect.height() / srcRect.height()) : 0.0;

        lines << QString("drawRect: x%1 y%2 w%3 h%4")
                     .arg(drawRect.x(), 0, 'f', 1)
                     .arg(drawRect.y(), 0, 'f', 1)
                     .arg(drawRect.width(), 0, 'f', 1)
                     .arg(drawRect.height(), 0, 'f', 1)
              << QString("srcRect: x%1 y%2 w%3 h%4")
                     .arg(srcRect.x(), 0, 'f', 2)
                     .arg(srcRect.y(), 0, 'f', 2)
                     .arg(srcRect.width(), 0, 'f', 2)
                     .arg(srcRect.height(), 0, 'f', 2)
              << QString("cell: %1 x %2 px").arg(cellW, 0, 'f', 2).arg(cellH, 0, 'f', 2)
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

    if (!m_heatmapStream) {
        lines << "stream: N/A";
        return lines.join('\n');
    }

    const auto snapshot = m_heatmapStream->snapshot();
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
    const int pendingUploads = m_heatmapStream->pendingUploadCount();
    const int writeColumn = m_heatmapStream->writeColumn();
    const qint64 lastAppendMs = m_heatmapStream->lastAppendMs();
    const qint64 nowMs = m_heatmapClock.isValid() ? m_heatmapClock.elapsed() : 0;
    const qint64 ageMs = (lastAppendMs > 0 && nowMs >= lastAppendMs) ? (nowMs - lastAppendMs) : -1;

    lines << QString("grid: %1x%2  append: %3 ms")
                 .arg(gridWidth)
                 .arg(gridHeight)
                 .arg(snapshot.appendMs)
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
QString UnifiedGridRenderer::getPerformanceStats() const { return "N/A (SentinelMonitor removed)"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_currentFps.load(); }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
void UnifiedGridRenderer::togglePerformanceOverlay() { }

void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); update(); } }
