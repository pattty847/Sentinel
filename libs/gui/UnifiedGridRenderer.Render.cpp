// UnifiedGridRenderer render-thread frame preparation and drawing.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "chart/shell/HeatmapChartPort.hpp"
#include "render/CandlePixelGeometry.hpp"
#include "render/VolumeProfileState.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"

#include <QQuickWindow>
#include <QSGOpacityNode>
#include <algorithm>
#include <cmath>

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

// ── GPU heatmap ──
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

// Liquidity labels: the layer's matched LabelCells, laid out by the reused
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

// Prepare and publish the frame on the render thread with the GUI blocked,
// before any item syncs (beforeSynchronizing): every
// sibling overlay that reads currentFrameContext() in its own updatePaintNode (candles,
// algo, paper trading) maps through the camera the heatmap draws in this frame, in
// whatever order Qt syncs the items. Qt notifications stay outside the producer.
void UnifiedGridRenderer::prepareSyncFrame() {
    m_frameProducer.frame().ready = false;
    if (width() <= 0 || height() <= 0 || !m_gpuLayer) return;
    const bool profile = FrameProfiler::enabled();
    if (profile) m_frameProfiler.beginFrame();
    const auto inputs = m_view->frameInputs(
        boundingRect(), window() ? window()->effectiveDevicePixelRatio() : 1.0,
        m_frameClock, m_timeAuthority, {m_heatmapLayerEnabled, m_footprintLayerEnabled, m_tpoLayerEnabled});
    if (profile) m_frameProfiler.mark(FrameProfiler::Context);
    const auto& frame = m_frameProducer.produce(inputs, *m_heatmapPort, m_published);
    if (frame.rasterChanged) {
        const auto& cam = frame.rest;
        const auto& in = inputs.camera.raster;
        const double priceSpan = in.maxPrice - in.minPrice;
        sLog_Probe("raster.step", "rowPx=" << cam.rowPx << " colPx=" << cam.colPx << " dpr=" << cam.dpr
                   << " dev=" << cam.widthDev << "x" << cam.heightDev << " tick=" << cam.tick
                   << " rRow=" << (priceSpan > 0 ? double(cam.heightDev) * cam.tick / priceSpan : 0.0)
                   << " rCol=" << double(cam.widthDev) * cam.tfMs / double(in.timeEnd - in.timeStart));
        // The axis models and sibling overlays follow the newly drawn surface.
        QMetaObject::invokeMethod(this, [this] {
            emit rasterChanged();
            emit viewportChanged();
        }, Qt::QueuedConnection);
    }
    if (profile) m_frameProfiler.mark(FrameProfiler::Mapping);
}

QSGNode* UnifiedGridRenderer::updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile) {
    heatmap::gpu::HeatmapTileNode* tile = nullptr;
    QSGNode* root = ensureGpuRootNode(oldNode, &tile);
    // The node draws the camera's window: rows and columns on device pixel edges.
    heatmap::gpu::HeatmapTileNode::Frame tileFrame;
    const auto& cam = frame.raster;
    const auto& sync = m_frameProducer.frame();
    std::optional<heatmap::gpu::ViewWindow> binView;
    if (sync.binView) binView = heatmap::gpu::ViewWindow{sync.binView->timeLo, sync.binView->timeHi,
                                                      sync.binView->priceLo, sync.binView->priceHi};
    const bool prepared =
        sync.drawHeatmap && cam.valid &&
        m_gpuLayer->prepareFrame(tileFrame, frame.surfaceBounds,
                                 {cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice, cam.drawnMaxPrice},
                                 sync.coverageGamma, binView);
    auto* gate = static_cast<QSGOpacityNode*>(root->firstChild());
    const double opacity = prepared ? 1.0 : 0.0; // 0 blocks the subtree: no prepare(), no draw
    if (gate->opacity() != opacity) gate->setOpacity(opacity);
    if (prepared) tile->setFrame(std::move(tileFrame));
    m_frameAck.submitted(frame);

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
        m_frameProducer.frame().ready = false;
        return oldNode;
    }
    // This sync's frame from beforeSynchronizing; built here when the window did not
    // announce the sync (an item just added to a window).
    if (!m_frameProducer.frame().ready) prepareSyncFrame();
    m_frameProducer.frame().ready = false;
    FrameContext& frame = m_frameProducer.frame().context;
    m_frameAck.stamp(frame);
    return updateGpuPaintNode(oldNode, frame, FrameProfiler::enabled()); // the one root path
}
