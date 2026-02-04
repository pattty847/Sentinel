/*
Sentinel — CandlestickOverlayItem
GPU-batched candlestick overlay (demo data only).
*/
#include "CandlestickOverlayItem.hpp"
#include "GridViewState.hpp"
#include "../datasources/CandleSeriesBuffer.hpp"
#include "../UnifiedGridRenderer.h"
#include "SentinelLogging.hpp"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QElapsedTimer>
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

bool candleDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG");
    return enabled;
}

bool mappingChanged(const HeatmapTimeMapping& a, const HeatmapTimeMapping& b) {
    return a.valid != b.valid ||
        a.drawRect != b.drawRect ||
        a.srcRect != b.srcRect ||
        a.dataStartMs != b.dataStartMs ||
        a.actualDataStartMs != b.actualDataStartMs ||
        a.actualDataEndMs != b.actualDataEndMs ||
        a.appendMs != b.appendMs ||
        a.gridWidth != b.gridWidth ||
        a.filledColumns != b.filledColumns ||
        a.timeOffset != b.timeOffset ||
        a.cellW != b.cellW;
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

QObject* CandlestickOverlayItem::heatmapRenderer() const {
    return static_cast<QObject*>(m_heatmapRenderer.data());
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

void CandlestickOverlayItem::setHeatmapRenderer(QObject* renderer) {
    if (m_heatmapRenderer == renderer) {
        return;
    }
    if (m_rendererViewportConn) {
        disconnect(m_rendererViewportConn);
    }
    if (m_rendererTimeframeConn) {
        disconnect(m_rendererTimeframeConn);
    }
    m_heatmapRenderer = qobject_cast<UnifiedGridRenderer*>(renderer);
    if (m_heatmapRenderer) {
        m_rendererViewportConn = connect(m_heatmapRenderer, &UnifiedGridRenderer::viewportChanged, this, [this]() {
            markGeometryDirty();
        });
        m_rendererTimeframeConn = connect(m_heatmapRenderer, &UnifiedGridRenderer::timeframeChanged, this, [this]() {
            markGeometryDirty();
        });
    }
    markGeometryDirty();
    emit heatmapRendererChanged();
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

QSGNode* CandlestickOverlayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<CandleOverlayNode*>(oldNode);
    if (!root) {
        root = new CandleOverlayNode();
    }

    if (!m_viewState || !m_viewState->isTimeWindowValid() || !m_heatmapRenderer) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const HeatmapTimeMapping mapping = m_heatmapRenderer->lastHeatmapMapping();
    if (mappingChanged(mapping, m_lastMapping)) {
        m_lastMapping = mapping;
        m_geometryDirty = true;
    }
    if (!mapping.valid) {
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
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    auto startIt = std::lower_bound(m_visibleCandles.begin(), m_visibleCandles.end(), timeStart,
                                    [](const CandleOverlayBar& c, qint64 t) { return c.timeStartMs < t; });
    auto endIt = std::upper_bound(m_visibleCandles.begin(), m_visibleCandles.end(), timeEnd,
                                  [](qint64 t, const CandleOverlayBar& c) { return t < c.timeStartMs; });
    const double baseColF = mapping.srcRect.x() + static_cast<double>(mapping.timeOffset) * mapping.gridWidth;
    double anchorBaseColF = baseColF;
    if (mapping.actualDataEndMs > mapping.actualDataStartMs && mapping.appendMs > 0.0) {
        const double shiftCols = (mapping.actualDataStartMs - mapping.dataStartMs) / mapping.appendMs;
        anchorBaseColF = baseColF - shiftCols;
    }
    const double visibleColStart = anchorBaseColF;
    const double visibleColEnd = anchorBaseColF + mapping.srcRect.width();
    const bool haveActualRange = (mapping.actualDataEndMs > mapping.actualDataStartMs);
    std::vector<CandleOverlayBar> filtered;
    filtered.reserve(static_cast<size_t>(std::distance(startIt, endIt)));
    for (auto it = startIt; it != endIt; ++it) {
        if (haveActualRange) {
            if (it->timeStartMs < static_cast<qint64>(mapping.actualDataStartMs) ||
                it->timeStartMs >= static_cast<qint64>(mapping.actualDataEndMs)) {
                continue;
            }
        }
        const double colF = (static_cast<double>(it->timeStartMs) - mapping.actualDataStartMs) / mapping.appendMs;
        const double colEndF = colF + 1.0;
        if (colEndF <= visibleColStart || colF >= visibleColEnd) {
            continue;
        }
        filtered.push_back(*it);
    }
    const int visibleCount = static_cast<int>(filtered.size());

    if (candleDebugEnabled()) {
        static QElapsedTimer timer;
        static bool started = false;
        if (!started) {
            timer.start();
            started = true;
        }
        if (timer.elapsed() > 1000) {
            const double spanMs = static_cast<double>(timeEnd - timeStart);
            const double msPerPixel = (width() > 0.0) ? (spanMs / width()) : 0.0;
            const QPointF pan = m_viewState ? m_viewState->getPanVisualOffset() : QPointF();
            const bool dragging = m_viewState ? m_viewState->isDragging() : false;
            const qint64 visFirst = (visibleCount > 0) ? filtered.front().timeStartMs : 0;
            const qint64 visLast = (visibleCount > 0) ? filtered.back().timeStartMs : 0;
            const qint64 newestCandle = (visibleCount > 0) ? filtered.back().timeStartMs : 0;
            const double newestColF = (visibleCount > 0)
                ? (static_cast<double>(newestCandle) - mapping.dataStartMs) / mapping.appendMs
                : 0.0;
            const bool autoScroll = m_viewState ? m_viewState->isAutoScrollEnabled() : false;
            sLog_Debug(QString("Candle overlay: symbol=%1 tfSec=%2 view=[%3..%4] visible=%5 source=%6")
                       .arg(m_symbol)
                       .arg(m_timeframeSec)
                       .arg(timeStart)
                       .arg(timeEnd)
                       .arg(visibleCount)
                       .arg(hasData ? "live" : "none"));
            sLog_Debug(QString("Candle mapping: dataStart=%1 appendMs=%2 gridWidth=%3 timeOffset=%4 baseColF=%5 srcX=%6 srcW=%7 drawX=%8 drawW=%9")
                       .arg(static_cast<qint64>(mapping.dataStartMs))
                       .arg(static_cast<qint64>(mapping.appendMs))
                       .arg(mapping.gridWidth)
                       .arg(mapping.timeOffset, 0, 'f', 4)
                       .arg(baseColF, 0, 'f', 3)
                       .arg(mapping.srcRect.x(), 0, 'f', 3)
                       .arg(mapping.srcRect.width(), 0, 'f', 3)
                       .arg(mapping.drawRect.x(), 0, 'f', 1)
                       .arg(mapping.drawRect.width(), 0, 'f', 1));
            sLog_Debug(QString("Candle mapping newest: t=%1 colF=%2 visCols=[%3..%4] autoScroll=%5")
                       .arg(newestCandle)
                       .arg(newestColF, 0, 'f', 3)
                       .arg(visibleColStart, 0, 'f', 3)
                       .arg(visibleColEnd, 0, 'f', 3)
                       .arg(autoScroll ? "true" : "false"));
            sLog_Debug(QString("Candle overlay scale: spanMs=%1 msPerPx=%2 width=%3")
                       .arg(spanMs, 0, 'f', 1)
                       .arg(msPerPixel, 0, 'f', 3)
                       .arg(width(), 0, 'f', 1));
            sLog_Debug(QString("Candle overlay pan: dragging=%1 pan=(%2,%3) visFirst=%4 visLast=%5")
                       .arg(dragging ? "true" : "false")
                       .arg(pan.x(), 0, 'f', 1)
                       .arg(pan.y(), 0, 'f', 1)
                       .arg(visFirst)
                       .arg(visLast));
            timer.restart();
        }
    }

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

    const QRectF bounds = boundingRect();
    double minPrice = m_viewState->getMinPrice();
    double maxPrice = m_viewState->getMaxPrice();
    const QPointF pan = m_viewState->getPanVisualOffset();
    double priceSpan = maxPrice - minPrice;
    if (!pan.isNull() && bounds.height() > 0.0 && priceSpan > 0.0 && m_viewState->isDragging()) {
        const double pricePixelsToUnits = priceSpan / bounds.height();
        const double priceDelta = pan.y() * pricePixelsToUnits;
        minPrice += priceDelta;
        maxPrice += priceDelta;
        priceSpan = maxPrice - minPrice;
    }
    if (priceSpan <= 0.0 || bounds.height() <= 0.0) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

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

    for (const auto& c : filtered) {
        const bool bullish = c.close >= c.open;

        const double colF = (static_cast<double>(c.timeStartMs) - mapping.actualDataStartMs) / mapping.appendMs;
        const double x = mapping.drawRect.x() + (colF - anchorBaseColF) * mapping.cellW;
        const float x0 = static_cast<float>(x);
        const float x1 = static_cast<float>(x + mapping.cellW);
        float bodyWidth = std::max(1.0f, static_cast<float>(mapping.cellW) * 0.7f);
        const float centerX = x0 + static_cast<float>(mapping.cellW) * 0.5f;
        const float bodyX0 = centerX - bodyWidth * 0.5f;
        const float bodyX1 = centerX + bodyWidth * 0.5f;

        const float wickWidth = std::max(1.0f, bodyWidth * 0.2f);
        const float wickX0 = centerX - wickWidth * 0.5f;
        const float wickX1 = centerX + wickWidth * 0.5f;

        const double yHigh = bounds.y() + bounds.height() * ((maxPrice - c.high) / priceSpan);
        const double yLow = bounds.y() + bounds.height() * ((maxPrice - c.low) / priceSpan);
        const double yOpen = bounds.y() + bounds.height() * ((maxPrice - c.open) / priceSpan);
        const double yClose = bounds.y() + bounds.height() * ((maxPrice - c.close) / priceSpan);
        const float yHighF = static_cast<float>(yHigh);
        const float yLowF = static_cast<float>(yLow);
        const float yOpenF = static_cast<float>(yOpen);
        const float yCloseF = static_cast<float>(yClose);
        const float bodyY0 = std::min(yOpenF, yCloseF);
        const float bodyY1 = std::max(yOpenF, yCloseF);

        addQuad(wickVerts, wickX0, yHighF, wickX1, yLowF, wickR, wickG, wickB, wickA);
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
