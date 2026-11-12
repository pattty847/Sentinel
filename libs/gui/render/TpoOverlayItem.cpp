#include "TpoOverlayItem.hpp"
#include "CandlePixelGeometry.hpp"
#include "ITimeAxisMappingProvider.hpp"
#include "TpoOverlayRenderer.hpp"
#include "chart/feeds/CvdFeed.hpp"
#include "chart/shell/ChartTpoQml.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include <QQuickWindow>
#include <algorithm>

TpoOverlayItem::TpoOverlayItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

TpoOverlayItem::~TpoOverlayItem() { unbind(); }

void TpoOverlayItem::unbind() {
    for (const auto& connection : m_connections) disconnect(connection);
    m_connections.clear();
    if (m_renderer) m_renderer->setChangeListener({});
    m_mapping = nullptr;
    m_tpo = nullptr;
    m_layers = nullptr;
    m_renderer = nullptr;
    m_enabled = false;
}

void TpoOverlayItem::setChart(QObject* chart) {
    if (m_chart == chart && chart) return;
    unbind();
    m_chart = chart;
    if (chart) {
        m_mapping = qobject_cast<ITimeAxisMappingProvider*>(chart);
        m_tpo = qobject_cast<chart::ChartTpoQml*>(chart->property("tpo").value<QObject*>());
        m_layers = qobject_cast<chart::LayerVisibility*>(chart->property("layers").value<QObject*>());
        m_renderer = m_tpo ? m_tpo->overlayRenderer() : nullptr;
        if (m_renderer) m_renderer->setChangeListener([this] { update(); });
        m_connections << connect(chart, &QObject::destroyed, this, [this] { setChart(nullptr); });
        if (QObject* view = m_mapping ? m_mapping->viewNotifications() : nullptr) {
            m_connections << connect(view, SIGNAL(viewportChanged()), this, SLOT(update()));
            m_connections << connect(view, SIGNAL(panVisualOffsetChanged()), this, SLOT(update()));
            m_connections << connect(view, SIGNAL(timeframeChanged()), this, SLOT(update()));
        }
        if (m_layers) {
            m_connections << connect(m_layers, &chart::LayerVisibility::layerVisibilityChanged, this,
                                     [this] { readLayers(); });
        }
    }
    readLayers();
    emit chartChanged();
}

void TpoOverlayItem::readLayers() {
    m_enabled = m_layers && m_layers->tpoEnabled();
    update();
}

QSGNode* TpoOverlayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    if (!m_renderer || !m_mapping || !m_tpo || !window()) return root;
    if (!oldNode) m_renderer->onRootRebuilt();  // a new root: the old nodes went with the old one
    const MappingFrameContext frame = m_mapping->currentFrameContext();
    QRectF bounds = frame.surfaceBounds;
    const auto* presentation = m_mapping->cvdPresentation();
    if (presentation && frame.raster.valid) {
        const double inset = presentation->bottomInset(frame.activeTimeframeMs, bounds.height());
        bounds.setHeight(std::max(0.0, bounds.height() - inset));
    }
    const auto snap = candle_pixels::axisSnap(frame.raster);
    const bool atlasReady = m_tpo->overlayAtlasReady();
    m_renderer->render(window(), root, m_enabled && frame.viewportValid, *m_tpo->overlayAtlas(), atlasReady,
                       frame.raster, frame.surfaceDpr, snap.x, snap.y, bounds);
    if (m_enabled && !atlasReady && !m_retryQueued.exchange(true)) {
        // The chart builds its text atlas during its own sync; draw letters once it exists.
        QMetaObject::invokeMethod(this, [this] { m_retryQueued.store(false); update(); }, Qt::QueuedConnection);
    }
    return root;
}
