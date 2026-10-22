/*
Sentinel — CandlestickOverlayItem
GPU-batched candlestick overlay (demo data only).
*/
#include "CandlestickOverlayItem.hpp"
#include "CandlePixelGeometry.hpp"
#include "../chart/core/CandleInterval.hpp"
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
    int wickCapacity = 0, bodyCapacity = 0;
    candle_pixels::AxisSnap snap;
    double surfaceDpr = 0;
    void setCounts(int wickCount, int bodyCount) {
        candle_pixels::setGeometryCount(*wickGeometry, wickCapacity, wickCount);
        candle_pixels::setGeometryCount(*bodyGeometry, bodyCapacity, bodyCount);
    }
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

// Draws a diagonal line segment as a thin quad (2 triangles).
inline void addLineSegment(QSGGeometry::ColoredPoint2D*& v,
                           float x1, float y1, float x2, float y2,
                           float thickness,
                           uchar r, uchar g, uchar b, uchar a) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f) {
        // Degenerate segment — emit invisible (collapsed) verts
        for (int i = 0; i < 6; ++i) v[i].set(x1, y1, r, g, b, 0);
        v += 6;
        return;
    }
    const float ht = thickness * 0.5f;
    const float px = -dy / len * ht;
    const float py =  dx / len * ht;
    v[0].set(x1 + px, y1 + py, r, g, b, a);
    v[1].set(x1 - px, y1 - py, r, g, b, a);
    v[2].set(x2 + px, y2 + py, r, g, b, a);
    v[3].set(x1 - px, y1 - py, r, g, b, a);
    v[4].set(x2 - px, y2 - py, r, g, b, a);
    v[5].set(x2 + px, y2 + py, r, g, b, a);
    v += 6;
}

CandleOverlayBar carryBar(qint64 t, int64_t timeframeMs, double close) {
    CandleOverlayBar synthetic;
    synthetic.timeStartMs = t;
    synthetic.timeEndMs = t + timeframeMs;
    synthetic.open = synthetic.high = synthetic.low = synthetic.close = close;
    return synthetic;
}

// Appends flat carry bars for the empty buckets after `from` up to `until`, only
// where the series was fetched: a covered empty bucket had no trades, an uncovered
// one was never loaded and draws nothing.
void appendCarryBars(const CandleOverlayBar& from, qint64 until, int64_t timeframeMs,
                     const CandleCoverage& coverage, int maxColumns, int& budget,
                     std::vector<CandleOverlayBar>& out) {
    qint64 t = from.timeStartMs + timeframeMs;
    while (t < until && budget > 0 && static_cast<int>(out.size()) < maxColumns) {
        if (!coverage.covers(t)) {
            const int64_t next = coverage.nextCovered(t);
            if (next >= until) return;
            t += (next - t + timeframeMs - 1) / timeframeMs * timeframeMs;
            continue;
        }
        out.push_back(carryBar(t, timeframeMs, from.close));
        --budget;
        t += timeframeMs;
    }
}

