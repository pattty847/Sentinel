/*
Sentinel — CandlestickOverlayItem
GPU-batched candlestick overlay (demo data only).
*/
#include "CandlestickOverlayItem.hpp"
#include "../datasources/CandleSeriesBuffer.hpp"
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

std::vector<CandleOverlayBar> buildContinuousBars(const std::vector<CandleOverlayBar>& source,
                                                  int64_t timeframeMs,
                                                  qint64 boundaryStartMs,
                                                  int maxColumns) {
    if (source.empty() || timeframeMs <= 0 || maxColumns <= 0) {
        return source;
    }

    std::vector<CandleOverlayBar> out;
    out.reserve(static_cast<size_t>(maxColumns));

    int syntheticBudget = std::max(0, maxColumns - static_cast<int>(source.size()));
    auto pushBar = [&out, maxColumns](const CandleOverlayBar& bar) {
        if (static_cast<int>(out.size()) < maxColumns) {
            out.push_back(bar);
        }
    };

    pushBar(source.front());
    for (size_t i = 1; i < source.size() && static_cast<int>(out.size()) < maxColumns; ++i) {
        const CandleOverlayBar& prev = source[i - 1];
        const CandleOverlayBar& cur = source[i];
        for (qint64 t = prev.timeStartMs + timeframeMs;
             t < cur.timeStartMs && syntheticBudget > 0 && static_cast<int>(out.size()) < maxColumns;
             t += timeframeMs) {
            CandleOverlayBar synthetic;
            synthetic.timeStartMs = t;
            synthetic.timeEndMs = t + timeframeMs;
            synthetic.open = prev.close;
            synthetic.high = prev.close;
            synthetic.low = prev.close;
            synthetic.close = prev.close;
            out.push_back(synthetic);
            --syntheticBudget;
        }
        pushBar(cur);
    }

    if (boundaryStartMs > 0 && !out.empty() && syntheticBudget > 0) {
        const CandleOverlayBar anchor = out.back();
        for (qint64 t = anchor.timeStartMs + timeframeMs;
             t <= boundaryStartMs && syntheticBudget > 0 && static_cast<int>(out.size()) < maxColumns;
             t += timeframeMs) {
            CandleOverlayBar synthetic;
            synthetic.timeStartMs = t;
            synthetic.timeEndMs = t + timeframeMs;
            synthetic.open = anchor.close;
            synthetic.high = anchor.close;
            synthetic.low = anchor.close;
            synthetic.close = anchor.close;
            out.push_back(synthetic);
            --syntheticBudget;
        }
    }

    return out;
}

bool mappingChanged(const TimeAxisMapping& a, const TimeAxisMapping& b) {
    // INV-005: timeOffset is shader-only ring wrap for heatmap sampling.
    // Candle geometry uses mapping helpers (world->screen), so timeOffset must not drive dirty.
    return a.valid != b.valid ||
        a.drawRect != b.drawRect ||
        a.srcRect != b.srcRect ||
        a.dataStartMs != b.dataStartMs ||
        a.actualDataStartMs != b.actualDataStartMs ||
        a.actualDataEndMs != b.actualDataEndMs ||
        a.appendMs != b.appendMs ||
        a.gridWidth != b.gridWidth ||
        a.filledColumns != b.filledColumns ||
        a.cellW != b.cellW ||
        a.viewStartMs != b.viewStartMs ||
        a.viewEndMs != b.viewEndMs ||
        a.viewMinPrice != b.viewMinPrice ||
        a.viewMaxPrice != b.viewMaxPrice;
}

} // namespace

CandlestickOverlayItem::CandlestickOverlayItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

QObject* CandlestickOverlayItem::candleBuffer() const {
    return static_cast<QObject*>(m_candleBuffer.data());
}

