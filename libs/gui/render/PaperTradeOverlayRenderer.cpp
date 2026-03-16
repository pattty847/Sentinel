#include "PaperTradeOverlayRenderer.hpp"

#include "PaperTradeOverlayModel.hpp"
#include "TimeAxisMapping.hpp"

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QSGNode>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
struct Vertex {
    float x;
    float y;
};

struct ColoredVerts {
    QColor color;
    std::vector<Vertex> verts;
    QSGGeometry::DrawingMode mode;
};

void addQuad(std::vector<Vertex>& verts, float x0, float y0, float x1, float y1) {
    verts.push_back({x0, y0});
    verts.push_back({x0, y1});
    verts.push_back({x1, y0});
    verts.push_back({x1, y0});
    verts.push_back({x0, y1});
    verts.push_back({x1, y1});
}

void addDashedHorizontal(std::vector<Vertex>& verts,
                         float width,
                         float y,
                         float dashLength,
                         float gapLength) {
    if (width <= 0.0f) {
        return;
    }
    for (float x = 0.0f; x < width; x += dashLength + gapLength) {
        const float x1 = std::min(width, x + dashLength);
        verts.push_back({x, y});
        verts.push_back({x1, y});
    }
}
}

PaperTradeOverlayRenderer::PaperTradeOverlayRenderer(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

double PaperTradeOverlayRenderer::screenYForPrice(double price) const {
    if (!m_mappingProvider || price <= 0.0) {
        return 0.0;
    }
    const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
    if (!mapping.valid) {
        return 0.0;
    }
    return mapping.priceToScreenY(price);
}

void PaperTradeOverlayRenderer::setMappingProvider(QObject* provider) {
    if (m_mappingProviderObject == provider) {
        return;
    }

    for (const auto& connection : m_mappingConnections) {
        QObject::disconnect(connection);
    }
    m_mappingConnections.clear();

    m_mappingProviderObject = provider;
    m_mappingProvider = qobject_cast<ITimeAxisMappingProvider*>(provider);
    if (m_mappingProviderObject) {
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProviderObject.data(), SIGNAL(viewportChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProviderObject.data(), SIGNAL(timeframeChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProviderObject.data(), SIGNAL(panVisualOffsetChanged()), this, SLOT(onMappingChanged())));
        m_mappingConnections.push_back(QObject::connect(
            m_mappingProviderObject.data(), SIGNAL(liveRenderTick()), this, SLOT(onMappingChanged())));
    }
    ++m_mappingRevision;
    m_dirty = true;
    update();
    emit mappingProviderChanged();
    emit mappingRevisionChanged();
}

void PaperTradeOverlayRenderer::setOverlayModel(QObject* model) {
    if (m_overlayModelObject == model) {
        return;
    }

    for (const auto& connection : m_modelConnections) {
        QObject::disconnect(connection);
    }
    m_modelConnections.clear();

    m_overlayModelObject = model;
    m_overlayModel = qobject_cast<PaperTradeOverlayModel*>(model);
    if (m_overlayModel) {
        m_modelConnections.push_back(QObject::connect(
            m_overlayModel, &PaperTradeOverlayModel::openOrdersChanged,
            this, &PaperTradeOverlayRenderer::syncFromModel, Qt::QueuedConnection));
        m_modelConnections.push_back(QObject::connect(
            m_overlayModel, &PaperTradeOverlayModel::activePositionChanged,
            this, &PaperTradeOverlayRenderer::syncFromModel, Qt::QueuedConnection));
        m_modelConnections.push_back(QObject::connect(
            m_overlayModel, &PaperTradeOverlayModel::symbolChanged,
            this, &PaperTradeOverlayRenderer::syncFromModel, Qt::QueuedConnection));
        syncFromModel();
    } else {
        QMutexLocker lock(&m_snapshotMutex);
        m_orders.clear();
        m_position = PositionSnapshot{};
    }
    m_dirty = true;
    update();
    emit overlayModelChanged();
}

void PaperTradeOverlayRenderer::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    m_dirty = true;
    update();
    emit enabledChanged();
}

void PaperTradeOverlayRenderer::onMappingChanged() {
    ++m_mappingRevision;
    m_dirty = true;
    update();
    emit mappingRevisionChanged();
}

