// UnifiedGridRenderer render-thread hot path split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "render/FrameContextBuilder.hpp"
#include "render/CandlePixelGeometry.hpp"
#include "render/VolumeProfileState.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"

#include <QQuickWindow>
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
    published.raster = frame.raster;
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

    const auto snap = candle_pixels::axisSnap(frame.raster);
    m_vpRenderer.render(parent,
                        m_volumeProfileLayerEnabled && !localBins.empty(),
                        frame.surfaceBounds,
                        frame.mapping.viewMinPrice,
                        frame.mapping.viewMaxPrice,
                        localBins,
                        localSnap, frame.surfaceDpr, snap.y);

    // TPO uses the same drawn camera, including fractional glide time.
    m_tpoOverlay.render(window(),
                        parent,
                        drawTpo,
                        m_chartTextAtlas,
                        m_chartTextAtlasBuilt,
                        frame.raster, frame.surfaceDpr, snap.x, snap.y, frame.surfaceBounds);
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

chart_raster::RasterInputs UnifiedGridRenderer::rasterInputs(const FrameViewportSnapshot& viewport, double tick,
                                                             double width, double height, double dpr) const {
    chart_raster::RasterInputs in;
    if (!viewport.valid) return in;
    in.timeStart = viewport.timeStart;
    in.timeEnd = viewport.timeEnd;
    in.minPrice = viewport.minPrice;
    in.maxPrice = viewport.maxPrice;
    in.dragLogicalPx = viewport.dragging ? viewport.panVisualOffset : QPointF();
    in.tfMs = static_cast<double>(m_currentTimeframe_ms);
    in.tick = tick;
    in.itemWidthLogical = width;
    in.itemHeightLogical = height;
    in.dpr = dpr;
    in.anchorFracX = viewport.anchorFracX;
    in.anchorFracY = viewport.anchorFracY;
    return in;
}

