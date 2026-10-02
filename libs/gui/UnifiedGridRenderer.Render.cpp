// UnifiedGridRenderer render-thread hot path split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "render/FrameContextBuilder.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapRowGrouping.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/VolumeProfileState.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"

#include <QElapsedTimer>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QtEndian>
#include <algorithm>
#include <cmath>

HeatmapIntensityNode* UnifiedGridRenderer::ensureHeatmapRootNode(QSGNode* oldNode) {
    if (oldNode && oldNode->type() == QSGNode::BasicNodeType) {
        // The renderer flipped back from gpu: the scene graph keeps a replaced
        // paint node, so the old root (tile node, overlays, text) goes here.
        delete oldNode;
        oldNode = nullptr;
    }
    auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
    if (!texNode) {
        texNode = new HeatmapIntensityNode();
        for (auto* overlay : m_overlays)
            overlay->onRootRebuilt();
        m_chartTextRenderer.onRootRebuilt();
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
    m_heatmapOverlay.setIntensityBytesPerCell(m_heatmapStreamService->intensityBytesPerCell());
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
    m_footprintOverlay.drainPending(footprintUploads);

    if (!m_heatmapStreamService->stream()) {
        return;
    }
    std::vector<HeatmapStreamState::PendingColumn> pendingUploads;
    m_heatmapStreamService->stream()->takePendingUploads(pendingUploads);
    heatmapUploads.reserve(pendingUploads.size());
    const double threshold = m_heatmapLiquidityThreshold;
    const int bytesPerCell = m_heatmapStreamService->intensityBytesPerCell();
    const int gridHeight = m_heatmapStreamService->gridHeight();
    const int labelMode = m_liquidityLabelMode;
    const auto streamSnap = m_heatmapStreamService->stream()->snapshot();
    for (auto& upload : pendingUploads) {
        if (threshold > 0.0 && !upload.liquidity.isEmpty() && upload.liquidityScale > 0.0) {
            const int expectedLiq = gridHeight * static_cast<int>(sizeof(uint16_t));
            if (upload.liquidity.size() == expectedLiq &&
                upload.data.size() == gridHeight * bytesPerCell) {
                const auto* liqRaw = reinterpret_cast<const uint16_t*>(upload.liquidity.constData());
                if (bytesPerCell == 1) {
                    auto* dst = reinterpret_cast<uint8_t*>(upload.data.data());
                    for (int y = 0; y < gridHeight; ++y) {
                        const uint16_t packed = qFromLittleEndian(liqRaw[y]);
                        if (packed == 0) { dst[y] = 0; continue; }
                        double val = static_cast<double>(packed) * upload.liquidityScale;
                        if (labelMode != 0 && streamSnap.tickSize > 0.0)
                            val *= (streamSnap.maxPrice - static_cast<double>(y) * streamSnap.tickSize);
                        if (val < threshold) dst[y] = 0;
                    }
                } else if (bytesPerCell == 2) {
                    auto* dst = reinterpret_cast<uint16_t*>(upload.data.data());
                    for (int y = 0; y < gridHeight; ++y) {
                        const uint16_t packed = qFromLittleEndian(liqRaw[y]);
                        if (packed == 0) { dst[y] = 0; continue; }
                        double val = static_cast<double>(packed) * upload.liquidityScale;
                        if (labelMode != 0 && streamSnap.tickSize > 0.0)
                            val *= (streamSnap.maxPrice - static_cast<double>(y) * streamSnap.tickSize);
                        if (val < threshold) dst[y] = 0;
                    }
                }
            }
        }
        // Recording mode: rows the recording never covered become the 0x8000
        // "unknown" code (ask side, zero size, never produced by the encoder), so
        // the shader draws them differently from recorded-empty rows (0).
        if (streamSnap.valueEncoding == heatmap_window::ValueEncoding::AbsoluteLogSize &&
            bytesPerCell == 2 && !upload.validity.isEmpty() &&
            upload.data.size() == gridHeight * bytesPerCell) {
            auto* dst = reinterpret_cast<uint16_t*>(upload.data.data());
            const auto* bits = reinterpret_cast<const uint8_t*>(upload.validity.constData());
            const int bitRows = std::min(gridHeight, static_cast<int>(upload.validity.size()) * 8);
            for (int y = 0; y < gridHeight; ++y) {
                const bool covered = y < bitRows && ((bits[y / 8] >> (y % 8)) & 1u);
                if (!covered) dst[y] = qToLittleEndian<uint16_t>(0x8000u);
            }
        }
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
    if (texNode) {
        // Recording columns arrive aggregated at the display tick: no client grouping.
        const bool recordingMode = snapshot.valueEncoding == heatmap_window::ValueEncoding::AbsoluteLogSize;
        if (recordingMode && snapshot.sizeFloor > 0.0 && snapshot.codesPerOctave > 0.0) {
            const recording::SizeScale scale{snapshot.sizeFloor, snapshot.codesPerOctave};
            texNode->setValueMode(true, recording::encodeSize(m_heatmapSensitivityMin, scale),
                                  recording::encodeSize(m_heatmapSensitivityMax, scale));
        } else {
            texNode->setValueMode(false, 0.0f, 1.0f);
        }
        const double columnPx = (srcRect.width() > 0.0) ? drawRect.width() / srcRect.width() : 0.0;
        const int rowGroup = recordingMode ? 1 : (drawHeatmap && srcRect.height() > 0.0)
            ? heatmap_rows::rowsPerDisplayRow(
                  drawRect.height() / srcRect.height(),
                  heatmap_rows::targetRowPx(columnPx, m_heatmapTargetRowPx, m_heatmapCellAspect))
            : 1;
        texNode->setRowGrouping(rowGroup, heatmap_rows::rowPhase(snapshot.maxPrice, snapshot.tickSize, rowGroup));

        // Zoom diagnostics (SENTINEL_PROBES=zoom): what the chart actually draws at this zoom.
        // Logged only when the view span, cell tick or grouping changes, never every frame.
        if (drawHeatmap && srcRect.height() > 0.0 && srcRect.width() > 0.0) {
            const double viewPrice = frame.mapping.viewMaxPrice - frame.mapping.viewMinPrice;
            const double viewTime = frame.mapping.viewEndMs - frame.mapping.viewStartMs;
            const uint64_t key = std::hash<double>{}(std::round(viewPrice * 100.0)) ^
                                 (std::hash<double>{}(std::round(viewTime)) << 1) ^
                                 (std::hash<double>{}(snapshot.tickSize) << 2) ^
                                 (static_cast<uint64_t>(rowGroup) << 40);
            if (key != m_lastZoomProbeKey) {
                m_lastZoomProbeKey = key;
                const double pxPerBaseRow = drawRect.height() / srcRect.height();
                sLog_Probe("zoom.frame",
                           "mode=" << (recordingMode ? "recording" : "legacy")
                           << " viewPrice=" << viewPrice
                           << " viewTimeMin=" << viewTime / 60000.0
                           << " plotPx=" << drawRect.width() << "x" << drawRect.height()
                           << " cellTick=" << snapshot.tickSize
                           << " pxPerRow=" << pxPerBaseRow
                           << " pxPerRowFromView=" << (viewPrice > 0.0 ? drawRect.height() * snapshot.tickSize / viewPrice : 0.0)
                           << " srcRect=" << srcRect.x() << "," << srcRect.y() << " " << srcRect.width() << "x" << srcRect.height()
                           << " forceFull=" << frame.forceFull
                           << " columnPx=" << columnPx
                           << " rowGroup=" << rowGroup
                           << " displayTick=" << snapshot.tickSize * rowGroup
                           << " displayRowPx=" << pxPerBaseRow * rowGroup
                           << " bandRows=" << snapshot.gridHeight
                           << " band=[" << snapshot.minPrice << ".." << snapshot.maxPrice << "]");
            }
        }
    }
    renderTradeOverlays(texNode, frame, drawFootprint, drawTpo, footprintUploads);
}

// Footprint, volume profile and TPO under `parent`, after its heatmap content.
void UnifiedGridRenderer::renderTradeOverlays(
    QSGNode* parent,
    const FrameContext& frame,
    bool drawFootprint,
    bool drawTpo,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads) {
    m_footprintOverlay.render(window(),
                              parent,
                              drawFootprint,
                              frame.mapping.viewStartMs, frame.mapping.viewEndMs,
                              frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice, frame.surfaceBounds,
                              footprintUploads);
    // ── TPO / VP dispatch ──────────────────────────────────────────────────
    std::vector<float> localBins;
    VolumeProfileState::Snapshot localSnap;
    m_vpRenderer.drainPending(localBins, localSnap);

    m_vpRenderer.render(parent,
                        m_volumeProfileLayerEnabled && !localBins.empty(),
                        frame.surfaceBounds,
                        frame.mapping.viewMinPrice,
                        frame.mapping.viewMaxPrice,
                        localBins,
                        localSnap);

    // TPO maps world -> screen with surfaceBounds + view time/price (INV-037).
    m_tpoOverlay.render(window(),
                        parent,
                        drawTpo,
                        m_chartTextAtlas,
                        m_chartTextAtlasBuilt,
                        frame.mapping.viewMinPrice, frame.mapping.viewMaxPrice,
                        frame.mapping.viewStartMs, frame.mapping.viewEndMs,
                        frame.surfaceBounds);
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

    if (m_heatmapStreamService->stream()) {
        std::vector<HeatmapStreamState::PendingLabelColumn> pendingLabelUploads;
        m_heatmapStreamService->stream()->takePendingLabelUploads(pendingLabelUploads);
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
    const float baseRowH = (srcRectCurrent.height() > 0.0f)
        ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRectCurrent.height())
        : 0.0f;
    // Display tick: labels follow the same row groups as the heatmap shader.
    // Same grouping as the colour pass, so text sits inside the drawn cells.
    const double columnPx = (srcRectCurrent.width() > 0.0) ? drawRect.width() / srcRectCurrent.width() : 0.0;
    const int rowGroup = (snapshot.valueEncoding == heatmap_window::ValueEncoding::AbsoluteLogSize)
        ? 1
        : heatmap_rows::rowsPerDisplayRow(
              baseRowH, heatmap_rows::targetRowPx(columnPx, m_heatmapTargetRowPx, m_heatmapCellAspect));
    const int rowPhase = heatmap_rows::rowPhase(snapshot.maxPrice, snapshot.tickSize, rowGroup);
    const float cellH = baseRowH * static_cast<float>(rowGroup);
    const float cellW = (srcRectCurrent.width() > 0.0f)
        ? static_cast<float>(drawRect.width()) / static_cast<float>(srcRectCurrent.width())
        : 0.0f;

    // Only render labels when we are zoomed in enough for both height and width to support text
    const float minCellH = 11.0f;
    const float minCellW = 24.0f;

    if (!(labelVisible && cellH >= minCellH && cellW >= minCellW && m_chartTextAtlasBuilt && window())) {
        sLog_Probe("text.gated",
                   "visible=" << labelVisible
                   << " cellH=" << cellH << " cellW=" << cellW
                   << " minCellH=" << minCellH << " minCellW=" << minCellW
                   << " atlas=" << m_chartTextAtlasBuilt
                   << " window=" << (window() != nullptr));
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
                                           m_heatmapLabelGlyphs,
                                           -1,
                                           rowGroup,
                                           rowPhase);
    sLog_Probe("text.submit",
               "glyphs=" << m_heatmapLabelGlyphs.size()
               << " cellH=" << cellH << " cellW=" << cellW << " scale=" << scale
               << " rowGroup=" << rowGroup);
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
    const int expectedIntensityBytes = gridHeight * m_heatmapStreamService->intensityBytesPerCell();
    for (const auto& upload : uploads) {
        const int column = upload.x;
        if (column < 0 || column >= gridWidth) {
            continue;
        }
        if (upload.intensity.size() == expectedIntensityBytes) {
            if (m_heatmapStreamService->intensityBytesPerCell() == 1) {
                const auto* src = reinterpret_cast<const uint8_t*>(upload.intensity.constData());
                for (int y = 0; y < gridHeight; ++y) {
                    m_labelIntensityRing[static_cast<size_t>(y) * gridWidth + column] =
                        static_cast<uint16_t>(src[y]) * 257;
                }
            } else if (m_heatmapStreamService->intensityBytesPerCell() == 2) {
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


// ── GPU heatmap (S6b) ─────────────────────────────────────────────────────────
QSGNode* UnifiedGridRenderer::ensureGpuRootNode(QSGNode* oldNode, heatmap::gpu::HeatmapTileNode** tile) {
    if (oldNode && oldNode->type() != QSGNode::BasicNodeType) {
        delete oldNode; // the legacy HeatmapIntensityNode root and its children
        oldNode = nullptr;
    }
    QSGNode* root = oldNode;
    if (!root) {
        root = new QSGNode();
        auto* gate = new QSGOpacityNode();
        gate->appendChildNode(new heatmap::gpu::HeatmapTileNode(m_gpuLayer->tileStatsPtr()));
        root->appendChildNode(gate);
        for (auto* overlay : m_overlays)
            overlay->onRootRebuilt();
        m_chartTextRenderer.onRootRebuilt();
    }
    *tile = static_cast<heatmap::gpu::HeatmapTileNode*>(root->firstChild()->firstChild());
    return root;
}

void UnifiedGridRenderer::computeGpuFrameMapping(FrameContext& frame, heatmap::gpu::ViewWindow& view,
                                                 double tickSize) {
    const QRectF bounds = frame.surfaceBounds;
    UgrFrameMath::ViewportState vs;
    vs.valid = frame.viewport.valid;
    vs.timeStart = static_cast<double>(frame.viewport.timeStart);
    vs.timeEnd = static_cast<double>(frame.viewport.timeEnd);
    vs.minPrice = frame.viewport.minPrice;
    vs.maxPrice = frame.viewport.maxPrice;
    vs.panVisualOffset = frame.viewport.panVisualOffset;
    vs.dragging = frame.viewport.dragging;
    vs = UgrFrameMath::applyDragPan(vs, bounds); // the drag offset moves heatmap and overlays together
    view = {vs.timeStart, vs.timeEnd, vs.minPrice, vs.maxPrice};
    const double tf = static_cast<double>(m_currentTimeframe_ms);
    const double timeSpan = vs.timeEnd - vs.timeStart, priceSpan = vs.maxPrice - vs.minPrice;
    // Before a tick is drawn the rows are one pixel tall: the mapping stays valid.
    const double tick = tickSize > 0 ? tickSize : priceSpan / std::max(1.0, bounds.height());
    auto& m = frame.mapping;
    m.viewStartMs = vs.timeStart;
    m.viewEndMs = vs.timeEnd;
    m.viewMinPrice = vs.minPrice;
    m.viewMaxPrice = vs.maxPrice;
    m.valid = vs.valid && tf > 0 && timeSpan > 0 && priceSpan > 0 && tick > 0 && !bounds.isEmpty();
    if (!m.valid) {
        m_lastTimeAxisMapping = m;
        return;
    }
    // Columns are anchored to epoch multiples of the timeframe (spec rule 3).
    m.dataStartMs = std::floor(vs.timeStart / tf) * tf;
    m.dataEndMs = std::ceil(vs.timeEnd / tf) * tf;
    m.actualDataStartMs = vs.timeStart;
    m.actualDataEndMs = vs.timeEnd;
    m.viewportColumns = true;
    m.dataMinPrice = vs.minPrice;
    m.dataMaxPrice = vs.maxPrice;
    m.appendMs = tf;
    m.tickSize = tick;
    m.drawRect = bounds;
    m.srcRect = QRectF((vs.timeStart - m.dataStartMs) / tf, 0.0, timeSpan / tf, priceSpan / tick);
    m.gridWidth = static_cast<int>(std::ceil(m.srcRect.right())) + 1;
    m.gridHeight = static_cast<int>(std::ceil(m.srcRect.height()));
    m.filledColumns = m.gridWidth;
    m.timeOffset = 0.0f;
    m.cellW = bounds.width() / m.srcRect.width();
    m.cellH = bounds.height() / m.srcRect.height();
    m_lastTimeAxisMapping = m;
}

// S7b liquidity labels: the layer's matched LabelCells, laid out by the reused
// HeatmapLabelLayout (no per-frame allocation), submitted as low-priority text.
void UnifiedGridRenderer::updateGpuLabels(const FrameContext& frame, bool prepared) {
    if (m_heatmapLabelGlyphs.capacity() < heatmap::gpu::HeatmapLabelLayout::kMaxGlyphs)
        m_heatmapLabelGlyphs.reserve(heatmap::gpu::HeatmapLabelLayout::kMaxGlyphs);
    const auto labels = prepared && m_chartTextAtlasBuilt && window() ? m_gpuLayer->labelsForFrame() : nullptr;
    m_gpuLabelSerial = labels ? labels->key.serial : 0;
    if (!labels) {
        m_heatmapLabelGlyphs.clear();
        return;
    }
    const auto& settings = m_gpuLayer->settings();
    heatmap::gpu::LabelStyle style;
    style.usd = settings.labelCurrency != "asset";
    style.minPx = settings.labelMinPx;
    style.maxPx = settings.labelMaxPx;
    style.window = {m_gpuLayer->drawStyle().codeFloor, m_gpuLayer->drawStyle().codeRange};
    style.palette = m_gpuLayer->palette();
    m_gpuLabels.layout(labels, m_chartTextAtlas, frame.mapping, window()->effectiveDevicePixelRatio(), style,
                       m_heatmapLabelGlyphs);
    const auto& st = m_gpuLabels.stats();
    sLog_Probe("heatmap.labels.layout", "serial=" << labels->key.serial << " labels=" << st.labels
               << " glyphs=" << st.glyphs << " sizePx=" << st.sizePx << " narrow=" << st.tooNarrow
               << " dropped=" << st.droppedBudget);
    m_chartTextRenderer.submitGlyphs(m_heatmapLabelGlyphs, ChartTextRenderer::Priority::Low);
}

QSGNode* UnifiedGridRenderer::updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile) {
    heatmap::gpu::HeatmapTileNode* tile = nullptr;
    QSGNode* root = ensureGpuRootNode(oldNode, &tile);
    heatmap::gpu::ViewWindow view;
    computeGpuFrameMapping(frame, view, 0.0);
    const bool drawHeatmap = frame.overlays.heatmap && frame.mapping.valid;
    heatmap::gpu::HeatmapTileNode::Frame tileFrame;
    const bool prepared = drawHeatmap && m_gpuLayer->prepareFrame(tileFrame, frame.surfaceBounds, view);
    // The mapping's rows follow the drawn tick (heatmapTickSize, PriceAxisModel).
    if (m_gpuLayer->tickPrice() > 0) computeGpuFrameMapping(frame, view, m_gpuLayer->tickPrice());
    auto* gate = static_cast<QSGOpacityNode*>(root->firstChild());
    const double opacity = prepared ? 1.0 : 0.0; // 0 blocks the subtree: no prepare(), no draw
    if (gate->opacity() != opacity) gate->setOpacity(opacity);
    if (prepared) tile->setFrame(std::move(tileFrame));
    publishFrameContext(frame);
    m_pendingFrameRevision = frame.controlRevision;
    m_pendingFrameId = frame.frameId;
    if (profile) m_frameProfiler.mark(FrameProfiler::Mapping);

    std::vector<FootprintOverlayRenderer::PendingUpload> footprintUploads;
    m_footprintOverlay.drainPending(footprintUploads);
    if (profile) m_frameProfiler.mark(FrameProfiler::Uploads);
    renderTradeOverlays(root, frame, frame.overlays.footprint, frame.overlays.tpo, footprintUploads);
    if (profile) m_frameProfiler.mark(FrameProfiler::Overlays);

    m_chartTextRenderer.beginFrame(root, window(), m_chartTextAtlas);
    if (m_axisTextService) {
        m_axisTextService->submitAxisText(m_chartTextRenderer, m_chartTextAtlas, width(), height());
    }
    if (profile) m_frameProfiler.mark(FrameProfiler::AxisText);
    updateGpuLabels(frame, prepared);
    if (profile) m_frameProfiler.mark(FrameProfiler::Labels);
    m_chartTextRenderer.endFrame();
    if (profile) {
        m_frameProfiler.mark(FrameProfiler::TextEnd);
        const QString report = m_frameProfiler.endFrame();
        if (!report.isEmpty()) sLog_Render(report);
    }
    return root;
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0 || !m_useGpuHeatmap) {
        return oldNode;
    }
    const bool profile = FrameProfiler::enabled();
    if (profile) m_frameProfiler.beginFrame();

    FrameContext frame = FrameContextBuilder::build(
        boundingRect(), window(),
        m_heatmapStreamService->clock(), m_heatmapStreamService->timeAuthority(),
        m_heatmapStreamService->stream(), m_viewState.get(),
        m_heatmapLayerEnabled, m_footprintLayerEnabled, m_tpoLayerEnabled,
        m_heatmapStreamService->streamGeneration(),
        m_footprintStreamGeneration.load(std::memory_order_acquire),
        m_candleStreamGeneration.load(std::memory_order_acquire));
    frame.controlRevision = m_controlRevision.load(std::memory_order_acquire);
    frame.selectionEpoch = m_controlSelectionEpoch.load(std::memory_order_acquire);
    frame.viewportVersion = m_controlViewportVersion.load(std::memory_order_acquire);
    frame.frameId = ++m_nextFrameId;
    if (m_gpuHeatmap) return updateGpuPaintNode(oldNode, frame, profile); // the one branch (plan section 2)
    const auto& snapshot = frame.heatmapSnapshot;
    const int64_t cadenceMs = (frame.time.activeTimeframeMs > 0)
        ? frame.time.activeTimeframeMs
        : static_cast<int64_t>(snapshot.appendMs);
    const bool drawHeatmap = frame.overlays.heatmap;
    const bool textOnlyDebug = qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_ONLY");
    const bool drawFootprint = frame.overlays.footprint;
    const bool drawTpo = frame.overlays.tpo;
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapStreamService->gridWidth();
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapStreamService->gridHeight();
    sLog_Probe("frame.state",
               "heatmap=" << drawHeatmap << " footprint=" << drawFootprint << " tpo=" << drawTpo
               << " cadenceMs=" << cadenceMs
               << " incomingSliceTfMs="
               << m_lastIncomingHeatmapSliceTimeframeMs.load(std::memory_order_relaxed)
               << " appendMs=" << snapshot.appendMs
               << " grid=" << gridWidth << "x" << gridHeight);

    if (profile) m_frameProfiler.mark(FrameProfiler::Context);
    auto* texNode = ensureHeatmapRootNode(oldNode);
    computeAndApplyFrameMapping(frame, texNode, cadenceMs, gridWidth, gridHeight);
    publishFrameContext(frame);
    m_pendingFrameRevision = frame.controlRevision;
    m_pendingFrameId = frame.frameId;
    if (profile) m_frameProfiler.mark(FrameProfiler::Mapping);

    std::vector<HeatmapOverlayRenderer::PendingUpload> framePendingHeatmapUploads;
    std::vector<FootprintOverlayRenderer::PendingUpload> framePendingFootprintUploads;
    drainFrameUploads(framePendingHeatmapUploads,
                      framePendingFootprintUploads);
    if (profile) m_frameProfiler.mark(FrameProfiler::Uploads);
    renderOverlays(texNode,
                   frame,
                   drawHeatmap && !textOnlyDebug,
                   drawFootprint,
                   drawTpo,
                   gridWidth,
                   gridHeight,
                   framePendingHeatmapUploads,
                   framePendingFootprintUploads);

    if (profile) m_frameProfiler.mark(FrameProfiler::Overlays);
    m_chartTextRenderer.beginFrame(texNode, window(), m_chartTextAtlas);
    if (m_axisTextService) {
        m_axisTextService->submitAxisText(m_chartTextRenderer, m_chartTextAtlas, width(), height());
    }
    if (profile) m_frameProfiler.mark(FrameProfiler::AxisText);
    if (drawHeatmap) {
        updateLabelGeometry(texNode, frame, snapshot, gridWidth, gridHeight);
    } else {
        clearLabelGeometry();
    }
    if (profile) m_frameProfiler.mark(FrameProfiler::Labels);
    m_chartTextRenderer.endFrame();
    if (profile) m_frameProfiler.mark(FrameProfiler::TextEnd);
    if (m_chartTextRenderer.droppedGlyphs() > 0) {
        sLog_Probe("text.dropped",
                   "total=" << m_chartTextRenderer.droppedGlyphs()
                   << " high=" << m_chartTextRenderer.droppedHighGlyphs()
                   << " low=" << m_chartTextRenderer.droppedLowGlyphs());
    }

    if (profile) {
        const QString report = m_frameProfiler.endFrame();
        if (!report.isEmpty()) sLog_Render(report);
    }

    return texNode;
}
