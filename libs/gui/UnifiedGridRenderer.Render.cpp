// UnifiedGridRenderer render-thread hot path split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/VolumeProfileState.hpp"
#include "render/TpoDebugTrace.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QTimeZone>
#include <QtEndian>
#include <algorithm>
#include <sstream>
UnifiedGridRenderer::FrameContext UnifiedGridRenderer::buildFrameContext() const {
    FrameContext frame;
    frame.surfaceBounds = boundingRect();
    frame.surfaceDpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qint64 steadyNowMs = m_heatmapClock.isValid() ? m_heatmapClock.elapsed() : 0;
    frame.time = m_timeAuthority.snapshot(steadyNowMs);
    frame.presentationTimeMs = frame.time.nowPresentationMs;
    frame.heatmapSnapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    frame.overlays.heatmap = m_heatmapLayerEnabled;
    frame.overlays.footprint = m_footprintLayerEnabled;
    frame.overlays.tpo = m_tpoLayerEnabled;
    frame.forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
    frame.streamGenerations.heatmap = m_heatmapStreamGeneration.load(std::memory_order_acquire);
    frame.streamGenerations.footprint = m_footprintStreamGeneration.load(std::memory_order_acquire);
    frame.streamGenerations.candle = m_candleStreamGeneration.load(std::memory_order_acquire);
    if (m_viewState && m_viewState->isTimeWindowValid()) {
        frame.viewport.valid = true;
        frame.viewport.timeStart = m_viewState->getVisibleTimeStart();
        frame.viewport.timeEnd = m_viewState->getVisibleTimeEnd();
        frame.viewport.minPrice = m_viewState->getMinPrice();
        frame.viewport.maxPrice = m_viewState->getMaxPrice();
        frame.viewport.panVisualOffset = m_viewState->getPanVisualOffset();
        frame.viewport.dragging = m_viewState->isDragging();
        frame.viewport.autoScrollEnabled = m_viewState->isAutoScrollEnabled();
    }
    return frame;
}

HeatmapIntensityNode* UnifiedGridRenderer::ensureHeatmapRootNode(QSGNode* oldNode) {
    auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
    if (!texNode) {
        texNode = new HeatmapIntensityNode();
        m_heatmapOverlay.onRootRebuilt();
        m_chartTextRenderer.onRootRebuilt();
        m_footprintOverlay.onRootRebuilt();
        m_tpoOverlay.onRootRebuilt();
        m_vpRenderer.onRootRebuilt();
    }
    return texNode;
}

void UnifiedGridRenderer::computeAndApplyFrameMapping(FrameContext& frame,
                                                       HeatmapIntensityNode* texNode,
                                                       int64_t cadenceMs,
                                                       int gridWidth,
                                                       int gridHeight) {
    const auto& snapshot = frame.heatmapSnapshot;
    m_heatmapOverlay.setGridDimensions(gridWidth, gridHeight);
    m_heatmapOverlay.setIntensityBytesPerCell(m_intensityBytesPerCell);
    m_heatmapOverlay.setBackgroundColor(m_heatmapBackgroundColor);

    const QRectF bounds = frame.surfaceBounds;
    UgrFrameMath::ViewportState viewportState;
    viewportState.valid = frame.viewport.valid;
    viewportState.timeStart = static_cast<double>(frame.viewport.timeStart);
    viewportState.timeEnd = static_cast<double>(frame.viewport.timeEnd);
    viewportState.minPrice = frame.viewport.minPrice;
    viewportState.maxPrice = frame.viewport.maxPrice;
    viewportState.panVisualOffset = frame.viewport.panVisualOffset;
    viewportState.dragging = frame.viewport.dragging;
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
    gridState.forceFull = frame.forceFull;

    const UgrFrameMath::RenderRects renderRects =
        UgrFrameMath::computeRenderRects(bounds, viewportState, gridState);
    texNode->setRect(renderRects.drawRect);
    texNode->setSourceRect(renderRects.srcRect);
    if (!frame.forceFull) {
        texNode->setTimeOffset(snapshot.timeOffset);
    }

    frame.mapping.viewStartMs = renderRects.viewTimeStart;
    frame.mapping.viewEndMs = renderRects.viewTimeEnd;
    frame.mapping.viewMinPrice = renderRects.viewMinPrice;
    frame.mapping.viewMaxPrice = renderRects.viewMaxPrice;
    frame.mapping.drawRect = renderRects.drawRect;
    frame.mapping.srcRect = renderRects.srcRect;
    frame.mapping.dataStartMs = renderRects.dataStart;
    frame.mapping.dataEndMs = renderRects.dataEnd;
    frame.mapping.actualDataStartMs = renderRects.actualDataStart;
    frame.mapping.actualDataEndMs = renderRects.actualDataEnd;
    frame.mapping.dataMinPrice = snapshot.minPrice;
    frame.mapping.dataMaxPrice = snapshot.maxPrice;
    frame.mapping.appendMs = static_cast<double>(cadenceMs);
    frame.mapping.tickSize = snapshot.tickSize;
    frame.mapping.gridWidth = gridWidth;
    frame.mapping.gridHeight = gridHeight;
    frame.mapping.filledColumns = snapshot.filledColumns;
    frame.mapping.timeOffset = frame.forceFull ? 0.0f : snapshot.timeOffset;
    frame.mapping.valid = (renderRects.dataStartValid &&
                           snapshot.timeOriginMs != 0 &&
                           cadenceMs > 0 &&
                           gridWidth > 0 &&
                           renderRects.drawRect.width() > 0.0 &&
                           renderRects.srcRect.width() > 0.0);
    frame.mapping.cellW = frame.mapping.valid
        ? (renderRects.drawRect.width() / renderRects.srcRect.width()) : 0.0;
    frame.mapping.cellH = frame.mapping.valid && renderRects.srcRect.height() > 0.0
        ? (renderRects.drawRect.height() / renderRects.srcRect.height()) : 0.0;
    m_lastTimeAxisMapping = frame.mapping;
}