// Whole-pixel mapping (docs/research/2026-10-08-whole-pixel-mapping.md): one raster
// camera per frame at the layer's tick; every layer maps through toMapping(camera).
// Plain arithmetic on the render thread (GUI blocked): no allocation.
void UnifiedGridRenderer::computeGpuFrameMapping(FrameContext& frame) {
    const double tick = m_gpuLayer->tickPrice();
    const auto in = rasterInputs(frame.viewport, tick, width(), height(), frame.surfaceDpr);
    chart_raster::RasterStep previous;
    {
        std::lock_guard<std::mutex> lock(m_frameContextMutex);
        previous = m_rasterStep;
    }
    // The rest camera (whole pixels), and what this frame draws: the rest camera, or a
    // free one during a zoom glide or gesture (fractional, drawn with coverage).
    const Cameras cameras = camerasFor(in, previous);
    frame.raster = cameras.drawn;
    frame.mapping = chart_raster::toMapping(frame.raster);
    m_sync.coverageGamma = cameras.drawn.valid && cameras.drawn.free ? 2.2f : 0.0f;
    m_sync.hasBinView = cameras.hasBinView;
    m_sync.binTimeLo = cameras.binTimeLo;
    m_sync.binTimeHi = cameras.binTimeHi;
    m_sync.binPriceLo = cameras.binPriceLo;
    m_sync.binPriceHi = cameras.binPriceHi;
    const auto& cam = cameras.rest;
    if (!cam.valid) return;
    if (cam.step() != previous) {
        std::lock_guard<std::mutex> lock(m_frameContextMutex);
        m_rasterStep = cam.step();
    }
    const double priceSpan = in.maxPrice - in.minPrice;
    m_gpuLayer->noteRaster(cam.rowPx, cam.colPx, priceSpan > 0 ? double(cam.heightDev) * cam.tick / priceSpan : 0.0);
    const RasterSurface surface{cam.step(), cam.dpr, cam.tick, cam.widthDev, cam.heightDev};
    if (surface != m_rasterAnnounced) {
        sLog_Probe("raster.step", "rowPx=" << cam.rowPx << " colPx=" << cam.colPx << " dpr=" << cam.dpr
                   << " dev=" << cam.widthDev << "x" << cam.heightDev << " tick=" << cam.tick
                   << " rRow=" << (priceSpan > 0 ? double(cam.heightDev) * cam.tick / priceSpan : 0.0)
                   << " rCol=" << double(cam.widthDev) * cam.tfMs / double(in.timeEnd - in.timeStart));
        m_rasterAnnounced = surface;
        // The drawn window moved without a viewport change (a tick, step, DPR or size
        // change): the axis models and the sibling overlays that redraw on
        // viewportChanged (candles, algo, paper trading) follow in the next frame.
        QMetaObject::invokeMethod(this, [this] {
            emit rasterChanged();
            emit viewportChanged();
        }, Qt::QueuedConnection);
    }
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

// Steps 1-3 of the frame (whole-pixel plan section 1.9) and the publication, on the
// render thread with the GUI blocked, before any item syncs (beforeSynchronizing): every
// sibling overlay that reads currentFrameContext() in its own updatePaintNode (candles,
// algo, paper trading) maps through the camera the heatmap draws in this frame, in
// whatever order Qt syncs the items. Plain data and arithmetic: no allocation.
void UnifiedGridRenderer::prepareSyncFrame() {
    m_sync.ready = false;
    if (width() <= 0 || height() <= 0 || !m_gpuLayer) return;
    const bool profile = FrameProfiler::enabled();
    if (profile) m_frameProfiler.beginFrame();
    FrameContext& frame = m_sync.frame;
    frame = FrameContextBuilder::build(
        boundingRect(), window(), m_frameClock, m_timeAuthority, m_viewState.get(),
        m_heatmapLayerEnabled, m_footprintLayerEnabled, m_tpoLayerEnabled,
        m_footprintStreamGeneration.load(std::memory_order_acquire),
        m_candleStreamGeneration.load(std::memory_order_acquire));
    if (profile) m_frameProfiler.mark(FrameProfiler::Context);
    // 1-2. The tick for the continuous view (committed + an active drag at the
    // continuous scale): Auto decides on the continuous camera.
    const auto& vp = frame.viewport;
    const double timeSpan = double(vp.timeEnd - vp.timeStart), priceSpan = vp.maxPrice - vp.minPrice;
    const QRectF& bounds = frame.surfaceBounds;
    const bool viewValid = vp.valid && m_currentTimeframe_ms > 0 && timeSpan > 0 && priceSpan > 0 && !bounds.isEmpty();
    m_sync.drawHeatmap = frame.overlays.heatmap && viewValid;
    if (m_sync.drawHeatmap) {
        const QPointF drag = vp.dragging ? vp.panVisualOffset : QPointF();
        const double dt = -drag.x() * timeSpan / bounds.width(), dp = drag.y() * priceSpan / bounds.height();
        m_gpuLayer->chooseTickForView({double(vp.timeStart) + dt, double(vp.timeEnd) + dt, vp.minPrice + dp,
                                       vp.maxPrice + dp});
    }
    // 3. The raster camera at that tick (rows follow the drawn tick: heatmapTickSize,
    // PriceAxisModel), the frame's mapping, published.
    computeGpuFrameMapping(frame);
    publishFrameContext(frame);
    if (profile) m_frameProfiler.mark(FrameProfiler::Mapping);
    m_sync.ready = true;
}

QSGNode* UnifiedGridRenderer::updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile) {
    heatmap::gpu::HeatmapTileNode* tile = nullptr;
    QSGNode* root = ensureGpuRootNode(oldNode, &tile);
    // 4. The node draws the camera's window: rows and columns on device pixel edges.
    heatmap::gpu::HeatmapTileNode::Frame tileFrame;
    const auto& cam = frame.raster;
    std::optional<heatmap::gpu::ViewWindow> binView;
    if (m_sync.hasBinView) binView = heatmap::gpu::ViewWindow{m_sync.binTimeLo, m_sync.binTimeHi, m_sync.binPriceLo,
                                                              m_sync.binPriceHi};
    const bool prepared =
        m_sync.drawHeatmap && cam.valid &&
        m_gpuLayer->prepareFrame(tileFrame, frame.surfaceBounds,
                                 {cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice, cam.drawnMaxPrice},
                                 m_sync.coverageGamma, binView);
    auto* gate = static_cast<QSGOpacityNode*>(root->firstChild());
    const double opacity = prepared ? 1.0 : 0.0; // 0 blocks the subtree: no prepare(), no draw
    if (gate->opacity() != opacity) gate->setOpacity(opacity);
    if (prepared) tile->setFrame(std::move(tileFrame));
    m_pendingFrameRevision = frame.controlRevision;
    m_pendingFrameId = frame.frameId;

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
        m_sync.ready = false;
        return oldNode;
    }
    // This sync's frame from beforeSynchronizing; built here when the window did not
    // announce the sync (an item just added to a window).
    if (!m_sync.ready) prepareSyncFrame();
    m_sync.ready = false;
    FrameContext& frame = m_sync.frame;
    frame.controlRevision = m_controlRevision.load(std::memory_order_acquire);
    frame.selectionEpoch = m_controlSelectionEpoch.load(std::memory_order_acquire);
    frame.viewportVersion = m_controlViewportVersion.load(std::memory_order_acquire);
    frame.frameId = ++m_nextFrameId;
    return updateGpuPaintNode(oldNode, frame, FrameProfiler::enabled()); // the one root path (S8a)
}
