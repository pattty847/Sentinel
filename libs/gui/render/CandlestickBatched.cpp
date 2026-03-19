/*
Sentinel — CandlestickBatched
GPU-batched candlestick renderer with mouse interactions, inline volume bars,
per-candle SEC signal color overrides, and exposed visible-range properties
for QML price/time axes.
*/
#include "CandlestickBatched.hpp"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QElapsedTimer>
#include <QHoverEvent>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <random>

namespace {

// Scene-graph node tree: volume (background) → wicks → bodies.
class CandleRootNode final : public QSGNode {
public:
    CandleRootNode() {
        // Helper: allocate one colored-vertex geometry node and wire it up.
        auto makeNode = [](QSGGeometry*& geom, QSGVertexColorMaterial*& mat) -> QSGGeometryNode* {
            auto* node = new QSGGeometryNode();
            geom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            geom->setDrawingMode(QSGGeometry::DrawTriangles);
            mat  = new QSGVertexColorMaterial();
            node->setGeometry(geom);
            node->setMaterial(mat);
            node->setFlag(QSGNode::OwnsGeometry, true);
            node->setFlag(QSGNode::OwnsMaterial, true);
            return node;
        };

        volNode  = makeNode(volGeometry,  volMaterial);
        wickNode = makeNode(wickGeometry, wickMaterial);
        bodyNode = makeNode(bodyGeometry, bodyMaterial);

        appendChildNode(volNode);   // drawn first → behind candles
        appendChildNode(wickNode);
        appendChildNode(bodyNode);
    }

