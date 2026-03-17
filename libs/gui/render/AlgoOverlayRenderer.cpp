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

    for (const auto& connection : m_mappingConnections) {
        QObject::disconnect(connection);
    }
    m_mappingConnections.clear();

    m_mappingProvider = provider;
    if (m_mappingProvider) {
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProvider, SIGNAL(viewportChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProvider, SIGNAL(timeframeChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProvider, SIGNAL(panVisualOffsetChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProvider, SIGNAL(liveRenderTick()), this, SLOT(onMappingChanged())));
    }
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
    m_pending.push_back(event);
    m_dirty = true;
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void AlgoOverlayRenderer::onMappingChanged() {
    m_dirty = true;
    update();
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

    // Drain pending events from GUI thread and maintain active order spans.
    {
        QMutexLocker lock(&m_pendingMutex);
        for (auto& ev : m_pending) {
            if (!ev.orderId.empty()) {
                auto& span = m_orderSpans[ev.orderId];
                if (ev.status == trading::OrderStatus::Open) {
                    span.side = ev.side;
                    span.price = ev.price;
                    span.openedMs = ev.timestampMs;
                    span.closedMs = 0;
                    span.terminalStatus = trading::OrderStatus::New;
                    span.active = true;
                } else if (ev.status == trading::OrderStatus::Filled ||
                           ev.status == trading::OrderStatus::Canceled ||
                           ev.status == trading::OrderStatus::Rejected) {
                    if (!span.active && span.openedMs == 0) {
                        span.side = ev.side;
                        span.price = ev.price;
                        span.openedMs = ev.timestampMs;
                    }
                    span.closedMs = ev.timestampMs;
                    span.terminalStatus = ev.status;
                    span.active = false;
                    m_markers.push_back(ev);
                }
            } else {
                m_markers.push_back(ev);
            }
        }
        m_pending.clear();
    }
    while (static_cast<int>(m_markers.size()) > kMaxEvents) {
        m_markers.pop_front();
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

    const QColor activeLineColor(255, 255, 255, 245);
    const QColor fillColor(255, 255, 255, 230); // white for fill diamond
    const QColor cancelColor(190, 190, 190, 180);

    ColoredVerts bidLines{activeLineColor, {}, QSGGeometry::DrawLines};
    ColoredVerts askLines{activeLineColor, {}, QSGGeometry::DrawLines};
    ColoredVerts cancelLines{cancelColor, {}, QSGGeometry::DrawLines};
    ColoredVerts fills{fillColor, {}, QSGGeometry::DrawTriangles};

    for (const auto& [orderId, span] : m_orderSpans) {
        Q_UNUSED(orderId);
        if (span.price <= 0.0 || span.openedMs <= 0) continue;

        const float y = static_cast<float>(mapping.priceToScreenY(span.price));
        if (y < -20.0f || y > H + 20.0f) continue;

        const double visibleStart = mapping.visibleDataStartMs();
        const double visibleEnd = mapping.visibleDataEndMs();
        if (visibleEnd <= visibleStart) continue;

        const double spanEndMs = span.active
            ? visibleEnd
            : (span.closedMs > 0 ? static_cast<double>(span.closedMs) : visibleEnd);
        if (spanEndMs < visibleStart || static_cast<double>(span.openedMs) > visibleEnd) {
            continue;
        }

        const double startMs = std::max(static_cast<double>(span.openedMs), visibleStart);
        const double endMs = std::min(spanEndMs, visibleEnd);
        float x0 = static_cast<float>(mapping.timeToScreenX(startMs));
        float x1 = static_cast<float>(mapping.timeToScreenX(endMs));
        if (span.active) {
            x1 = static_cast<float>(mapping.drawRect.right());
        }
        if (x1 < x0) std::swap(x0, x1);
        if (std::abs(x1 - x0) < 1.0f) {
            x1 = x0 + 1.0f;
        }

        ColoredVerts& target = (span.side == trading::OrderSide::Buy) ? bidLines : askLines;
        target.verts.push_back({x0, y});
        target.verts.push_back({x1, y});
    }

    for (const auto& ev : m_markers) {
        if (ev.price <= 0.0) continue;

        const float y = static_cast<float>(mapping.priceToScreenY(ev.price));
        if (y < -20.0f || y > H + 20.0f) continue;

        const float x = static_cast<float>(mapping.timeToScreenX(static_cast<double>(ev.timestampMs)));

        if (ev.status == trading::OrderStatus::Filled) {
            const float r = std::max(3.5f, H * 0.004f);
            fills.verts.push_back({x, y - r});
            fills.verts.push_back({x - r, y});
            fills.verts.push_back({x + r, y});
            fills.verts.push_back({x + r, y});
            fills.verts.push_back({x - r, y});
            fills.verts.push_back({x, y + r});
        } else if (ev.status == trading::OrderStatus::Canceled) {
            const float halfWidth = std::max(6.0f, W * 0.01f);
            cancelLines.verts.push_back({x - halfWidth, y});
            cancelLines.verts.push_back({x + halfWidth, y});
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
