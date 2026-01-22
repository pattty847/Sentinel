/*
Sentinel — UnifiedGridRenderer
Role: Implements the data batching and rendering orchestration logic.
Inputs/Outputs: Buffers incoming data; passes it to rendering strategies in updatePaintNode.
Threading: Data receiving slots run on main thread; updatePaintNode runs on the render thread.
Performance: Batches high-frequency data events into single, throttled screen updates via a QTimer.
Integration: Defines the specific set of render strategies that compose the final chart.
Observability: Traces the data flow from reception to rendering via qDebug.
Related: UnifiedGridRenderer.h, HeatmapStrategy.h, TradeStrategy.h, CoordinateSystem.h.
Assumptions: The render strategies are compatible and can be layered together.
*/
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
#include <QFont>
#include <QFontDatabase>
#include <algorithm>
#include <cmath>

// New modular architecture includes
#include "render/GridViewState.hpp"
#include "render/DataProcessor.hpp"
#include "render/GlyphAtlas.hpp"
#include "render/HeatmapGlyphNode.hpp"
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
    // TODO: Wire trades into candle/overlay pipeline.
}

// onLiveOrderBookUpdated(QString) removed: legacy pass-through; MainWindow connects Core→DataProcessor directly

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
    // Mark transform dirty for rendering update
    update();
}


void UnifiedGridRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    
    if (newGeometry.size() != oldGeometry.size()) {
        sLog_Render("UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        // Keep GridViewState in sync with the item size for accurate coord math
        if (m_viewState) {
            m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
        }

        // Size change only affects transform, not geometry topology
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

    // Delegate to DataProcessor
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
    
    // NO pan offset applied - that's handled by the node transform in updatePaintNode
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
    
    // NO pan offset removed - node transform handles it
    return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
}

void UnifiedGridRenderer::init() {
    m_useGpuHeatmap = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP");
    if (m_useGpuHeatmap) {
        bool ok = false;
        const int envSize = qgetenv("SENTINEL_HEATMAP_GRID").toInt(&ok);
        if (ok && envSize > 0) {
            m_heatmapGridSize = envSize;
        }
        m_heatmapClock.start();
    }

    // Register metatypes for cross-thread signal/slot connections
    qRegisterMetaType<Trade>("Trade");
    
    m_viewState = std::make_unique<GridViewState>(this);
    m_heatmapStream = std::make_unique<HeatmapStreamState>();
    m_heatmapStream->setGridSize(m_heatmapGridSize);
    m_heatmapStream->setAppendMs(100);
    m_autoScrollController = std::make_unique<ViewportAutoScrollController>();
    m_autoScrollController->setPaddingFrac(m_autoScrollPaddingFrac);
    m_autoScrollController->setSmoothEnabled(m_smoothAutoScrollEnabled);
    buildGlyphAtlases();
    
    // Create DataProcessor on worker thread for background processing
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessor = std::make_unique<DataProcessor>();  // No parent - will be moved to thread
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    if (m_useGpuHeatmap) {
        bool ok = false;
        const double recenter = qgetenv("SENTINEL_HEATMAP_RECENTER").toDouble(&ok);
        if (ok && recenter > 0.0) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, recenter]() {
                m_dataProcessor->setHeatmapRecenterFraction(recenter);
            }, Qt::QueuedConnection);
        }
    }
    
    // Connect signals with QueuedConnection for thread safety
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
                   double liquidityScale) {
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
                                    << " grid=" << m_heatmapGridSize
                                    << " bytes=" << column.size());
                    }
                    return;
                }
                if (column.size() > 0 && column.size() != m_heatmapGridSize) {
                    m_heatmapGridSize = column.size();
                    m_heatmapTextureDirty = true;
                    if (m_heatmapStream) {
                        m_heatmapStream->reset(m_heatmapGridSize, minPrice, maxPrice, tickSize);
                    }
                    m_heatmapViewportInitialized = false;
                }

                if (debug) {
                    static int debugCount = 0;
                    ++debugCount;
                    if (debugCount <= 5 || debugCount % 50 == 0) {
                        int bidCount = 0;
                        int askCount = 0;
                        unsigned char minByte = 255;
                        unsigned char maxByte = 0;
                        const auto* bytes = reinterpret_cast<const unsigned char*>(column.constData());
                        for (int i = 0; i < column.size(); ++i) {
                            const unsigned char v = bytes[i];
                            if (v == 0) {
                                continue;
                            }
                            minByte = std::min(minByte, v);
                            maxByte = std::max(maxByte, v);
                            if (v >= 128) {
                                ++askCount;
                            } else {
                                ++bidCount;
                            }
                        }
                        sLog_Render("GPU HEATMAP BYTES: bids=" << bidCount
                                    << " asks=" << askCount
                                    << " min=" << static_cast<int>(minByte)
                                    << " max=" << static_cast<int>(maxByte));
                    }
                }

                if (m_heatmapStream) {
                    m_heatmapStream->updateRange(minPrice, maxPrice, tickSize);
                    if (timeframeMs > 0) {
                        m_heatmapStream->setAppendMs(static_cast<int>(timeframeMs));
                    }
                }

                const int expectedLiquidityBytes = m_heatmapGridSize * static_cast<int>(sizeof(uint16_t));
                const bool haveLiquidityColumn = (liquidityColumn.size() == expectedLiquidityBytes);
                QByteArray intensityColumn = column;
                if (m_heatmapLiquidityThreshold > 0.0 && haveLiquidityColumn && liquidityScale > 0.0 &&
                    intensityColumn.size() == m_heatmapGridSize) {
                    const auto* raw = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
                    const double threshold = m_heatmapLiquidityThreshold;
                    for (int y = 0; y < m_heatmapGridSize; ++y) {
                        const uint16_t packed = qFromLittleEndian(raw[y]);
                        if (packed == 0) {
                            intensityColumn[y] = 0;
                            continue;
                        }
                        double value = static_cast<double>(packed) * liquidityScale;
                        if (m_liquidityLabelMode != 0) {
                            const double price = maxPrice - (static_cast<double>(y) * tickSize);
                            value *= price;
                        }
                        if (value < threshold) {
                            intensityColumn[y] = 0;
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
            [this](double minPrice, double maxPrice, double tickSize, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapTextureDirty = true;
                    m_heatmapClock.start();
                }
                if (gridHeight > 0) {
                    m_heatmapGridSize = gridHeight;
                }
                m_heatmapTextureDirty = true;
                if (m_heatmapStream) {
                    m_heatmapStream->reset(m_heatmapGridSize, minPrice, maxPrice, tickSize);
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
    
    // Start the worker thread
    m_dataProcessorThread->start();
    
    // Initialize viewport size immediately to avoid 0x0 transforms
    if (width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
    }

    // Set ViewState immediately as it's available
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
            m_dataProcessor->setHeatmapGridHeight(m_heatmapGridSize);
            m_dataProcessor->setHeatmapIntensityScale(m_intensityScale);
        }, Qt::QueuedConnection);

        m_heatmapRenderTimer = new QTimer(this);
        connect(m_heatmapRenderTimer, &QTimer::timeout, this, [this]() {
            const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
            if (!m_useGpuHeatmap || snapshot.gridSize <= 0 || snapshot.appendMs <= 0) {
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
        const int gridSize = (snapshot.gridSize > 0) ? snapshot.gridSize : m_heatmapGridSize;
        if (snapshot.gridSize > 0 && snapshot.gridSize != m_heatmapGridSize) {
            m_heatmapGridSize = snapshot.gridSize;
            m_heatmapTextureDirty = true;
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
                QImage fallback = m_heatmapImage.convertToFormat(QImage::Format_Grayscale8);
                intensityTexture = window()->createTextureFromImage(fallback);
            }
            auto* paletteTexture = window()->createTextureFromImage(m_heatmapPaletteImage);
            if (intensityTexture && paletteTexture) {
                intensityTexture->setFiltering(QSGTexture::Nearest);
                paletteTexture->setFiltering(QSGTexture::Linear);
                texNode->setTextures(intensityTexture, paletteTexture);
                texNode->setGamma(1.05f);
                texNode->setContrast(1.15f);
                m_heatmapTextureDirty = false;
            }
        }

        const QRectF bounds = boundingRect();
        QRectF drawRect = bounds;
        texNode->setRect(drawRect);

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
                texNode->setSourceRect(QRectF(0, 0, gridSize, gridSize));
                texNode->setTimeOffset(0.0f);
            } else if (timeEndF > timeStartF && maxPriceF > minPriceF) {
                const double maxCoord = static_cast<double>(gridSize);
                const double viewTimeSpan = timeEndF - timeStartF;
                const double viewPriceSpan = maxPriceF - minPriceF;
                if (viewTimeSpan <= 0.0 || viewPriceSpan <= 0.0) {
                    drawRect = QRectF();
                    texNode->setRect(drawRect);
                    texNode->setSourceRect(QRectF());
                } else {
                    const int64_t lastSlice = snapshot.lastSliceStartMs;
                    const int64_t bufferSpanMs = static_cast<int64_t>(snapshot.gridSize) * snapshot.appendMs;
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

                    if (overlapEnd <= overlapStart || overlapMax <= overlapMin) {
                        drawRect = QRectF();
                        texNode->setRect(drawRect);
                        texNode->setSourceRect(QRectF());
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

                        const double srcW = std::clamp(overlapTimeSpan / snapshot.appendMs, 1.0, maxCoord);
                        const double srcH = std::clamp(overlapPriceSpan / snapshot.tickSize, 1.0, maxCoord);
                        double srcX = (overlapStart - dataStart) / snapshot.appendMs;
                        double srcY = (snapshot.maxPrice - overlapMax) / snapshot.tickSize;
                        srcX = std::clamp(srcX, 0.0, maxCoord - srcW);
                        srcY = std::clamp(srcY, 0.0, maxCoord - srcH);
                        texNode->setSourceRect(QRectF(srcX, srcY, srcW, srcH));
                    }
                }
            } else {
                drawRect = bounds;
                texNode->setRect(drawRect);
                texNode->setSourceRect(QRectF(0, 0, gridSize, gridSize));
            }
        } else {
            drawRect = bounds;
            texNode->setRect(drawRect);
            texNode->setSourceRect(QRectF(0, 0, gridSize, gridSize));
        }

        const bool forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
        if (!forceFull) {
            texNode->setTimeOffset(snapshot.timeOffset);
        }

        if (m_labelRingGridSize != gridSize && gridSize > 0) {
            m_labelRingGridSize = gridSize;
            m_labelLiquidityRing.assign(static_cast<size_t>(gridSize) * gridSize, 0);
            m_labelIntensityRing.assign(static_cast<size_t>(gridSize) * gridSize, 0);
            m_labelLiquidityScales.assign(gridSize, 1.0);
        }

        if (m_heatmapStream) {
            std::vector<HeatmapStreamState::PendingLabelColumn> pendingLabelUploads;
            m_heatmapStream->takePendingLabelUploads(pendingLabelUploads);
            if (!pendingLabelUploads.empty()) {
                applyLabelUploads(pendingLabelUploads, gridSize);
            }
        }

        const QRectF srcRect = texNode->getSourceRect();
        const bool labelVisible = (!drawRect.isEmpty() && !bounds.isEmpty() &&
                                   srcRect.width() > 0.0 && srcRect.height() > 0.0 &&
                                   snapshot.liquidityAvailable && m_labelRingGridSize == gridSize);
        const float cellW = (srcRect.width() > 0.0f)
            ? static_cast<float>(drawRect.width()) / static_cast<float>(srcRect.width())
            : 0.0f;
        const float cellH = (srcRect.height() > 0.0f)
            ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRect.height())
            : 0.0f;
        const float labelThreshold = 12.0f;

        if (labelVisible && cellH >= labelThreshold && m_glyphAtlasesBuilt && window()) {
            const int bucket = pickFontBucket(cellH);
            const float fontPx = fontBucketPx(bucket);
            const float scale = (fontPx > 0.0f) ? (cellH / fontPx) : 1.0f;
            const GlyphAtlas& atlas = m_glyphAtlases[static_cast<size_t>(bucket)];
            if (!atlas.isBuilt()) {
                if (m_whiteGlyphNode) {
                    m_labelWhiteQuads.clear();
                    m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
                }
                if (m_blackGlyphNode) {
                    m_labelBlackQuads.clear();
                    m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
                }
            } else {
                const float timeOffset = forceFull ? 0.0f : snapshot.timeOffset;
                const float baseX = static_cast<float>(srcRect.x()) + (timeOffset * gridSize);
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
                                                      atlas,
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
                    m_whiteGlyphNode = new HeatmapGlyphNode();
                    m_whiteGlyphNode->setColor(Qt::white);
                    m_whiteGlyphNode->ensureCapacity(32000);
                    texNode->appendChildNode(m_whiteGlyphNode);
                }
                if (!m_blackGlyphNode) {
                    m_blackGlyphNode = new HeatmapGlyphNode();
                    m_blackGlyphNode->setColor(Qt::black);
                    m_blackGlyphNode->ensureCapacity(32000);
                    texNode->appendChildNode(m_blackGlyphNode);
                }

                m_whiteGlyphNode->setAtlas(atlas.image(), window());
                m_blackGlyphNode->setAtlas(atlas.image(), window());
                m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
                m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
            }
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
    if (!m_heatmapImage.isNull() && m_heatmapImage.width() == m_heatmapGridSize &&
        m_heatmapImage.height() == m_heatmapGridSize) {
        return;
    }

    m_heatmapImage = QImage(m_heatmapGridSize, m_heatmapGridSize, QImage::Format_Grayscale8);
    if (m_heatmapImage.isNull()) {
        return;
    }
    m_heatmapImage.fill(Qt::black);
}