void UnifiedGridRenderer::publishFrameContext(const FrameContext& frame) {
    MappingFrameContext published;
    published.surfaceBounds = frame.surfaceBounds;
    published.surfaceDpr = frame.surfaceDpr;
    published.presentationTimeMs = frame.presentationTimeMs;
    published.activeTimeframeMs = frame.time.activeTimeframeMs;
    published.nowEventTimeMs = frame.time.nowEventMs;
    published.currentBoundaryStartMs = frame.time.currentBoundaryStartMs;
    published.nextBoundaryStartMs = frame.time.nextBoundaryStartMs;
    published.boundarySequence = frame.time.boundarySequence;
    published.hasEventTime = frame.time.hasEvent;
    published.viewportValid = frame.viewport.valid;
    published.viewportTimeStart = frame.viewport.timeStart;
    published.viewportTimeEnd = frame.viewport.timeEnd;
    published.viewportMinPrice = frame.viewport.minPrice;
    published.viewportMaxPrice = frame.viewport.maxPrice;
    published.viewportPanVisualOffset = frame.viewport.panVisualOffset;
    published.viewportDragging = frame.viewport.dragging;
    published.viewportAutoScrollEnabled = frame.viewport.autoScrollEnabled;
    published.heatmapGeneration = frame.streamGenerations.heatmap;
    published.footprintGeneration = frame.streamGenerations.footprint;
    published.candleGeneration = frame.streamGenerations.candle;
    published.mapping = frame.mapping;
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    m_lastFrameContext = published;
}

void UnifiedGridRenderer::drainFrameUploads(
    std::vector<HeatmapOverlayRenderer::PendingUpload>& heatmapUploads,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads,
    std::vector<TpoOverlayRenderer::PendingUpload>& tpoUploads) {
    {
        std::lock_guard<std::mutex> lock(m_footprintPendingMutex);
        if (!m_pendingFootprintUploads.empty()) {
            footprintUploads.swap(m_pendingFootprintUploads);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_tpoPendingMutex);
        if (!m_pendingTpoUploads.empty()) {
            tpoUploads.swap(m_pendingTpoUploads);
        }
    }

    if (!m_heatmapStream) {
        return;
    }
    std::vector<HeatmapStreamState::PendingColumn> pendingUploads;
    m_heatmapStream->takePendingUploads(pendingUploads);
    heatmapUploads.reserve(pendingUploads.size());
    for (auto& upload : pendingUploads) {
        heatmapUploads.push_back({upload.x, std::move(upload.data)});
    }
}

