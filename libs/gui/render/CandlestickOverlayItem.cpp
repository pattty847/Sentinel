/*
Sentinel — CandlestickOverlayItem
GPU-batched candlestick overlay (demo data only).
*/
#include "CandlestickOverlayItem.hpp"
#include "GridViewState.hpp"
#include "../datasources/CandleSeriesBuffer.hpp"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QVector3D>
#include <QMatrix4x4>
#include <algorithm>
#include <cmath>


namespace {

class CandleOverlayNode final : public QSGNode {
public:
    CandleOverlayNode() {
        wickNode = new QSGGeometryNode();
        bodyNode = new QSGGeometryNode();

        wickGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        bodyGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        wickGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
        bodyGeometry->setDrawingMode(QSGGeometry::DrawTriangles);

        wickMaterial = new QSGVertexColorMaterial();
        bodyMaterial = new QSGVertexColorMaterial();

        wickNode->setGeometry(wickGeometry);
        wickNode->setMaterial(wickMaterial);
        bodyNode->setGeometry(bodyGeometry);
        bodyNode->setMaterial(bodyMaterial);

        wickNode->setFlag(QSGNode::OwnsGeometry, true);
        wickNode->setFlag(QSGNode::OwnsMaterial, true);
        bodyNode->setFlag(QSGNode::OwnsGeometry, true);
        bodyNode->setFlag(QSGNode::OwnsMaterial, true);

        appendChildNode(wickNode);
        appendChildNode(bodyNode);
    }

    QSGGeometryNode* wickNode = nullptr;
    QSGGeometryNode* bodyNode = nullptr;
    QSGGeometry* wickGeometry = nullptr;
    QSGGeometry* bodyGeometry = nullptr;
    QSGVertexColorMaterial* wickMaterial = nullptr;
    QSGVertexColorMaterial* bodyMaterial = nullptr;
};

inline void addQuad(QSGGeometry::ColoredPoint2D*& v,
                    float x0, float y0, float x1, float y1,
                    uchar r, uchar g, uchar b, uchar a) {
    v[0].set(x0, y0, r, g, b, a);
    v[1].set(x0, y1, r, g, b, a);
    v[2].set(x1, y0, r, g, b, a);
    v[3].set(x1, y0, r, g, b, a);
    v[4].set(x0, y1, r, g, b, a);
    v[5].set(x1, y1, r, g, b, a);
    v += 6;
}

inline QPointF mapWorld(const QMatrix4x4& m, qint64 timeMs, double price) {
    QVector3D p = m.map(QVector3D(static_cast<float>(timeMs),
                                  static_cast<float>(price),
                                  0.0f));
    return QPointF(p.x(), p.y());
}

} // namespace

CandlestickOverlayItem::CandlestickOverlayItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

QObject* CandlestickOverlayItem::viewState() const {
    return static_cast<QObject*>(m_viewState.data());
}

QObject* CandlestickOverlayItem::candleBuffer() const {
    return static_cast<QObject*>(m_candleBuffer.data());
}

void CandlestickOverlayItem::setViewState(QObject* viewState) {
    if (m_viewState == viewState) {
        return;
    }
    disconnectViewStateSignals();
    m_viewState = qobject_cast<GridViewState*>(viewState);
    connectViewStateSignals();
    markGeometryDirty();
    emit viewStateChanged();
}

void CandlestickOverlayItem::setCandleBuffer(QObject* buffer) {
    if (m_candleBuffer == buffer) {
        return;
    }
    disconnectCandleSignals();
    m_candleBuffer = qobject_cast<CandleSeriesBuffer*>(buffer);
    connectCandleSignals();
    markGeometryDirty();
    emit candleBufferChanged();
}

void CandlestickOverlayItem::setSymbol(const QString& symbol) {
    if (m_symbol == symbol) {
        return;
    }
    m_symbol = symbol;
    markGeometryDirty();
    emit symbolChanged();
}

void CandlestickOverlayItem::setTimeframeSec(int sec) {
    if (m_timeframeSec == sec || sec <= 0) {
        return;
    }
    m_timeframeSec = sec;
    markGeometryDirty();
    emit timeframeSecChanged();
}

void CandlestickOverlayItem::connectViewStateSignals() {
    if (!m_viewState) {
        return;
    }
    m_viewportChangedConn = connect(m_viewState, &GridViewState::viewportChanged, this, [this]() {
        markGeometryDirty();
    });
    m_panChangedConn = connect(m_viewState, &GridViewState::panVisualOffsetChanged, this, [this]() {
        markGeometryDirty();
    });
}