void UnifiedGridRenderer::ensureHeatmapPaletteImage() {
    if (!m_heatmapPaletteImage.isNull()) {
        return;
    }

    const int width = 512;
    const int height = 1;
    // Use ARGB32 format which matches QRgb (0xAARRGGBB) layout
    m_heatmapPaletteImage = QImage(width, height, QImage::Format_ARGB32);
    if (m_heatmapPaletteImage.isNull()) {
        return;
    }

    auto* row = reinterpret_cast<QRgb*>(m_heatmapPaletteImage.scanLine(0));
    const float gamma = 0.65f;
    for (int i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const bool isAsk = (i >= width / 2);
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float x = std::clamp(localT, 0.0f, 1.0f);
        const float curve = std::pow(x, gamma);

        // Base hues: bids = teal/green, asks = red/orange.
        const int r0 = isAsk ? 255 : 18;
        const int g0 = isAsk ? 64 : 255;
        const int b0 = isAsk ? 32 : 160;

        const int r1 = isAsk ? 255 : 180;
        const int g1 = isAsk ? 200 : 255;
        const int b1 = isAsk ? 120 : 220;

        const int r = static_cast<int>(r0 + (r1 - r0) * curve);
        const int g = static_cast<int>(g0 + (g1 - g0) * curve);
        const int b = static_cast<int>(b0 + (b1 - b0) * curve);
        row[i] = qRgba(std::clamp(r, 0, 255),
                       std::clamp(g, 0, 255),
                       std::clamp(b, 0, 255),
                       255);
    }
}