    QSGGeometryNode*        volNode  = nullptr;
    QSGGeometryNode*        wickNode = nullptr;
    QSGGeometryNode*        bodyNode = nullptr;
    QSGGeometry*            volGeometry  = nullptr;
    QSGGeometry*            wickGeometry = nullptr;
    QSGGeometry*            bodyGeometry = nullptr;
    QSGVertexColorMaterial* volMaterial  = nullptr;
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

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

CandlestickBatched::CandlestickBatched(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
}

// ── Property setters ─────────────────────────────────────────────────────────

void CandlestickBatched::setLodEnabled(bool enabled) {
    if (m_lodEnabled == enabled) return;
    m_lodEnabled = enabled;
    emit lodEnabledChanged();
}

void CandlestickBatched::setCandleWidth(float width) {
    if (qFuzzyCompare(m_candleWidth, width)) return;
    m_candleWidth = width;
    recomputeViewport();
    markDirty();
    emit candleWidthChanged();
}

void CandlestickBatched::setCandleSpacing(float spacing) {
    if (qFuzzyCompare(m_candleSpacing, spacing)) return;
    m_candleSpacing = spacing;
    recomputeViewport();
    markDirty();
    emit candleSpacingChanged();
}

void CandlestickBatched::setVolumeScaling(bool enabled) {
    if (m_volumeScaling == enabled) return;
    m_volumeScaling = enabled;
    emit volumeScalingChanged();
}

void CandlestickBatched::setMaxCandles(int maxCandles) {
    if (m_maxCandles == maxCandles) return;
    m_maxCandles = maxCandles;
    emit maxCandlesChanged();
}

void CandlestickBatched::setViewOffset(float offset) {
    if (qFuzzyCompare(m_viewOffset, offset)) return;
    m_viewOffset = offset;
    recomputeViewport();
    markDirty();
    emit viewOffsetChanged();
}

void CandlestickBatched::setZoomScale(float scale) {
    scale = std::clamp(scale, 0.2f, 10.0f);
    if (qFuzzyCompare(m_zoomScale, scale)) return;
    m_zoomScale = scale;
    recomputeViewport();
    markDirty();
    emit zoomScaleChanged();
}

// ── Data management ───────────────────────────────────────────────────────────

void CandlestickBatched::clearCandles() {
    m_candles.clear();
    m_signalOverrides.clear();
    m_hoveredCandle = -1;
    m_firstVis = -1;
    m_lastVis  = -1;
    m_visMin   = 0.0;
    m_visMax   = 1.0;
    emit candleCountChanged(0);
    emit hoveredCandleChanged(-1);
    emit visibleRangeChanged();
    markDirty();
}

void CandlestickBatched::setCandles(const QVariantList& candles) {
    m_candles.clear();
    m_candles.reserve(candles.size());

    for (const auto& v : candles) {
        QVariantMap m = v.toMap();
        CandleData c;
        c.timestamp = m.value("timestamp", 0).toLongLong();
        c.open  = m.value("open",  0.0).toDouble();
        c.high  = m.value("high",  0.0).toDouble();
        c.low   = m.value("low",   0.0).toDouble();
        c.close = m.value("close", 0.0).toDouble();
        c.volume = m.value("volume", 0.0).toDouble();
        m_candles.push_back(c);
    }

    m_viewOffset = 0.0f;
    updatePriceRange();
    recomputeViewport();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

void CandlestickBatched::addCandle(qint64 timestamp, double open, double high, double low, double close, double volume) {
    CandleData c;
    c.timestamp = timestamp;
    c.open  = open;
    c.high  = high;
    c.low   = low;
    c.close = close;
    c.volume = volume;
    m_candles.push_back(c);

    updatePriceRange();
    recomputeViewport();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

void CandlestickBatched::loadDemoData(int count) {
    m_candles.clear();
    m_candles.reserve(count);

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> priceDist(-2.0, 2.0);
    std::uniform_real_distribution<double> volDist(1000.0, 10000.0);

    double price = 100.0;
    qint64 ts = 1700000000000LL;

    for (int i = 0; i < count; ++i) {
        CandleData c;
        c.timestamp = ts + i * 86400000LL;

        double change = priceDist(rng);
        double volatility = std::abs(priceDist(rng)) + 0.5;

        c.open  = price;
        c.close = price + change;
        c.high  = std::max(c.open, c.close) + volatility;
        c.low   = std::min(c.open, c.close) - volatility;
        c.volume = volDist(rng);

        price = c.close;
        m_candles.push_back(c);
    }

    updatePriceRange();
    recomputeViewport();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

QVariantMap CandlestickBatched::getCandleAt(int index) const {
    QVariantMap result;
    if (index >= 0 && index < static_cast<int>(m_candles.size())) {
        const auto& c = m_candles[index];
        result["timestamp"] = c.timestamp;
        result["open"]   = c.open;
        result["high"]   = c.high;
        result["low"]    = c.low;
        result["close"]  = c.close;
        result["volume"] = c.volume;
        result["bullish"] = c.close >= c.open;
    }
    return result;
}

void CandlestickBatched::setTimeWindow(qint64, qint64) { markDirty(); }
void CandlestickBatched::setAutoLOD(bool enabled) { setLodEnabled(enabled); }
void CandlestickBatched::forceTimeFrame(int timeframeMs) { emit lodLevelChanged(timeframeMs); }
double CandlestickBatched::calculateCurrentPixelsPerCandle() const {
    return std::max(1.0f, m_candleWidth + m_candleSpacing);
}

// ── SEC signal overrides ──────────────────────────────────────────────────────

void CandlestickBatched::setSecSignalOverrides(const QVariantList& overrides) {
    m_signalOverrides.clear();
    for (const auto& v : overrides) {
        const QVariantMap m = v.toMap();
        const int index = m.value("index", -1).toInt();
        const int type  = m.value("signalType", 0).toInt();
        if (index >= 0 && index < static_cast<int>(m_candles.size()) && type > 0)
            m_signalOverrides[index] = static_cast<SignalType>(type);
    }
    markDirty();
}

// ── Appearance ────────────────────────────────────────────────────────────────

void CandlestickBatched::setBullishColor(const QColor& color) {
    if (m_bullishColor == color) return;
    m_bullishColor = color;
    markDirty();
}

void CandlestickBatched::setBearishColor(const QColor& color) {
    if (m_bearishColor == color) return;
    m_bearishColor = color;
    markDirty();
}

void CandlestickBatched::setWickColor(const QColor& color) {
    if (m_wickColor == color) return;
    m_wickColor = color;
    markDirty();
}

void CandlestickBatched::setBuySignalColor(const QColor& color) {
    if (m_buySignalColor == color) return;
    m_buySignalColor = color;
    markDirty();
    emit buySignalColorChanged();
}

void CandlestickBatched::setSellSignalColor(const QColor& color) {
    if (m_sellSignalColor == color) return;
    m_sellSignalColor = color;
    markDirty();
    emit sellSignalColorChanged();
}

void CandlestickBatched::setMixedSignalColor(const QColor& color) {
    if (m_mixedSignalColor == color) return;
    m_mixedSignalColor = color;
    markDirty();
    emit mixedSignalColorChanged();
}

void CandlestickBatched::setVolumeBarAlpha(int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    if (m_volumeBarAlpha == alpha) return;
    m_volumeBarAlpha = alpha;
    markDirty();
    emit volumeBarAlphaChanged();
}

void CandlestickBatched::setCandleStyle(int style) {
    const auto s = static_cast<CandleStyle>(std::clamp(style, 0, 2));
    if (m_candleStyle == s) return;
    m_candleStyle = s;
    markDirty();
    emit candleStyleChanged();
}

// ── Viewport computation (GUI thread) ─────────────────────────────────────────

void CandlestickBatched::recomputeViewport() {
    const float w = static_cast<float>(width());
    if (w <= 0.0f || m_candles.empty()) {
        m_firstVis = -1;
        m_lastVis  = -1;
        m_visMin   = 0.0;
        m_visMax   = 1.0;
        emit visibleRangeChanged();
        return;
    }

    const int n = static_cast<int>(m_candles.size());
    const float totalWidth = (m_candleWidth + m_candleSpacing) * m_zoomScale;
    const float startX = w - static_cast<float>(n) * totalWidth + m_viewOffset;

    const int first = std::max(0, static_cast<int>(std::floor((-startX) / totalWidth)));
    const int last  = std::min(n - 1, static_cast<int>(std::floor((w - startX) / totalWidth)));

    if (first > last) {
        m_firstVis = -1;
        m_lastVis  = -1;
        m_visMin   = 0.0;
        m_visMax   = 1.0;
        emit visibleRangeChanged();
        return;
    }

    m_firstVis = first;
    m_lastVis  = last;

    double vMin = m_candles[first].low;
    double vMax = m_candles[first].high;
    for (int i = first; i <= last; ++i) {
        vMin = std::min(vMin, m_candles[i].low);
        vMax = std::max(vMax, m_candles[i].high);
    }
    double range = vMax - vMin;
    if (range < 0.01) range = 1.0;
    m_visMin = vMin - range * 0.05;
    m_visMax = vMax + range * 0.05;

    emit visibleRangeChanged();
}

void CandlestickBatched::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (!qFuzzyCompare((float)newGeometry.width(), (float)oldGeometry.width()))
        recomputeViewport();
}

// ── Internal helpers ──────────────────────────────────────────────────────────

void CandlestickBatched::updatePriceRange() {
    if (m_candles.empty()) {
        m_priceMin = 0.0;
        m_priceMax = 1.0;
        return;
    }
    m_priceMin = m_candles[0].low;
    m_priceMax = m_candles[0].high;
    for (const auto& c : m_candles) {
        m_priceMin = std::min(m_priceMin, c.low);
        m_priceMax = std::max(m_priceMax, c.high);
    }
    double range = m_priceMax - m_priceMin;
    if (range < 0.01) range = 1.0;
    m_priceMin -= range * 0.05;
    m_priceMax += range * 0.05;
}

int CandlestickBatched::candleAtX(float x) const {
    if (m_candles.empty()) return -1;
    const float totalWidth = (m_candleWidth + m_candleSpacing) * m_zoomScale;
    const float chartWidth = static_cast<float>(width());
    const float startX = chartWidth - static_cast<float>(m_candles.size()) * totalWidth + m_viewOffset;
    const float xInChart = x - startX;
    if (xInChart < 0.0f) return -1;
    int index = static_cast<int>(xInChart / totalWidth);
    if (index < 0 || index >= static_cast<int>(m_candles.size())) return -1;
    const float candleStartX = startX + static_cast<float>(index) * totalWidth;
    const float candleEndX   = candleStartX + m_candleWidth * m_zoomScale;
    if (x >= candleStartX && x <= candleEndX) return index;
    return -1;
}

// ── Mouse events ──────────────────────────────────────────────────────────────

void CandlestickBatched::hoverMoveEvent(QHoverEvent* event) {
    int index = candleAtX(static_cast<float>(event->position().x()));
    if (index != m_hoveredCandle) {
        m_hoveredCandle = index;
        emit hoveredCandleChanged(index);
        markDirty();
    }
}

void CandlestickBatched::hoverLeaveEvent(QHoverEvent*) {
    if (m_hoveredCandle != -1) {
        m_hoveredCandle = -1;
        emit hoveredCandleChanged(-1);
        markDirty();
    }
}

void CandlestickBatched::mousePressEvent(QMouseEvent* event) {
    int index = candleAtX(static_cast<float>(event->position().x()));
    if (index >= 0) emit candleClicked(index);
}

// ── Render (render thread) ────────────────────────────────────────────────────

QSGNode* CandlestickBatched::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QElapsedTimer timer;
    timer.start();

    auto* root = static_cast<CandleRootNode*>(oldNode);
    if (!root) root = new CandleRootNode();

    const float w = static_cast<float>(width());
    const float h = static_cast<float>(height());

    auto clearAll = [&]() {
        root->volGeometry->allocate(0);
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->volNode->markDirty(QSGNode::DirtyGeometry);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
    };

    if (w <= 0.0f || h <= 0.0f || m_candles.empty()) { clearAll(); return root; }

    const int n = static_cast<int>(m_candles.size());
    const float totalWidth = (m_candleWidth + m_candleSpacing) * m_zoomScale;
    const float scaledBody = m_candleWidth * m_zoomScale;
    const float startX = w - static_cast<float>(n) * totalWidth + m_viewOffset;

    const int firstVisible = std::max(0, static_cast<int>(std::floor((-startX) / totalWidth)));
    const int lastVisible  = std::min(n - 1, static_cast<int>(std::floor((w - startX) / totalWidth)));
    const int visCount     = std::max(0, lastVisible - firstVisible + 1);

    if (visCount == 0) { clearAll(); return root; }

    // ── Price range over visible candles ──────────────────────────────────────
    double visMin = m_candles[firstVisible].low;
    double visMax = m_candles[firstVisible].high;
    for (int i = firstVisible; i <= lastVisible; ++i) {
        visMin = std::min(visMin, m_candles[i].low);
        visMax = std::max(visMax, m_candles[i].high);
    }
    double visRange = visMax - visMin;
    if (visRange < 0.01) visRange = 1.0;
    visMin -= visRange * 0.05;
    visMax += visRange * 0.05;
    visRange = visMax - visMin;

    // ── Layout: top (1-kVolFraction) for candles, bottom kVolFraction for volume ──
    const float candleAreaH = h * (1.0f - kVolFraction);
    const float padding     = candleAreaH * 0.05f;
    const float chartHeight = candleAreaH - 2.0f * padding;

    auto priceToY = [&](double price) -> float {
        const double normalized = (price - visMin) / visRange;
        return padding + static_cast<float>(1.0 - normalized) * chartHeight;
    };

    // ── Volume bar metrics ────────────────────────────────────────────────────
    double maxVol = 0.0;
    for (int i = firstVisible; i <= lastVisible; ++i)
        maxVol = std::max(maxVol, m_candles[i].volume);
    if (maxVol < 1.0) maxVol = 1.0;

    const float volAreaTop  = candleAreaH + 2.0f;   // 2px separator gap
    const float volBarMaxH  = h - volAreaTop - 2.0f; // 2px bottom margin

    // ── Allocate geometry ────────────────────────────────────────────────────
    // Hollow bullish bodies need 4 border quads (24 verts) instead of 1 filled (6 verts).
    const bool isHollow = (m_candleStyle == CandleStyle::Hollow);
    const bool isLine   = (m_candleStyle == CandleStyle::Line);
    root->volGeometry->allocate(visCount * 6);
    root->wickGeometry->allocate(isLine ? 0 : visCount * 6);
    root->bodyGeometry->allocate(isHollow ? visCount * 24 : visCount * 6);

    auto* volVerts  = root->volGeometry->vertexDataAsColoredPoint2D();
    auto* wickVerts = isLine ? nullptr : root->wickGeometry->vertexDataAsColoredPoint2D();
    auto* bodyVerts = root->bodyGeometry->vertexDataAsColoredPoint2D();

    const uchar wr = static_cast<uchar>(m_wickColor.red());
    const uchar wg = static_cast<uchar>(m_wickColor.green());
    const uchar wb = static_cast<uchar>(m_wickColor.blue());
    const uchar wa = static_cast<uchar>(m_wickColor.alpha());

    for (int i = firstVisible; i <= lastVisible; ++i) {
        const auto& c = m_candles[i];
        const bool bullish = c.close >= c.open;
        const bool hovered = (i == m_hoveredCandle);

        const float cx  = startX + static_cast<float>(i) * totalWidth;
        const float ccx = cx + scaledBody * 0.5f;

        // ── Wick (skipped in Line mode) ───────────────────────────────────────
        if (!isLine) {
            const float wickW = std::max(1.0f, scaledBody * 0.15f);
            addQuad(wickVerts, ccx - wickW * 0.5f, priceToY(c.high),
                               ccx + wickW * 0.5f, priceToY(c.low), wr, wg, wb, wa);
        }

        // ── Body — SEC signal color override ─────────────────────────────────
        QColor bodyColor;
        const auto it = m_signalOverrides.find(i);
        if (it != m_signalOverrides.end()) {
            switch (it.value()) {
                case SignalType::Buy:   bodyColor = m_buySignalColor;   break;
                case SignalType::Sell:  bodyColor = m_sellSignalColor;  break;
                case SignalType::Mixed: bodyColor = m_mixedSignalColor; break;
                default:               bodyColor = bullish ? m_bullishColor : m_bearishColor; break;
            }
        } else {
            bodyColor = bullish ? m_bullishColor : m_bearishColor;
        }
        if (hovered) bodyColor = bodyColor.lighter(130);

        const uchar br = static_cast<uchar>(bodyColor.red());
        const uchar bg = static_cast<uchar>(bodyColor.green());
        const uchar bb = static_cast<uchar>(bodyColor.blue());
        const uchar ba = static_cast<uchar>(bodyColor.alpha());

        if (isLine) {
            // Line mode: 2px horizontal tick at close price
            const float closeY = priceToY(c.close);
            addQuad(bodyVerts, cx, closeY - 1.0f, cx + scaledBody, closeY + 1.0f, br, bg, bb, ba);
        } else if (isHollow && bullish) {
            // Hollow mode: bullish candles draw as border outline only
            const float bodyY0 = priceToY(std::max(c.open, c.close));
            const float bodyY1 = priceToY(std::min(c.open, c.close));
            const float bw = std::max(1.0f, scaledBody * 0.12f); // border thickness
            // Top
            addQuad(bodyVerts, cx, bodyY0, cx + scaledBody, bodyY0 + bw, br, bg, bb, ba);
            // Bottom
            addQuad(bodyVerts, cx, bodyY1 - bw, cx + scaledBody, bodyY1, br, bg, bb, ba);
            // Left
            addQuad(bodyVerts, cx, bodyY0, cx + bw, bodyY1, br, bg, bb, ba);
            // Right
            addQuad(bodyVerts, cx + scaledBody - bw, bodyY0, cx + scaledBody, bodyY1, br, bg, bb, ba);
        } else {
            // Normal filled body (also used for bearish candles in Hollow mode)
            const float bodyY0 = priceToY(std::max(c.open, c.close));
            const float bodyY1 = priceToY(std::min(c.open, c.close));
            addQuad(bodyVerts, cx, bodyY0, cx + scaledBody, bodyY1, br, bg, bb, ba);
            // Pad remaining 3 quads for hollow-mode allocation (degenerate, same point)
            if (isHollow) {
                addQuad(bodyVerts, cx, bodyY0, cx, bodyY0, 0, 0, 0, 0);
                addQuad(bodyVerts, cx, bodyY0, cx, bodyY0, 0, 0, 0, 0);
                addQuad(bodyVerts, cx, bodyY0, cx, bodyY0, 0, 0, 0, 0);
            }
        }

        // ── Volume bar (bottom region, semi-transparent) ───────────────────
        const float volFrac = static_cast<float>(c.volume / maxVol);
        const float barH    = std::max(1.0f, volFrac * volBarMaxH);
        const QColor vc     = bullish ? m_bullishColor : m_bearishColor;
        addQuad(volVerts, cx, h - 2.0f - barH, cx + scaledBody, h - 2.0f,
                static_cast<uchar>(vc.red()),
                static_cast<uchar>(vc.green()),
                static_cast<uchar>(vc.blue()),
                static_cast<uchar>(m_volumeBarAlpha));
    }

    root->volNode->markDirty(QSGNode::DirtyGeometry);
    root->wickNode->markDirty(QSGNode::DirtyGeometry);
    root->bodyNode->markDirty(QSGNode::DirtyGeometry);

    emit renderTimeChanged(timer.nsecsElapsed() / 1.0e6);
    return root;
}

void CandlestickBatched::markDirty() { update(); }