void CandlestickOverlayItem::disconnectViewStateSignals() {
    if (m_viewportChangedConn) {
        disconnect(m_viewportChangedConn);
    }
    if (m_panChangedConn) {
        disconnect(m_panChangedConn);
    }
}

void CandlestickOverlayItem::connectCandleSignals() {
    if (!m_candleBuffer) {
        return;
    }
    m_candleDirtyConn = connect(m_candleBuffer, &CandleSeriesBuffer::candlesDirty,
                                this,
                                [this](const QString& symbol,
                                       int64_t timeframeSec,
                                       qint64 dirtyStartMs,
                                       qint64 dirtyEndMs) {
                                    if (symbol != m_symbol || timeframeSec != m_timeframeSec) {
                                        return;
                                    }
                                    if (!m_viewState || !m_viewState->isTimeWindowValid()) {
                                        return;
                                    }
                                    const qint64 viewStart = m_viewState->getVisibleTimeStart();
                                    const qint64 viewEnd = m_viewState->getVisibleTimeEnd();
                                    if (dirtyEndMs < viewStart || dirtyStartMs > viewEnd) {
                                        return;
                                    }
                                    markGeometryDirty();
                                });
}

void CandlestickOverlayItem::disconnectCandleSignals() {
    if (m_candleDirtyConn) {
        disconnect(m_candleDirtyConn);
    }
}

void CandlestickOverlayItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        markGeometryDirty();
    }
}

void CandlestickOverlayItem::markGeometryDirty() {
    m_geometryDirty = true;
    update();
}

void CandlestickOverlayItem::ensureDemoCandles() {
    if (!m_viewState || !m_viewState->isTimeWindowValid()) {
        return;
    }

    const qint64 timeStart = m_viewState->getVisibleTimeStart();
    const qint64 timeEnd = m_viewState->getVisibleTimeEnd();
    if (timeEnd <= timeStart) {
        return;
    }

    if (m_demoStartMs == timeStart && m_demoEndMs == timeEnd && !m_demoCandles.empty()) {
        return;
    }

    m_demoCandles.clear();
    m_demoStartMs = timeStart;
    m_demoEndMs = timeEnd;

    const int targetCount = 200;
    const qint64 span = std::max<qint64>(1, timeEnd - timeStart);
    const qint64 step = std::max<qint64>(1, span / targetCount);
    m_demoStepMs = step;

    const double minPrice = m_viewState->getMinPrice();
    const double maxPrice = m_viewState->getMaxPrice();
    const double mid = (minPrice + maxPrice) * 0.5;
    const double amp = std::max(1e-6, (maxPrice - minPrice) * 0.2);

    m_demoCandles.reserve(targetCount);
    for (int i = 0; i < targetCount; ++i) {
        const qint64 t = timeStart + static_cast<qint64>(i) * step;
        const double wave = std::sin(static_cast<double>(i) * 0.35);
        const double open = mid + wave * amp * 0.6;
        const double close = mid + std::cos(static_cast<double>(i) * 0.35) * amp * 0.6;
        const double high = std::max(open, close) + amp * 0.2;
        const double low = std::min(open, close) - amp * 0.2;
        m_demoCandles.push_back({t, t + step, open, high, low, close});
    }
}