void PaperTradeOverlayRenderer::syncFromModel() {
    if (!m_overlayModel) {
        return;
    }

    std::vector<OrderSnapshot> orders;
    const QVariantList openOrders = m_overlayModel->openOrders();
    orders.reserve(static_cast<size_t>(openOrders.size()));
    for (const QVariant& itemVar : openOrders) {
        const QVariantMap item = itemVar.toMap();
        const double price = item.value(QStringLiteral("price")).toDouble();
        if (price <= 0.0) {
            continue;
        }
        OrderSnapshot order;
        order.price = price;
        order.isBuy = item.value(QStringLiteral("side")).toString() == QStringLiteral("BUY");
        orders.push_back(order);
    }

    PositionSnapshot position;
    const QVariantMap activePosition = m_overlayModel->activePosition();
    if (!activePosition.isEmpty()) {
        position.active = true;
        position.isLong = activePosition.value(QStringLiteral("side")).toString() == QStringLiteral("LONG");
        position.entryPrice = activePosition.value(QStringLiteral("entryPrice")).toDouble();
        position.markPrice = activePosition.value(QStringLiteral("markPrice")).toDouble();
        position.totalPnl = activePosition.value(QStringLiteral("totalPnl")).toDouble();
    }

    {
        QMutexLocker lock(&m_snapshotMutex);
        m_orders = std::move(orders);
        m_position = position;
    }

    m_dirty = true;
    update();
}

void PaperTradeOverlayRenderer::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        ++m_mappingRevision;
        m_dirty = true;
        update();
        emit mappingRevisionChanged();
    }
}

QSGNode* PaperTradeOverlayRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    if (!m_enabled) {
        delete oldNode;
        return nullptr;
    }

    auto* provider = m_mappingProvider;
    if (!provider) {
        delete oldNode;
        return nullptr;
    }

    const TimeAxisMapping mapping = provider->currentTimeAxisMapping();
    if (!mapping.valid || width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }

    std::vector<OrderSnapshot> orders;
    PositionSnapshot position;
    {
        QMutexLocker lock(&m_snapshotMutex);
        orders = m_orders;
        position = m_position;
    }

    if (!m_dirty && oldNode) {
        return oldNode;
    }
    m_dirty = false;

    const float widthPx = static_cast<float>(width());
    const float heightPx = static_cast<float>(height());

    ColoredVerts bidLines{QColor(79, 140, 255, 242), {}, QSGGeometry::DrawLines};
    ColoredVerts askLines{QColor(245, 155, 90, 242), {}, QSGGeometry::DrawLines};
    ColoredVerts entryLine{position.isLong ? QColor(79, 140, 255, 255) : QColor(242, 109, 109, 255), {}, QSGGeometry::DrawLines};
    ColoredVerts markLine{position.totalPnl >= 0.0 ? QColor(91, 227, 155, 245) : QColor(255, 140, 130, 245), {}, QSGGeometry::DrawLines};
    ColoredVerts pnlBand{position.totalPnl >= 0.0 ? QColor(24, 55, 43, 150) : QColor(59, 36, 24, 150), {}, QSGGeometry::DrawTriangles};

    for (const auto& order : orders) {
        const float y = static_cast<float>(mapping.priceToScreenY(order.price));
        if (y < -20.0f || y > heightPx + 20.0f) {
            continue;
        }
        if (order.isBuy) {
            addDashedHorizontal(bidLines.verts, widthPx, y, 8.0f, 6.0f);
        } else {
            addDashedHorizontal(askLines.verts, widthPx, y, 8.0f, 6.0f);
        }
    }

    if (position.active && position.entryPrice > 0.0 && position.markPrice > 0.0) {
        const float entryY = static_cast<float>(mapping.priceToScreenY(position.entryPrice));
        const float markY = static_cast<float>(mapping.priceToScreenY(position.markPrice));
        const float topY = std::clamp(std::min(entryY, markY), 0.0f, heightPx);
        const float bottomY = std::clamp(std::max(entryY, markY), 0.0f, heightPx);
        if (bottomY > topY + 0.5f) {
            addQuad(pnlBand.verts, 0.0f, topY, widthPx, bottomY);
        }

        entryLine.verts.push_back({0.0f, entryY});
        entryLine.verts.push_back({widthPx, entryY});
        addDashedHorizontal(markLine.verts, widthPx, markY, 8.0f, 6.0f);
    }

    QSGNode* root = oldNode ? oldNode : new QSGNode();
    int childIdx = 0;
    auto addToTree = [&](const ColoredVerts& cv) {
        if (cv.verts.empty()) {
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

    addToTree(pnlBand);
    addToTree(bidLines);
    addToTree(askLines);
    addToTree(entryLine);
    addToTree(markLine);

    while (root->childCount() > childIdx) {
        delete root->childAtIndex(root->childCount() - 1);
    }

    return root;
}
