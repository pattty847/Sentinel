#include "AlgoOverlayRenderer.hpp"
#include "ITimeAxisMappingProvider.hpp"
#include "TimeAxisMapping.hpp"

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QSGNode>
#include <cmath>

AlgoOverlayRenderer::AlgoOverlayRenderer(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void AlgoOverlayRenderer::setMappingProvider(QObject* provider) {
    if (m_mappingProvider == provider) return;
    m_mappingProvider = provider;
    m_dirty = true;
    update();
    emit mappingProviderChanged();
}

void AlgoOverlayRenderer::setEnabled(bool v) {
    if (m_enabled == v) return;
    m_enabled = v;
    m_dirty = true;
    update();
    emit enabledChanged();
}

void AlgoOverlayRenderer::onAlgoOrderEvent(const trading::AlgoOrderEvent& event) {
    QMutexLocker lock(&m_pendingMutex);
    m_pending.push_back({event});
    m_dirty = true;
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void AlgoOverlayRenderer::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        m_dirty = true;
        update();
    }
}

QSGNode* AlgoOverlayRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    if (!m_enabled) {
        delete oldNode;
        return nullptr;
    }

    // Drain pending events from GUI thread
    {
        QMutexLocker lock(&m_pendingMutex);
        for (auto& p : m_pending) {
            m_events.push_back(std::move(p.event));
        }
        m_pending.clear();
    }
    while (static_cast<int>(m_events.size()) > kMaxEvents) {
        m_events.pop_front();
    }

    if (!m_dirty && oldNode) {
        return oldNode;
    }
    m_dirty = false;

    // Get coordinate mapping
    auto* mappingObj = m_mappingProvider.data();
    auto* provider = mappingObj ? qobject_cast<ITimeAxisMappingProvider*>(mappingObj) : nullptr;
    if (!provider) {
        delete oldNode;
        return nullptr;
    }
    const TimeAxisMapping mapping = provider->currentTimeAxisMapping();
    if (!mapping.valid) {
        delete oldNode;
        return nullptr;
    }

    const float W = static_cast<float>(width());
    const float H = static_cast<float>(height());
    if (W <= 0 || H <= 0) {
        delete oldNode;
        return nullptr;
    }

    struct Vertex { float x, y; };

    struct ColoredVerts {
        QColor color;
        std::vector<Vertex> verts;
        QSGGeometry::DrawingMode mode;
    };

    const QColor bidColor(0, 200, 255, 200);    // cyan for buy
    const QColor askColor(255, 140, 0, 200);    // orange for sell
    const QColor fillColor(255, 255, 255, 230); // white for fill diamond
    const QColor cancelColor(120, 120, 120, 100);

    ColoredVerts bidLines{bidColor, {}, QSGGeometry::DrawLines};
    ColoredVerts askLines{askColor, {}, QSGGeometry::DrawLines};
    ColoredVerts cancelLines{cancelColor, {}, QSGGeometry::DrawLines};
    ColoredVerts fills{fillColor, {}, QSGGeometry::DrawTriangles};

    const float lineHalfWidthPx = std::max(2.0f, W * 0.003f);

    for (const auto& ev : m_events) {
        if (ev.price <= 0.0) continue;

        const float y = static_cast<float>(mapping.priceToScreenY(ev.price));
        if (y < -20.0f || y > H + 20.0f) continue;

        const float x = static_cast<float>(mapping.timeToScreenX(static_cast<double>(ev.timestampMs)));
        bool isBuy = (ev.side == trading::OrderSide::Buy);

        if (ev.status == trading::OrderStatus::Open) {
            ColoredVerts& target = isBuy ? bidLines : askLines;
            target.verts.push_back({x - lineHalfWidthPx * 8, y});
            target.verts.push_back({x + lineHalfWidthPx * 8, y});
        } else if (ev.status == trading::OrderStatus::Filled) {
            const float r = std::max(3.5f, H * 0.004f);
            fills.verts.push_back({x, y - r});
            fills.verts.push_back({x - r, y});
            fills.verts.push_back({x + r, y});
            fills.verts.push_back({x + r, y});
            fills.verts.push_back({x - r, y});
            fills.verts.push_back({x, y + r});
        } else if (ev.status == trading::OrderStatus::Canceled) {
            cancelLines.verts.push_back({x - lineHalfWidthPx * 6, y});
            cancelLines.verts.push_back({x + lineHalfWidthPx * 6, y});
        }
    }

    QSGNode* root = oldNode ? oldNode : new QSGNode();

    int childIdx = 0;
    auto addToTree = [&](const ColoredVerts& cv) {
        if (cv.verts.empty()) {
            ++childIdx;
            return;
        }

        QSGGeometryNode* node = nullptr;
        if (childIdx < static_cast<int>(root->childCount())) {
            node = static_cast<QSGGeometryNode*>(root->childAtIndex(childIdx));
        } else {
            node = new QSGGeometryNode();
            auto* mat = new QSGFlatColorMaterial();
            node->setMaterial(mat);
            node->setFlag(QSGNode::OwnsMaterial);
            node->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0));
            node->setFlag(QSGNode::OwnsGeometry);
            root->appendChildNode(node);
        }

        auto* mat = static_cast<QSGFlatColorMaterial*>(node->material());
        mat->setColor(cv.color);
        node->markDirty(QSGNode::DirtyMaterial);

        auto* geo = node->geometry();
        geo->setDrawingMode(cv.mode);
        geo->allocate(static_cast<int>(cv.verts.size()));
        auto* pts = geo->vertexDataAsPoint2D();
        for (int i = 0; i < static_cast<int>(cv.verts.size()); ++i) {
            pts[i].set(cv.verts[i].x, cv.verts[i].y);
        }
        node->markDirty(QSGNode::DirtyGeometry);
        ++childIdx;
    };

    addToTree(bidLines);
    addToTree(askLines);
    addToTree(cancelLines);
    addToTree(fills);

    return root;
}