QSGNode* CandlestickOverlayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<CandleOverlayNode*>(oldNode);
    if (!root) {
        root = new CandleOverlayNode();
    }

    if (!m_viewState || !m_viewState->isTimeWindowValid()) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const QSizeF currentSize(width(), height());
    const uint64_t viewportVersion = m_viewState->getViewportVersion();
    if (!m_geometryDirty && m_lastViewportVersion == viewportVersion && m_lastSize == currentSize) {
        return root;
    }

    m_lastViewportVersion = viewportVersion;
    m_lastSize = currentSize;
    m_geometryDirty = false;

    const qint64 timeStart = m_viewState->getVisibleTimeStart();
    const qint64 timeEnd = m_viewState->getVisibleTimeEnd();
    if (timeEnd <= timeStart) {
        return root;
    }

    bool hasData = false;
    m_visibleCandles.clear();
    if (m_candleBuffer && !m_symbol.isEmpty() && m_timeframeSec > 0) {
        std::vector<CandleSeriesBuffer::CandleBar> bufferSlice;
        hasData = m_candleBuffer->getVisibleSlice(m_symbol,
                                                  m_timeframeSec,
                                                  timeStart,
                                                  timeEnd,
                                                  bufferSlice);
        if (hasData) {
            m_visibleCandles.reserve(bufferSlice.size());
            for (const auto& bar : bufferSlice) {
                CandleOverlayBar out;
                out.timeStartMs = bar.timeStartMs;
                out.timeEndMs = bar.timeEndMs;
                out.open = bar.open;
                out.high = bar.high;
                out.low = bar.low;
                out.close = bar.close;
                m_visibleCandles.push_back(out);
            }
        }
    }
    if (!hasData) {
        ensureDemoCandles();
        m_visibleCandles = m_demoCandles;
    }

    auto startIt = std::lower_bound(m_visibleCandles.begin(), m_visibleCandles.end(), timeStart,
                                    [](const CandleOverlayBar& c, qint64 t) { return c.timeStartMs < t; });
    auto endIt = std::upper_bound(m_visibleCandles.begin(), m_visibleCandles.end(), timeEnd,
                                  [](qint64 t, const CandleOverlayBar& c) { return t < c.timeStartMs; });
    const int visibleCount = static_cast<int>(std::distance(startIt, endIt));

    if (visibleCount <= 0) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    root->wickGeometry->allocate(visibleCount * 6);
    root->bodyGeometry->allocate(visibleCount * 6);
    auto* wickVerts = root->wickGeometry->vertexDataAsColoredPoint2D();
    auto* bodyVerts = root->bodyGeometry->vertexDataAsColoredPoint2D();

    const QMatrix4x4 xform = m_viewState->calculateViewportTransform(boundingRect());

    const uchar wickR = 240;
    const uchar wickG = 240;
    const uchar wickB = 240;
    const uchar wickA = 200;
    const uchar bullR = 60;
    const uchar bullG = 210;
    const uchar bullB = 110;
    const uchar bullA = 220;
    const uchar bearR = 230;
    const uchar bearG = 80;
    const uchar bearB = 80;
    const uchar bearA = 220;

    for (auto it = startIt; it != endIt; ++it) {
        const auto& c = *it;
        const bool bullish = c.close >= c.open;

        const qint64 stepMs = (c.timeEndMs > c.timeStartMs)
            ? (c.timeEndMs - c.timeStartMs)
            : ((m_timeframeSec > 0) ? static_cast<qint64>(m_timeframeSec) * 1000 : 1000);
        const QPointF p0 = mapWorld(xform, c.timeStartMs, c.open);
        const QPointF p1 = mapWorld(xform, c.timeStartMs, c.close);
        const QPointF pHigh = mapWorld(xform, c.timeStartMs, c.high);
        const QPointF pLow = mapWorld(xform, c.timeStartMs, c.low);
        const QPointF pNext = mapWorld(xform, c.timeStartMs + stepMs, c.open);

        const float x0 = static_cast<float>(p0.x());
        const float x1 = static_cast<float>(pNext.x());
        float bodyWidth = std::max(1.0f, (x1 - x0) * 0.7f);
        const float centerX = (x0 + x1) * 0.5f;
        const float bodyX0 = centerX - bodyWidth * 0.5f;
        const float bodyX1 = centerX + bodyWidth * 0.5f;

        const float wickWidth = std::max(1.0f, bodyWidth * 0.2f);
        const float wickX0 = centerX - wickWidth * 0.5f;
        const float wickX1 = centerX + wickWidth * 0.5f;

        const float yHigh = static_cast<float>(pHigh.y());
        const float yLow = static_cast<float>(pLow.y());
        const float yOpen = static_cast<float>(p0.y());
        const float yClose = static_cast<float>(p1.y());
        const float bodyY0 = std::min(yOpen, yClose);
        const float bodyY1 = std::max(yOpen, yClose);

        addQuad(wickVerts, wickX0, yHigh, wickX1, yLow, wickR, wickG, wickB, wickA);
        if (bullish) {
            addQuad(bodyVerts, bodyX0, bodyY0, bodyX1, bodyY1, bullR, bullG, bullB, bullA);
        } else {
            addQuad(bodyVerts, bodyX0, bodyY0, bodyX1, bodyY1, bearR, bearG, bearB, bearA);
        }
    }

    root->wickNode->markDirty(QSGNode::DirtyGeometry);
    root->bodyNode->markDirty(QSGNode::DirtyGeometry);
    return root;
}