void UnifiedGridRenderer::renderOverlays(
    HeatmapIntensityNode* texNode,
    const FrameContext& frame,
    bool drawHeatmap,
    bool drawFootprint,
    bool drawTpo,
    int gridWidth,
    int gridHeight,
    std::vector<HeatmapOverlayRenderer::PendingUpload>& heatmapUploads,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads,
    std::vector<TpoOverlayRenderer::PendingUpload>& tpoUploads) {
    const auto& snapshot = frame.heatmapSnapshot;
    const QRectF drawRect = frame.mapping.drawRect;
    const QRectF srcRect = frame.mapping.srcRect;
    m_heatmapOverlay.applyToNode(window(),
                                 texNode,
                                 drawHeatmap,
                                 static_cast<float>(m_heatmapGamma),
                                 static_cast<float>(m_heatmapContrast),
                                 static_cast<float>(m_heatmapShaderFloor),
                                 frame.forceFull,
                                 snapshot.timeOffset,
                                 drawRect,
                                 srcRect,
                                 heatmapUploads);
    m_footprintOverlay.render(window(),
                              texNode,
                              drawFootprint,
                              frame.forceFull,
                              snapshot.timeOffset,
                              drawRect,
                              srcRect,
                              gridWidth,
                              gridHeight,
                              footprintUploads);
    // ── TPO / VP dispatch ──────────────────────────────────────────────────
    std::vector<float> localBins;
    VolumeProfileState::Snapshot localSnap;
    {
        std::lock_guard<std::mutex> lock(m_vpMutex);
        if (m_vpDirty || !m_vpBins.empty()) {
            localBins = m_vpBins;
            localSnap = m_vpSnap;
            m_vpDirty = false;
        }
    }

    m_vpRenderer.render(texNode,
                        m_volumeProfileLayerEnabled && !localBins.empty(),
                        drawRect,
                        frame.viewport.minPrice,
                        frame.viewport.maxPrice,
                        localBins,
                        localSnap);

    int64_t tpoSessionStart = 0;
    int64_t tpoSessionEnd = 0;
    int64_t tpoBracketMs = 0;
    int tpoSessionColumns = 0;
    {
        std::lock_guard<std::mutex> lock(m_tpoPendingMutex);
        tpoSessionStart = m_tpoSessionStartMs;
        tpoSessionEnd = m_tpoSessionEndMs;
        tpoBracketMs = m_tpoBracketMs;
        tpoSessionColumns = m_tpoSessionColumns;
    }
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG") &&
        drawTpo &&
        tpoSessionStart > 0 &&
        tpoSessionEnd > tpoSessionStart &&
        tpoBracketMs > 0) {
        static QElapsedTimer tpoSessionLogTimer;
        static bool tpoSessionLogTimerStarted = false;
        if (!tpoSessionLogTimerStarted) {
            tpoSessionLogTimer.start();
            tpoSessionLogTimerStarted = true;
        }
        if (tpoSessionLogTimer.elapsed() > 1000) {
            const int computedColumns = static_cast<int>(
                std::max<int64_t>(1, (tpoSessionEnd - tpoSessionStart) / tpoBracketMs));
            const QDateTime startDt = QDateTime::fromMSecsSinceEpoch(tpoSessionStart, QTimeZone::utc());
            const QDateTime endDt = QDateTime::fromMSecsSinceEpoch(tpoSessionEnd, QTimeZone::utc());
            sLog_Debug(QString("TPO session: %1-%2 UTC | Bracket: %3m | Columns: %4")
                           .arg(startDt.toString(QStringLiteral("HH:mm")))
                           .arg(endDt.toString(QStringLiteral("HH:mm")))
                           .arg(tpoBracketMs / 60000)
                           .arg((tpoSessionColumns > 0) ? tpoSessionColumns : computedColumns));
            tpoSessionLogTimer.restart();
        }
    }
    if (tpo_debug::enabled() && drawTpo) {
        static QElapsedTimer tpoFileLogTimer;
        static bool tpoFileLogTimerStarted = false;
        if (!tpoFileLogTimerStarted) {
            tpoFileLogTimer.start();
            tpoFileLogTimerStarted = true;
        }
        if (tpoFileLogTimer.elapsed() > 250) {
            std::ostringstream payload;
            payload << "{"
                    << "\"sessionStartMs\":" << tpoSessionStart
                    << ",\"sessionEndMs\":" << tpoSessionEnd
                    << ",\"bracketMs\":" << tpoBracketMs
                    << ",\"sessionColumns\":" << tpoSessionColumns
                    << ",\"viewStartMs\":" << frame.mapping.viewStartMs
                    << ",\"viewEndMs\":" << frame.mapping.viewEndMs
                    << ",\"drawRectX\":" << drawRect.x()
                    << ",\"drawRectW\":" << drawRect.width()
                    << ",\"srcRectX\":" << srcRect.x()
                    << ",\"srcRectW\":" << srcRect.width()
                    << ",\"mappingValid\":" << (frame.mapping.valid ? "true" : "false")
                    << "}";
            tpo_debug::append("UnifiedGridRenderer.Render.cpp:renderOverlays",
                              "tpo_render_context",
                              "H4",
                              payload.str());
            tpoFileLogTimer.restart();
        }
    }
    m_tpoOverlay.render(window(),
                        texNode,
                        drawTpo,
                        frame.forceFull,
                        snapshot.timeOffset,
                        drawRect,
                        srcRect,
                        gridWidth,
                        gridHeight,
                        tpoUploads,
                        tpoSessionStart,
                        tpoSessionEnd,
                        frame.mapping.viewStartMs,
                        frame.mapping.viewEndMs,
                        frame.surfaceBounds); // world→screen base; must match candle/label mapping
}

