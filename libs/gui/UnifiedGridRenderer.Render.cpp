// UnifiedGridRenderer render-thread frame preparation and drawing.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include "chart/shell/HeatmapChartPort.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"

#include <QQuickWindow>

// Prepare and publish the frame on the render thread with the GUI blocked,
// before any item syncs (beforeSynchronizing): every
// sibling overlay that reads currentFrameContext() in its own updatePaintNode (candles,
// algo, paper trading) maps through the camera the heatmap draws in this frame, in
// whatever order Qt syncs the items. Qt notifications stay outside the producer.
void UnifiedGridRenderer::prepareSyncFrame() {
    m_frameProducer.frame().ready = false;
    if (width() <= 0 || height() <= 0 || !m_layers.heatmap.gpu) return;
    const bool profile = FrameProfiler::enabled();
    if (profile) m_frameProfiler.beginFrame();
    const auto inputs = m_view->frameInputs(
        boundingRect(), window() ? window()->effectiveDevicePixelRatio() : 1.0,
        m_frameClock, m_timeAuthority, {m_visibility.heatmapEnabled(), m_visibility.footprintEnabled(), m_visibility.tpoEnabled(), m_visibility.volumeProfileEnabled()});
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
    auto* root = oldNode;
    if (!root) {
        root = new QSGNode();
        m_layers.registry.onRootRebuilt(root);
    }
    const auto& sync = m_frameProducer.frame();
    m_layers.registry.sync(sync);
    m_bubbles.publish(frame.mapping);
    m_layers.registry.updateNodes(sync, {window(), root, m_layers.text.renderer, m_layers.text.atlas,
                                         m_layers.text.atlasBuilt, profile ? &m_frameProfiler : nullptr});
    m_frameAck.submitted(frame);
    if (profile) {
        const QString report = m_frameProfiler.endFrame();
        if (!report.isEmpty()) sLog_Render(report);
    }
    return root;
}


QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    m_bubbles.disableFrame(); // Also covers invalid surfaces.
    if (width() <= 0 || height() <= 0 || !m_layers.heatmap.gpu) {
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