void UnifiedGridRenderer::buildGlyphAtlases() {
    if (m_glyphAtlasesBuilt) {
        return;
    }
    static const QString charset = QStringLiteral("0123456789.kMB$+-");
    const std::array<int, 5> sizes = {12, 20, 32, 48, 96};
    const QFont baseFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (size_t i = 0; i < m_glyphAtlases.size(); ++i) {
        QFont font = baseFont;
        font.setPixelSize(sizes[i]);
        m_glyphAtlases[i].build(font, charset);
        if (qEnvironmentVariableIsSet("SENTINEL_DUMP_GLYPH_ATLAS")) {
            const QString path = QString("/tmp/sentinel_glyph_atlas_%1.png").arg(sizes[i]);
            m_glyphAtlases[i].image().save(path);
        }
    }
    m_glyphAtlasesBuilt = true;
}

int UnifiedGridRenderer::pickFontBucket(float cellHeight) const {
    if (cellHeight < 18.0f) {
        return 0;
    }
    if (cellHeight < 28.0f) {
        return 1;
    }
    if (cellHeight < 48.0f) {
        return 2;
    }
    if (cellHeight < 80.0f) {
        return 3;
    }
    return 4;
}

float UnifiedGridRenderer::fontBucketPx(int bucket) const {
    switch (bucket) {
    case 0:
        return 12.0f;
    case 1:
        return 20.0f;
    case 2:
        return 32.0f;
    case 3:
        return 48.0f;
    default:
        return 96.0f;
    }
}