void UnifiedGridRenderer::updateLabelGeometry(HeatmapIntensityNode* texNode,
                                              const FrameContext& frame,
                                              const HeatmapStreamState::Snapshot& snapshot,
                                              int gridWidth,
                                              int gridHeight) {
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

    const QRectF drawRect = frame.mapping.drawRect;
    const QRectF srcRectCurrent = texNode->getSourceRect();
    const bool labelVisible = (!drawRect.isEmpty() && !frame.surfaceBounds.isEmpty() &&
                               srcRectCurrent.width() > 0.0 && srcRectCurrent.height() > 0.0 &&
                               snapshot.liquidityAvailable &&
                               m_labelRingGridWidth == gridWidth &&
                               m_labelRingGridHeight == gridHeight);
    const float cellH = (srcRectCurrent.height() > 0.0f)
        ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRectCurrent.height())
        : 0.0f;
    const float cellW = (srcRectCurrent.width() > 0.0f)
        ? static_cast<float>(drawRect.width()) / static_cast<float>(srcRectCurrent.width())
        : 0.0f;

    // Only render labels when we are zoomed in enough for both height and width to support text
    const float minCellH = 11.0f;
    const float minCellW = 24.0f;

    if (!(labelVisible && cellH >= minCellH && cellW >= minCellW && m_chartTextAtlasBuilt && window())) {
        if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG")) {
            static QElapsedTimer labelDebugTimer;
            static bool labelDebugStarted = false;
            if (!labelDebugStarted) {
                labelDebugTimer.start();
                labelDebugStarted = true;
            }
            if (labelDebugTimer.elapsed() > 1000) {
                sLog_Debug(QString("Heatmap text gated: visible=%1 cellH=%2 cellW=%3 atlas=%4 window=%5")
                               .arg(labelVisible ? 1 : 0)
                               .arg(cellH, 0, 'f', 2)
                               .arg(cellW, 0, 'f', 2)
                               .arg(m_chartTextAtlasBuilt ? 1 : 0)
                               .arg(window() ? 1 : 0));
                labelDebugTimer.restart();
            }
        }
        clearLabelGeometry();
        return;
    }

    const float fontPx = static_cast<float>(m_chartTextAtlas.fontPx());
    
    // Smooth ramp for legible text scaling based on cell size
    const float appearMinPx = 11.0f;
    const float fullSizePx = 22.0f;
    const float maxScale = 0.95f; 
    const float minScale = 0.45f;
    const float cellMin = std::min(cellW, cellH);
    
    // Smoothstep interpolation (t * t * (3 - 2t))
    const float t = std::clamp((cellMin - appearMinPx) / (fullSizePx - appearMinPx), 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    const float easedScale = minScale + (maxScale - minScale) * eased;
    
    // Guaranteed hard bounds limit so text NEVER crosses the cell walls regardless of the S-curve
    const float vertScale = (cellH * 0.75f) / fontPx;
    const float horizScale = (cellW * 0.85f) / (fontPx * 2.5f);
    const float safeScale = std::min(vertScale, horizScale);
    
    const float scale = (fontPx > 0.0f) ? std::min(easedScale, safeScale) : 1.0f;

    if (m_heatmapLabelGlyphs.capacity() < 32000) {
        m_heatmapLabelGlyphs.reserve(32000);
    }

    const bool dollars = (m_liquidityLabelMode != 0);
    HeatmapLabelRenderer::buildLabelGlyphs(frame.mapping,
                                           snapshot,
                                           m_chartTextAtlas,
                                           m_labelLiquidityRing,
                                           m_labelIntensityRing,
                                           m_labelLiquidityScales,
                                           scale,
                                           dollars,
                                           m_heatmapLabelGlyphs);
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG")) {
        static QElapsedTimer labelSubmitTimer;
        static bool labelSubmitStarted = false;
        if (!labelSubmitStarted) {
            labelSubmitTimer.start();
            labelSubmitStarted = true;
        }
        if (labelSubmitTimer.elapsed() > 1000) {
            sLog_Debug(QString("Heatmap text submit: glyphs=%1 cellH=%2 cellW=%3 scale=%4")
                           .arg(static_cast<int>(m_heatmapLabelGlyphs.size()))
                           .arg(cellH, 0, 'f', 2)
                           .arg(cellW, 0, 'f', 2)
                           .arg(scale, 0, 'f', 2));
            labelSubmitTimer.restart();
        }
    }
    m_chartTextRenderer.submitGlyphs(m_heatmapLabelGlyphs, ChartTextRenderer::Priority::Low);
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

