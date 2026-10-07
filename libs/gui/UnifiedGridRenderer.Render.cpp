// UnifiedGridRenderer render-thread hot path split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "render/FrameContextBuilder.hpp"
#include "render/UgrFrameMath.hpp"
#include "render/VolumeProfileState.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"

#include <QSGOpacityNode>
#include <algorithm>
#include <cmath>

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
    published.footprintGeneration = frame.streamGenerations.footprint;
    published.candleGeneration = frame.streamGenerations.candle;
    published.mapping = frame.mapping;
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    m_lastFrameContext = published;
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

// ── GPU heatmap (S6b) ─────────────────────────────────────────────────────────
QSGNode* UnifiedGridRenderer::ensureGpuRootNode(QSGNode* oldNode, heatmap::gpu::HeatmapTileNode** tile) {
    auto* root = oldNode;
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
        m_gpuLabelSignature.store(0);
        // Held back by a transition: after the frame, check whether they can draw.
        m_gpuLabelsIncomplete.store(prepared && m_gpuLayer->labelsPending());
        return;
    }
    // Only the columns showing the picture the labels describe (drawn last frame
    // and this frame's target: the node prepares after this layout).
    if (m_gpuLabelColumns.capacity() < heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells)
        m_gpuLabelColumns.reserve(heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells);
    const size_t matched = m_gpuLayer->matchLabelColumns(*labels, m_gpuLabelColumns);
    const auto& settings = m_gpuLayer->settings();
    heatmap::gpu::LabelStyle style;
    style.usd = settings.labelCurrency != "asset";
    style.minPx = settings.labelMinPx;
    style.maxPx = settings.labelMaxPx;
    style.window = {m_gpuLayer->drawStyle().codeFloor, m_gpuLayer->drawStyle().codeRange};
    style.palette = m_gpuLayer->palette();
    m_gpuLabels.layout(labels, m_chartTextAtlas, frame.mapping, window()->effectiveDevicePixelRatio(), style,
                       m_heatmapLabelGlyphs, &m_gpuLabelColumns);
    const auto& st = m_gpuLabels.stats();
    m_gpuLabelSignature.store(heatmap::gpu::HeatmapGpuLayer::labelSignature(labels.get(), m_gpuLabelColumns, matched));
    m_gpuLabelsIncomplete.store(st.unmatched > 0);
    sLog_Probe("heatmap.labels.layout", "serial=" << labels->key.serial << " labels=" << st.labels
               << " glyphs=" << st.glyphs << " sizePx=" << st.sizePx << " narrow=" << st.tooNarrow
               << " dropped=" << st.droppedBudget << " unmatched=" << st.unmatched << " matchedColumns=" << matched);
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
    // Qt blocks the GUI thread here: read chart-owned tape/settings only,
    // never traverse the QObject graph from the node or its shader.
    publishTradeBubbleFrame(frame.mapping);
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

void UnifiedGridRenderer::publishTradeBubbleFrame(const TimeAxisMapping& mapping) {
    m_tradeBubbleFrame->mapping = mapping;
    m_tradeBubbleFrame->enabled = m_showTrades;
    m_tradeBubbleFrame->minNotional = m_tradeMinNotional;
    m_tradeBubbleFrame->buy = m_tradeBuyColor;
    m_tradeBubbleFrame->sell = m_tradeSellColor;
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    m_tradeBubbleFrame->enabled = false; // Also covers invalid surfaces.
    if (width() <= 0 || height() <= 0 || !m_gpuLayer) {
        return oldNode;
    }
    const bool profile = FrameProfiler::enabled();
    if (profile) m_frameProfiler.beginFrame();

    FrameContext frame = FrameContextBuilder::build(
        boundingRect(), window(), m_frameClock, m_timeAuthority, m_viewState.get(),
        m_heatmapLayerEnabled, m_footprintLayerEnabled, m_tpoLayerEnabled,
        m_footprintStreamGeneration.load(std::memory_order_acquire),
        m_candleStreamGeneration.load(std::memory_order_acquire));
    frame.controlRevision = m_controlRevision.load(std::memory_order_acquire);
    frame.selectionEpoch = m_controlSelectionEpoch.load(std::memory_order_acquire);
    frame.viewportVersion = m_controlViewportVersion.load(std::memory_order_acquire);
    frame.frameId = ++m_nextFrameId;
    if (profile) m_frameProfiler.mark(FrameProfiler::Context);
    return updateGpuPaintNode(oldNode, frame, profile); // the one root path (S8a)
}