QObject* CandlestickOverlayItem::mappingProvider() const {
    return m_mappingProviderObject.data();
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

void CandlestickOverlayItem::setMappingProvider(QObject* provider) {
    if (m_mappingProviderObject == provider) {
        return;
    }
    if (m_mappingViewportConn) {
        disconnect(m_mappingViewportConn);
    }
    if (m_mappingTimeframeConn) {
        disconnect(m_mappingTimeframeConn);
    }
    m_mappingProviderObject = provider;
    m_mappingProvider = qobject_cast<ITimeAxisMappingProvider*>(provider);
    m_lastBoundarySequence = std::numeric_limits<qint64>::min();
    if (m_mappingProviderObject) {
        m_mappingViewportConn = QObject::connect(m_mappingProviderObject.data(),
                                                 SIGNAL(viewportChanged()),
                                                 this,
                                                 SLOT(update()));
        m_mappingTimeframeConn = QObject::connect(m_mappingProviderObject.data(),
                                                  SIGNAL(timeframeChanged()),
                                                  this,
                                                  SLOT(update()));
    }
    markGeometryDirty();
    emit mappingProviderChanged();
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
    m_lastBoundarySequence = std::numeric_limits<qint64>::min();
    markGeometryDirty();
    emit timeframeSecChanged();
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
                                    if (!m_mappingProvider) {
                                        return;
                                    }
                                    const MappingFrameContext frame = m_mappingProvider->currentFrameContext();
                                    if (!frame.viewportValid) {
                                        return;
                                    }
                                    const qint64 viewStart = frame.viewportTimeStart;
                                    const qint64 viewEnd = frame.viewportTimeEnd;
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

    if (!m_mappingProvider) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const MappingFrameContext frame = m_mappingProvider->currentFrameContext();
    const TimeAxisMapping mapping = frame.mapping;
    if (mappingChanged(mapping, m_lastMapping)) {
        m_lastMapping = mapping;
        m_geometryDirty = true;
    }
    if (!mapping.valid || !frame.viewportValid) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const QSizeF currentSize(width(), height());
    const uint64_t candleGeneration = frame.candleGeneration;
    const qint64 expectedTfMs = static_cast<qint64>(m_timeframeSec) * 1000;
    const bool cadenceMatches = (expectedTfMs > 0 && frame.activeTimeframeMs == expectedTfMs);
    if (cadenceMatches && frame.boundarySequence != m_lastBoundarySequence) {
        // Boundary progression comes from TimeAuthority and may advance with sparse/no-event windows.
        // Treat it as a geometry-affecting trigger for candle cadence continuity.
        m_geometryDirty = true;
    }
    if (cadenceMatches) {
        m_lastBoundarySequence = frame.boundarySequence;
    }
    if (!m_geometryDirty && m_lastCandleGeneration == candleGeneration && m_lastSize == currentSize) {
        return root;
    }

    m_lastCandleGeneration = candleGeneration;
    m_lastSize = currentSize;
    m_geometryDirty = false;

    const qint64 timeStart = frame.viewportTimeStart;
    const qint64 timeEnd = frame.viewportTimeEnd;
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

    // Filter candles to visible data range using TimeAxisMapping
    const double visStartMs = mapping.visibleDataStartMs();
    const double visEndMs = mapping.visibleDataEndMs();
    const bool haveActualRange = (mapping.actualDataEndMs > mapping.actualDataStartMs);

    std::vector<CandleOverlayBar> filtered;
    filtered.reserve(m_visibleCandles.size());
    for (const auto& c : m_visibleCandles) {
        if (haveActualRange) {
            if (c.timeStartMs < static_cast<qint64>(mapping.actualDataStartMs) ||
                c.timeStartMs >= static_cast<qint64>(mapping.actualDataEndMs)) {
                continue;
            }
        }
        const double candleEndMs = static_cast<double>(c.timeStartMs) + mapping.appendMs;
        if (candleEndMs <= visStartMs || static_cast<double>(c.timeStartMs) >= visEndMs) {
            continue;
        }
        filtered.push_back(c);
    }
    const int64_t tfMs = static_cast<int64_t>(std::llround(mapping.appendMs));
    const int maxColumns = std::max(0, mapping.gridWidth);
    if (cadenceMatches && tfMs > 0 && maxColumns > 0) {
        filtered = buildContinuousBars(filtered, tfMs, frame.currentBoundaryStartMs, maxColumns);
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
            const QPointF pan = frame.viewportPanVisualOffset;
            const bool dragging = frame.viewportDragging;
            const qint64 visFirst = (visibleCount > 0) ? filtered.front().timeStartMs : 0;
            const qint64 visLast = (visibleCount > 0) ? filtered.back().timeStartMs : 0;
            const bool autoScroll = frame.viewportAutoScrollEnabled;
            sLog_Debug(QString("Candle overlay: symbol=%1 tfSec=%2 view=[%3..%4] visible=%5 source=%6")
                       .arg(m_symbol)
                       .arg(m_timeframeSec)
                       .arg(timeStart)
                       .arg(timeEnd)
                       .arg(visibleCount)
                       .arg(hasData ? "live" : "none"));
            sLog_Debug(QString("Candle mapping: dataStart=%1 appendMs=%2 gridWidth=%3 srcX=%4 srcW=%5 drawX=%6 drawW=%7")
                       .arg(static_cast<qint64>(mapping.dataStartMs))
                       .arg(static_cast<qint64>(mapping.appendMs))
                       .arg(mapping.gridWidth)
                       .arg(mapping.srcRect.x(), 0, 'f', 3)
                       .arg(mapping.srcRect.width(), 0, 'f', 3)
                       .arg(mapping.drawRect.x(), 0, 'f', 1)
                       .arg(mapping.drawRect.width(), 0, 'f', 1));
            sLog_Debug(QString("Candle overlay scale: spanMs=%1 msPerPx=%2 width=%3 autoScroll=%4")
                       .arg(spanMs, 0, 'f', 1)
                       .arg(msPerPixel, 0, 'f', 3)
                       .arg(width(), 0, 'f', 1)
                       .arg(autoScroll ? "true" : "false"));
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

        // X: world time → screen via TimeAxisMapping (no timeOffset, no manual column math)
        const double x = mapping.timeToScreenX(static_cast<double>(c.timeStartMs));
        const double xEnd = mapping.timeToScreenX(static_cast<double>(c.timeStartMs) + mapping.appendMs);
        const double candleW = xEnd - x;
        float bodyWidth = std::max(1.0f, static_cast<float>(candleW) * 0.7f);
        const float centerX = static_cast<float>(x + candleW * 0.5);
        const float bodyX0 = centerX - bodyWidth * 0.5f;
        const float bodyX1 = centerX + bodyWidth * 0.5f;

        const float wickWidth = std::max(1.0f, bodyWidth * 0.2f);
        const float wickX0 = centerX - wickWidth * 0.5f;
        const float wickX1 = centerX + wickWidth * 0.5f;

        // Y: price → screen via TimeAxisMapping (pan already baked in)
        const float yHighF = static_cast<float>(mapping.priceToScreenY(c.high));
        const float yLowF  = static_cast<float>(mapping.priceToScreenY(c.low));
        const float yOpenF = static_cast<float>(mapping.priceToScreenY(c.open));
        const float yCloseF = static_cast<float>(mapping.priceToScreenY(c.close));
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