void UnifiedGridRenderer::applyLabelUploads(
    const std::vector<HeatmapStreamState::PendingLabelColumn>& uploads,
    int gridSize) {
    if (gridSize <= 0 || uploads.empty()) {
        return;
    }
    const size_t expectedSize = static_cast<size_t>(gridSize) * gridSize;
    if (m_labelLiquidityRing.size() != expectedSize) {
        m_labelLiquidityRing.assign(expectedSize, 0);
    }
    if (m_labelIntensityRing.size() != expectedSize) {
        m_labelIntensityRing.assign(expectedSize, 0);
    }
    if (m_labelLiquidityScales.size() != static_cast<size_t>(gridSize)) {
        m_labelLiquidityScales.assign(gridSize, 1.0);
    }

    const int expectedLiquidityBytes = gridSize * static_cast<int>(sizeof(uint16_t));
    for (const auto& upload : uploads) {
        const int column = upload.x;
        if (column < 0 || column >= gridSize) {
            continue;
        }
        if (upload.intensity.size() == gridSize) {
            const auto* src = reinterpret_cast<const uint8_t*>(upload.intensity.constData());
            for (int y = 0; y < gridSize; ++y) {
                m_labelIntensityRing[static_cast<size_t>(y) * gridSize + column] = src[y];
            }
        }
        if (upload.liquidity.size() == expectedLiquidityBytes) {
            const auto* src = reinterpret_cast<const uint16_t*>(upload.liquidity.constData());
            for (int y = 0; y < gridSize; ++y) {
                const uint16_t raw = qFromLittleEndian(src[y]);
                m_labelLiquidityRing[static_cast<size_t>(y) * gridSize + column] = raw;
            }
            m_labelLiquidityScales[column] = upload.liquidityScale;
        }
    }
}

void UnifiedGridRenderer::fitHeatmapToDataRange() {
    const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    if (!m_viewState || snapshot.appendMs <= 0 || snapshot.gridSize <= 0) {
        return;
    }
    if (snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return;
    }
    const int64_t bufferSpanMs = std::max<int64_t>(
        1, static_cast<int64_t>(snapshot.gridSize) * snapshot.appendMs);
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

// ===== GPU STATS DEBUG API =====
QString UnifiedGridRenderer::getTextureSize() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridSize > 0) {
            return QString("%1x%2").arg(snapshot.gridSize).arg(snapshot.gridSize);
        }
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridSize <= 0) {
            return "N/A";
        }
        // Grayscale8 = 1 byte per pixel
        qint64 bytes = static_cast<qint64>(snapshot.gridSize) * snapshot.gridSize;
        double mb = bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
    if (m_useGpuHeatmap) {
        return "Grayscale8";
    }
    return "N/A";
}

double UnifiedGridRenderer::getUploadBandwidth() const {
    return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridSize > 0) {
            return QString("%1/%2").arg(m_heatmapStream->writeColumn()).arg(snapshot.gridSize);
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

// ===== QT EVENT HANDLERS =====
// Mouse and wheel event handling for user interaction
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
        // Keep visual pan until resync arrives to avoid snap-back
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

// ===== QML PROPERTY GETTERS =====
// Read-only property access for QML bindings
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

// ===== QML DEBUG API =====
// Debug and monitoring methods for QML
QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Size:%1x%2").arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
QString UnifiedGridRenderer::getPerformanceStats() const { return "N/A (SentinelMonitor removed)"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_currentFps.load(); }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

// ===== QML DATA API =====
// Methods for data input and manipulation from QML
void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
void UnifiedGridRenderer::togglePerformanceOverlay() { /* No-op: SentinelMonitor removed */ }

// ===== QML PAN/ZOOM CONTROLS =====
// Methods for pan and zoom control from QML
void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); update(); } }