void buildContinuousBars(const std::vector<CandleOverlayBar>& source,
                         int64_t timeframeMs, qint64 boundaryStartMs,
                         const CandleCoverage& coverage,
                         int maxColumns, std::vector<CandleOverlayBar>& out) {
    out.clear();
    if (source.empty() || timeframeMs <= 0 || maxColumns <= 0) {
        return;
    }
    out.reserve(static_cast<size_t>(maxColumns));

    int syntheticBudget = std::max(0, maxColumns - static_cast<int>(source.size()));
    auto pushBar = [&out, maxColumns](const CandleOverlayBar& bar) {
        if (static_cast<int>(out.size()) < maxColumns) {
            out.push_back(bar);
        }
    };

    pushBar(source.front());
    for (size_t i = 1; i < source.size() && static_cast<int>(out.size()) < maxColumns; ++i) {
        appendCarryBars(source[i - 1], source[i].timeStartMs, timeframeMs, coverage, maxColumns,
                        syntheticBudget, out);
        pushBar(source[i]);
    }

    if (boundaryStartMs > 0 && !out.empty() && syntheticBudget > 0) {
        const CandleOverlayBar anchor = out.back();
        appendCarryBars(anchor, boundaryStartMs, timeframeMs, coverage, maxColumns, syntheticBudget, out);
    }
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
    if (m_mappingPanConn) {
        disconnect(m_mappingPanConn);
    }
    if (m_mappingTimeframeConn) {
        disconnect(m_mappingTimeframeConn);
    }
    m_mappingProviderObject = provider;
    m_mappingProvider = qobject_cast<ITimeAxisMappingProvider*>(provider);
    m_lastBoundarySequence = std::numeric_limits<qint64>::min();
    if (m_mappingProviderObject) {
        auto* mapping = qobject_cast<ITimeAxisMappingProvider*>(provider);
        auto* view = mapping ? mapping->viewNotifications() : nullptr;
        m_mappingViewportConn = QObject::connect(view,
                                                 SIGNAL(viewportChanged()),
                                                 this,
                                                 SLOT(update()));
        m_mappingPanConn = QObject::connect(view,
                                            SIGNAL(panVisualOffsetChanged()),
                                            this,
                                            SLOT(onPanVisualOffsetChanged()));
        m_mappingTimeframeConn = QObject::connect(view,
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
    sLog_Render("Candle overlay symbol: " << m_symbol << "->" << symbol << " tfMs=" << m_timeframeMs);
    m_symbol = symbol;
    markGeometryDirty();
    emit symbolChanged();
}

void CandlestickOverlayItem::setTimeframeMs(int ms) {
    if (m_timeframeMs == ms) {
        return;
    }
    sLog_Render("Candle overlay timeframe: tfMs=" << m_timeframeMs << "->" << ms << " symbol=" << m_symbol);
    m_timeframeMs = ms;
    m_lastBoundarySequence = std::numeric_limits<qint64>::min();
    markGeometryDirty();
    emit timeframeMsChanged();
}

void CandlestickOverlayItem::setCandleStyle(int style) {
    style = std::clamp(style, 0, 2);
    if (m_candleStyle == style) return;
    m_candleStyle = style;
    markGeometryDirty();
    emit candleStyleChanged();
}

void CandlestickOverlayItem::setUpColor(const QColor& value) {
    if (!value.isValid() || m_upColor == value) return;
    m_upColor = value; markGeometryDirty(); emit appearanceChanged();
}
void CandlestickOverlayItem::setDownColor(const QColor& value) {
    if (!value.isValid() || m_downColor == value) return;
    m_downColor = value; markGeometryDirty(); emit appearanceChanged();
}
void CandlestickOverlayItem::setWickColor(const QString& value) {
    const QColor parsed = value == "auto" ? QColor{} : QColor(value);
    if (value != "auto" && !parsed.isValid()) return;
    if (m_wickColor == value) return;
    m_wickColor = value;
    m_customWickColor = parsed;
    markGeometryDirty(); emit appearanceChanged();
}
void CandlestickOverlayItem::setBodyOpacity(double value) {
    if (!std::isfinite(value)) return;
    value = std::clamp(value, 0.0, 1.0);
    if (m_bodyOpacity == value) return;
    m_bodyOpacity = value; markGeometryDirty(); emit appearanceChanged();
}
void CandlestickOverlayItem::setWickWidth(int value) {
    value = std::clamp(value, 1, 3);
    if (m_wickWidth == value) return;
    m_wickWidth = value; markGeometryDirty(); emit appearanceChanged();
}

void CandlestickOverlayItem::connectCandleSignals() {
    if (!m_candleBuffer) {
        return;
    }
    m_candleDirtyConn = connect(m_candleBuffer, &CandleSeriesBuffer::candlesDirty,
                                this,
                                [this](const QString& symbol,
                                       int64_t timeframeMs,
                                       qint64 dirtyStartMs,
                                       qint64 dirtyEndMs) {
                                    if (symbol != m_symbol || timeframeMs != m_timeframeMs) {
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

void CandlestickOverlayItem::onPanVisualOffsetChanged() {
    markGeometryDirty();
}

QSGNode* CandlestickOverlayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<CandleOverlayNode*>(oldNode);
    if (!root) {
        root = new CandleOverlayNode();
    }

    if (!m_mappingProvider || m_timeframeMs <= 0) {
        m_geometryDirty = true;
        root->setCounts(0, 0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const MappingFrameContext frame = m_mappingProvider->currentFrameContext();
    const TimeAxisMapping mapping = frame.mapping;
    const auto snap = candle_pixels::axisSnap(frame.raster);
    if (mappingChanged(mapping, m_lastMapping) || root->snap != snap ||
        root->surfaceDpr != frame.surfaceDpr) {
        root->snap = snap;
        root->surfaceDpr = frame.surfaceDpr;
        m_lastMapping = mapping;
        m_geometryDirty = true;
    }
    if (!mapping.valid || !frame.viewportValid) {
        m_geometryDirty = true;
        root->setCounts(0, 0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const QSizeF currentSize(width(), height());
    const qint64 expectedTfMs = m_timeframeMs;
    const bool cadenceMatches = (expectedTfMs > 0 && frame.activeTimeframeMs == expectedTfMs);
    if (cadenceMatches && frame.boundarySequence != m_lastBoundarySequence) {
        // Boundary progression comes from TimeAuthority and may advance with sparse/no-event windows.
        // Treat it as a geometry-affecting trigger for candle cadence continuity.
        m_geometryDirty = true;
    }
    if (cadenceMatches) {
        m_lastBoundarySequence = frame.boundarySequence;
    }
    if (!m_geometryDirty && m_lastSize == currentSize) {
        return root;
    }

    m_lastSize = currentSize;
    m_geometryDirty = false;

    const qint64 timeStart = frame.viewportTimeStart;
    const qint64 timeEnd = frame.viewportTimeEnd;
    if (timeEnd <= timeStart) {
        return root;
    }

    bool hasData = false;
    m_visibleCandles.clear();
    if (m_candleBuffer && !m_symbol.isEmpty() && m_timeframeMs > 0) {
        m_bufferSlice.clear();
        // gpu renderer: from the bucket under the (drag-panned) left edge, so the candle
        // that starts before the view and ends inside it is drawn (the slice selects by
        // bar start). Legacy keeps the viewport bounds.
        qint64 sliceStart = timeStart, sliceEnd = timeEnd;
        if (mapping.viewportColumns && mapping.appendMs > 0) {
            const double tf = mapping.appendMs;
            sliceStart = static_cast<qint64>(std::floor(mapping.viewStartMs / tf) * tf);
            sliceEnd = static_cast<qint64>(std::ceil(mapping.viewEndMs));
        }
        hasData = m_candleBuffer->getVisibleSlice(m_symbol,
                                                  m_timeframeMs,
                                                  sliceStart,
                                                  sliceEnd,
                                                  m_bufferSlice);
        if (hasData) {
            m_visibleCandles.reserve(m_bufferSlice.size());
            for (const auto& bar : m_bufferSlice) {
                if (!chart::drawsCandle(m_timeframeMs, bar.volume)) continue;
                CandleOverlayBar out;
                out.timeStartMs = bar.timeStartMs;
                out.timeEndMs = bar.timeEndMs;
                out.open = bar.open;
                out.high = bar.high;
                out.low = bar.low;
                out.close = bar.close;
                out.volume = bar.volume;
                m_visibleCandles.push_back(out);
            }
        }
    }
    if (!hasData) {
        sLog_Probe("candles.empty", "symbol=" << m_symbol << " tfMs=" << m_timeframeMs
                   << " view=[" << timeStart << ".." << timeEnd << "] buffer=" << (m_candleBuffer != nullptr));
        root->setCounts(0, 0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    // Filter candles to visible data range using TimeAxisMapping
    const double visStartMs = mapping.visibleDataStartMs();
    const double visEndMs = mapping.visibleDataEndMs();
    const bool haveActualRange = (mapping.actualDataEndMs > mapping.actualDataStartMs);

    auto& filtered = m_filteredCandles;
    filtered.clear();
    filtered.reserve(m_visibleCandles.size());
    for (const auto& c : m_visibleCandles) {
        if (!std::isfinite(c.open) || !std::isfinite(c.high) || !std::isfinite(c.low) || !std::isfinite(c.close))
            continue;
        if (haveActualRange && !mapping.viewportColumns) {
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
    const int baseVisibleCount = static_cast<int>(filtered.size());
    const int64_t tfMs = static_cast<int64_t>(std::llround(mapping.appendMs));
    const int maxColumns = std::max(0, mapping.gridWidth);
    const bool synthesize = cadenceMatches && tfMs > 0 && maxColumns > 0 &&
                            chart::synthesizesQuietBars(m_timeframeMs);
    static const CandleCoverage kNoCoverage;
    const CandleCoverage* coveragePtr = m_candleBuffer ? m_candleBuffer->coverage(m_symbol, m_timeframeMs) : nullptr;
    const CandleCoverage& coverage = coveragePtr ? *coveragePtr : kNoCoverage;
    if (synthesize) {
        buildContinuousBars(filtered, tfMs, frame.currentBoundaryStartMs, coverage, maxColumns, m_continuousCandles);
    }
    const auto& drawBars = synthesize ? m_continuousCandles : filtered;
    const int visibleCount = static_cast<int>(drawBars.size());
    const int syntheticCount = std::max(0, visibleCount - baseVisibleCount);

    // Probe candles.frame: overlay/mapping state, at most once per second.
    static const bool kCandleProbe = sentinel::logging::probeEnabled("candles.frame");
    if (kCandleProbe) {
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
            const qint64 visFirst = (visibleCount > 0) ? drawBars.front().timeStartMs : 0;
            const qint64 visLast = (visibleCount > 0) ? drawBars.back().timeStartMs : 0;
            const bool autoScroll = frame.viewportAutoScrollEnabled;
            sLog_Probe("candles.frame", QString("overlay: symbol=%1 tfMs=%2 view=[%3..%4] visible=%5 base_visible=%6 synthetic=%7 source=%8 cadence_match=%9 boundary_seq=%10 boundary_start=%11")
                       .arg(m_symbol)
                       .arg(m_timeframeMs)
                       .arg(timeStart)
                       .arg(timeEnd)
                       .arg(visibleCount)
                       .arg(baseVisibleCount)
                       .arg(syntheticCount)
                       .arg(hasData ? "live" : "none")
                       .arg(cadenceMatches ? "true" : "false")
                       .arg(frame.boundarySequence)
                       .arg(frame.currentBoundaryStartMs));
            sLog_Probe("candles.frame", QString("mapping: dataStart=%1 appendMs=%2 gridWidth=%3 srcX=%4 srcW=%5 drawX=%6 drawW=%7")
                       .arg(static_cast<qint64>(mapping.dataStartMs))
                       .arg(static_cast<qint64>(mapping.appendMs))
                       .arg(mapping.gridWidth)
                       .arg(mapping.srcRect.x(), 0, 'f', 3)
                       .arg(mapping.srcRect.width(), 0, 'f', 3)
                       .arg(mapping.drawRect.x(), 0, 'f', 1)
                       .arg(mapping.drawRect.width(), 0, 'f', 1));
            sLog_Probe("candles.frame", QString("overlay scale: spanMs=%1 msPerPx=%2 width=%3 autoScroll=%4")
                       .arg(spanMs, 0, 'f', 1)
                       .arg(msPerPixel, 0, 'f', 3)
                       .arg(width(), 0, 'f', 1)
                       .arg(autoScroll ? "true" : "false"));
            sLog_Probe("candles.frame", QString("overlay pan: dragging=%1 pan=(%2,%3) visFirst=%4 visLast=%5")
                       .arg(dragging ? "true" : "false")
                       .arg(pan.x(), 0, 'f', 1)
                       .arg(pan.y(), 0, 'f', 1)
                       .arg(visFirst)
                       .arg(visLast));
            timer.restart();
        }
    }

    if (visibleCount <= 0) {
        root->setCounts(0, 0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const bool isHollow = (m_candleStyle == 1);
    const bool isLine   = (m_candleStyle == 2);

    const uchar bullR = uchar(m_upColor.red()), bullG = uchar(m_upColor.green()), bullB = uchar(m_upColor.blue());
    const uchar bearR = uchar(m_downColor.red()), bearG = uchar(m_downColor.green()), bearB = uchar(m_downColor.blue());
    const uchar bodyA = candle_pixels::bodyAlpha(m_bodyOpacity);
    const QColor customWick = m_customWickColor;
    const bool autoWick = m_wickColor == "auto";
    const double dpr = frame.surfaceDpr > 0 && std::isfinite(frame.surfaceDpr) ? frame.surfaceDpr : 1.0;

    // ── Line mode: continuous line connecting all close prices, no candle shapes ─
    if (isLine) {
        auto& pts = m_closePoints;
        pts.clear();
        pts.reserve(static_cast<size_t>(visibleCount));
        for (const auto& c : drawBars) {
            const bool bullish = c.close >= c.open;
            const double x    = mapping.timeToScreenX(static_cast<double>(c.timeStartMs));
            const double xEnd = mapping.timeToScreenX(static_cast<double>(c.timeStartMs) + mapping.appendMs);
            const double cw   = xEnd - x;
            const float cx    = static_cast<float>(x + cw * 0.5);
            pts.push_back({cx,
                           static_cast<float>(mapping.priceToScreenY(c.close)),
                           bullish ? bullR : bearR,
                           bullish ? bullG : bearG,
                           bullish ? bullB : bearB,
                           bodyA});
        }
        // The line does not cross a span that was never fetched.
        const auto joined = [&](int i) {
            const qint64 from = drawBars[size_t(i)].timeStartMs + m_timeframeMs, to = drawBars[size_t(i) + 1].timeStartMs;
            return from >= to || coverage.coversRange(from, to);
        };
        int segCount = 0;
        for (int i = 0; i + 1 < visibleCount; ++i) segCount += joined(i) ? 1 : 0;
        root->setCounts(0, segCount * 6);
        auto* bodyVerts = root->bodyGeometry->vertexDataAsColoredPoint2D();

        for (int i = 0; i + 1 < visibleCount; ++i) {
            if (!joined(i)) continue;
            addLineSegment(bodyVerts,
                           pts[i].cx, pts[i].cy, pts[i + 1].cx, pts[i + 1].cy,
                           1.5f, pts[i].r, pts[i].g, pts[i].b, pts[i].a);
        }

        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    // ── Candle / Hollow mode ───────────────────────────────────────────────────
    // Hollow bullish bodies need 4 border quads (24 verts) instead of 1 filled (6 verts)
    root->setCounts(visibleCount * 6, isHollow ? visibleCount * 24 : visibleCount * 6);
    auto* wickVerts = root->wickGeometry->vertexDataAsColoredPoint2D();
    auto* bodyVerts = root->bodyGeometry->vertexDataAsColoredPoint2D();

    for (const auto& c : drawBars) {
        const bool bullish = c.close >= c.open;

        const double x    = mapping.timeToScreenX(static_cast<double>(c.timeStartMs));
        const double xEnd = mapping.timeToScreenX(static_cast<double>(c.timeStartMs) + mapping.appendMs);
        const double yOpen = mapping.priceToScreenY(c.open);
        const double yClose = mapping.priceToScreenY(c.close);
        const auto body = candle_pixels::body(x, xEnd, yOpen, yClose, dpr, snap.x, snap.y);
        const float bodyX0 = body.x.lo, bodyX1 = body.x.hi;
        const float bodyY0 = body.y.lo, bodyY1 = body.y.hi;
        const float bodyWidth = bodyX1 - bodyX0;
        const double centerX = (double(bodyX0) + bodyX1) * 0.5;

        // An even body and odd wick cannot share an exact device-pixel
        // centre. stroke() resolves that half-pixel tie to the right.
        const auto wickSpan = !snap.x
            ? candle_pixels::Span{float(centerX - m_wickWidth * 0.5 / dpr),
                                  float(centerX + m_wickWidth * 0.5 / dpr)}
            : candle_pixels::stroke(centerX, m_wickWidth, dpr);
        const float wickX0 = wickSpan.lo;
        const float wickX1 = wickSpan.hi;
        const float yHighF = static_cast<float>(mapping.priceToScreenY(c.high));
        const float yLowF = static_cast<float>(mapping.priceToScreenY(c.low));

        const uchar br = bullish ? bullR : bearR;
        const uchar bg = bullish ? bullG : bearG;
        const uchar bb = bullish ? bullB : bearB;
        const uchar ba = bodyA;
        const uchar wr = autoWick ? br : uchar(customWick.red());
        const uchar wg = autoWick ? bg : uchar(customWick.green());
        const uchar wb = autoWick ? bb : uchar(customWick.blue());

        addQuad(wickVerts, wickX0, yHighF, wickX1, yLowF, wr, wg, wb, 255);

        if (isHollow && bullish && c.open != c.close) {
            // Hollow bullish: border outline only (4 quads)
            const float bw = std::max(1.0f, bodyWidth * 0.12f);
            addQuad(bodyVerts, bodyX0, bodyY0, bodyX1, bodyY0 + bw, br, bg, bb, ba); // top
            addQuad(bodyVerts, bodyX0, bodyY1 - bw, bodyX1, bodyY1, br, bg, bb, ba); // bottom
            addQuad(bodyVerts, bodyX0, bodyY0, bodyX0 + bw, bodyY1, br, bg, bb, ba); // left
            addQuad(bodyVerts, bodyX1 - bw, bodyY0, bodyX1, bodyY1, br, bg, bb, ba); // right
        } else {
            // Normal filled body (also bearish candles in Hollow mode)
            addQuad(bodyVerts, bodyX0, bodyY0, bodyX1, bodyY1, br, bg, bb, ba);
            if (isHollow) {
                // Pad 3 unused body quads with collapsed (invisible) entries
                addQuad(bodyVerts, bodyX0, bodyY0, bodyX0, bodyY0, 0, 0, 0, 0);
                addQuad(bodyVerts, bodyX0, bodyY0, bodyX0, bodyY0, 0, 0, 0, 0);
                addQuad(bodyVerts, bodyX0, bodyY0, bodyX0, bodyY0, 0, 0, 0, 0);
            }
        }
    }

    root->wickNode->markDirty(QSGNode::DirtyGeometry);
    root->bodyNode->markDirty(QSGNode::DirtyGeometry);
    return root;
}
