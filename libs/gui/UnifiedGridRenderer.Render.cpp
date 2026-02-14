// UnifiedGridRenderer render-thread hot path split from main TU.
#include "UnifiedGridRenderer.h"

#include "render/HeatmapIntensityNode.hpp"
#include "render/MsdfGlyphNode.hpp"
#include "render/UgrFrameMath.hpp"

#include <algorithm>
UnifiedGridRenderer::FrameContext UnifiedGridRenderer::buildFrameContext() const {
    FrameContext frame;
    frame.surfaceBounds = boundingRect();
    frame.surfaceDpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qint64 steadyNowMs = m_heatmapClock.isValid() ? m_heatmapClock.elapsed() : 0;
    frame.time = m_timeAuthority.snapshot(steadyNowMs);
    frame.presentationTimeMs = frame.time.nowPresentationMs;
    frame.heatmapSnapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    frame.overlays.heatmap = (m_primaryField == 0);
    frame.overlays.footprint = (m_primaryField == 1);
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
        m_whiteGlyphNode = nullptr;
        m_blackGlyphNode = nullptr;
        m_footprintOverlay.onRootRebuilt();
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
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads) {
    {
        std::lock_guard<std::mutex> lock(m_footprintPendingMutex);
        if (!m_pendingFootprintUploads.empty()) {
            footprintUploads.swap(m_pendingFootprintUploads);
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
    int gridWidth,
    int gridHeight,
    std::vector<HeatmapOverlayRenderer::PendingUpload>& heatmapUploads,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads) {
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
}

void UnifiedGridRenderer::updateLabelGeometry(HeatmapIntensityNode* texNode,
                                              const FrameContext& frame,
                                              const HeatmapStreamState::Snapshot& snapshot,
                                              int gridWidth,
                                              int gridHeight) {
    HeatmapStreamState::LabelSnapshot labelSnapshot;
    const bool haveLabelSnapshot = m_heatmapStream && m_heatmapStream->copyLabelSnapshot(labelSnapshot);
    const bool labelGridMatches = haveLabelSnapshot &&
                                  labelSnapshot.snapshot.gridWidth == gridWidth &&
                                  labelSnapshot.snapshot.gridHeight == gridHeight;

    const QRectF drawRect = frame.mapping.drawRect;
    const QRectF srcRectCurrent = texNode->getSourceRect();
    const bool labelVisible = (!drawRect.isEmpty() && !frame.surfaceBounds.isEmpty() &&
                               srcRectCurrent.width() > 0.0 && srcRectCurrent.height() > 0.0 &&
                               labelGridMatches);
    const float cellH = (srcRectCurrent.height() > 0.0f)
        ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRectCurrent.height())
        : 0.0f;
    const int labelPx = (m_heatmapLabelPx > 0) ? m_heatmapLabelPx : 14;
    const float labelThreshold = static_cast<float>(labelPx);

    if (!(labelVisible && cellH >= labelThreshold && m_msdfAtlasBuilt && window())) {
        clearLabelGeometry();
        return;
    }

    const float fontPx = static_cast<float>(m_msdfAtlas.fontPx());
    const float scale = (fontPx > 0.0f) ? std::clamp(cellH / fontPx, 0.25f, 2.5f) : 1.0f;

    if (m_labelWhiteQuads.capacity() < 32000) {
        m_labelWhiteQuads.reserve(32000);
    }
    if (m_labelBlackQuads.capacity() < 32000) {
        m_labelBlackQuads.reserve(32000);
    }

    const bool dollars = (m_liquidityLabelMode != 0);
    HeatmapLabelRenderer::buildLabelQuads(frame.mapping,
                                          snapshot,
                                          m_msdfAtlas,
                                          labelSnapshot.liquidityRing,
                                          labelSnapshot.intensityRing,
                                          labelSnapshot.liquidityScales,
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
}

void UnifiedGridRenderer::clearLabelGeometry() {
    if (m_whiteGlyphNode) {
        m_labelWhiteQuads.clear();
        m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
    }
    if (m_blackGlyphNode) {
        m_labelBlackQuads.clear();
        m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
    }
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
    const bool drawFootprint = frame.overlays.footprint;
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;

    auto* texNode = ensureHeatmapRootNode(oldNode);
    computeAndApplyFrameMapping(frame, texNode, cadenceMs, gridWidth, gridHeight);
    publishFrameContext(frame);

    std::vector<HeatmapOverlayRenderer::PendingUpload> framePendingHeatmapUploads;
    std::vector<FootprintOverlayRenderer::PendingUpload> framePendingFootprintUploads;
    drainFrameUploads(framePendingHeatmapUploads, framePendingFootprintUploads);
    renderOverlays(texNode,
                   frame,
                   drawHeatmap,
                   drawFootprint,
                   gridWidth,
                   gridHeight,
                   framePendingHeatmapUploads,
                   framePendingFootprintUploads);

    if (!drawHeatmap) {
        m_whiteGlyphNode = nullptr;
        m_blackGlyphNode = nullptr;
        return texNode;
    }

    updateLabelGeometry(texNode, frame, snapshot, gridWidth, gridHeight);
    updateFpsEstimate();
    return texNode;
}