void UnifiedGridRenderer::clearLabelGeometry() {
    m_heatmapLabelGlyphs.clear();
}

void UnifiedGridRenderer::updateFpsEstimate() {
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
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0 || !m_useGpuHeatmap) {
        return oldNode;
    }

    FrameContext frame = buildFrameContext();
    const auto& snapshot = frame.heatmapSnapshot;
    const int64_t cadenceMs = (frame.time.activeTimeframeMs > 0)
        ? frame.time.activeTimeframeMs
        : static_cast<int64_t>(snapshot.appendMs);
    const bool drawHeatmap = frame.overlays.heatmap;
    const bool textOnlyDebug = qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_ONLY");
    const bool drawFootprint = frame.overlays.footprint;
    const bool drawTpo = frame.overlays.tpo;
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        static QElapsedTimer renderDebugTimer;
        static bool renderDebugTimerStarted = false;
        if (!renderDebugTimerStarted) {
            renderDebugTimer.start();
            renderDebugTimerStarted = true;
        }
        if (renderDebugTimer.elapsed() > 1000) {
            const int64_t incomingTfMs = m_lastIncomingHeatmapSliceTimeframeMs.load(std::memory_order_relaxed);
            sLog_Debug(QString("Render frame: overlays[h=%1 fp=%2 tpo=%3] primary=%4 active_tf=%5ms incoming_slice_tf=%6ms append=%7ms grid=%8x%9")
                           .arg(drawHeatmap ? 1 : 0)
                           .arg(drawFootprint ? 1 : 0)
                           .arg(drawTpo ? 1 : 0)
                           .arg(m_primaryField)
                           .arg(cadenceMs)
                           .arg(incomingTfMs)
                           .arg(snapshot.appendMs)
                           .arg(gridWidth)
                           .arg(gridHeight));
            renderDebugTimer.restart();
        }
    }

    auto* texNode = ensureHeatmapRootNode(oldNode);
    computeAndApplyFrameMapping(frame, texNode, cadenceMs, gridWidth, gridHeight);
    publishFrameContext(frame);

    std::vector<HeatmapOverlayRenderer::PendingUpload> framePendingHeatmapUploads;
    std::vector<FootprintOverlayRenderer::PendingUpload> framePendingFootprintUploads;
    std::vector<TpoOverlayRenderer::PendingUpload> framePendingTpoUploads;
    drainFrameUploads(framePendingHeatmapUploads,
                      framePendingFootprintUploads,
                      framePendingTpoUploads);
    renderOverlays(texNode,
                   frame,
                   drawHeatmap && !textOnlyDebug,
                   drawFootprint,
                   drawTpo,
                   gridWidth,
                   gridHeight,
                   framePendingHeatmapUploads,
                   framePendingFootprintUploads,
                   framePendingTpoUploads);

    m_chartTextRenderer.beginFrame(texNode, window(), m_chartTextAtlas);
    submitAxisText();
    if (drawHeatmap) {
        updateLabelGeometry(texNode, frame, snapshot, gridWidth, gridHeight);
    } else {
        clearLabelGeometry();
    }
    m_chartTextRenderer.endFrame();
    if (m_chartTextRenderer.droppedGlyphs() > 0 && qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Chart text dropped glyphs: total=%1 high=%2 low=%3")
                       .arg(m_chartTextRenderer.droppedGlyphs())
                       .arg(m_chartTextRenderer.droppedHighGlyphs())
                       .arg(m_chartTextRenderer.droppedLowGlyphs()));
    }
    updateFpsEstimate();
    return texNode;
}


