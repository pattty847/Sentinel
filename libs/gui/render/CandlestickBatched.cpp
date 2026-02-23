/*
Sentinel — CandlestickBatched
GPU-batched candlestick renderer with mouse interactions.
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

class CandleRootNode final : public QSGNode {
public:
    CandleRootNode() {
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

} // namespace

CandlestickBatched::CandlestickBatched(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
}

void CandlestickBatched::setLodEnabled(bool enabled) {
    if (m_lodEnabled == enabled) return;
    m_lodEnabled = enabled;
    emit lodEnabledChanged();
}

void CandlestickBatched::setCandleWidth(float width) {
    if (qFuzzyCompare(m_candleWidth, width)) return;
    m_candleWidth = width;
    markDirty();
    emit candleWidthChanged();
}

void CandlestickBatched::setCandleSpacing(float spacing) {
    if (qFuzzyCompare(m_candleSpacing, spacing)) return;
    m_candleSpacing = spacing;
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
    markDirty();
    emit viewOffsetChanged();
}

void CandlestickBatched::setZoomScale(float scale) {
    scale = std::clamp(scale, 0.2f, 10.0f);
    if (qFuzzyCompare(m_zoomScale, scale)) return;
    m_zoomScale = scale;
    markDirty();
    emit zoomScaleChanged();
}

void CandlestickBatched::clearCandles() {
    m_candles.clear();
    m_hoveredCandle = -1;
    emit candleCountChanged(0);
    emit hoveredCandleChanged(-1);
    markDirty();
}

void CandlestickBatched::setCandles(const QVariantList& candles) {
    m_candles.clear();
    m_candles.reserve(candles.size());

    for (const auto& v : candles) {
        QVariantMap m = v.toMap();
        CandleData c;
        c.timestamp = m.value("timestamp", 0).toLongLong();
        c.open = m.value("open", 0.0).toDouble();
        c.high = m.value("high", 0.0).toDouble();
        c.low = m.value("low", 0.0).toDouble();
        c.close = m.value("close", 0.0).toDouble();
        c.volume = m.value("volume", 0.0).toDouble();
        m_candles.push_back(c);
    }

    m_viewOffset = 0.0f;  // reset pan to show most-recent candles on load
    updatePriceRange();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

void CandlestickBatched::addCandle(qint64 timestamp, double open, double high, double low, double close, double volume) {
    CandleData c;
    c.timestamp = timestamp;
    c.open = open;
    c.high = high;
    c.low = low;
    c.close = close;
    c.volume = volume;
    m_candles.push_back(c);

    updatePriceRange();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

void CandlestickBatched::loadDemoData(int count) {
    m_candles.clear();
    m_candles.reserve(count);

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_real_distribution<double> priceDist(-2.0, 2.0);
    std::uniform_real_distribution<double> volDist(1000.0, 10000.0);

    double price = 100.0;
    qint64 ts = 1700000000000LL;  // Some base timestamp

    for (int i = 0; i < count; ++i) {
        CandleData c;
        c.timestamp = ts + i * 86400000LL;  // Daily candles

        double change = priceDist(rng);
        double volatility = std::abs(priceDist(rng)) + 0.5;

        c.open = price;
        c.close = price + change;
        c.high = std::max(c.open, c.close) + volatility;
        c.low = std::min(c.open, c.close) - volatility;
        c.volume = volDist(rng);

        price = c.close;
        m_candles.push_back(c);
    }

    updatePriceRange();
    emit candleCountChanged(static_cast<int>(m_candles.size()));
    markDirty();
}

QVariantMap CandlestickBatched::getCandleAt(int index) const {
    QVariantMap result;
    if (index >= 0 && index < static_cast<int>(m_candles.size())) {
        const auto& c = m_candles[index];
        result["timestamp"] = c.timestamp;
        result["open"] = c.open;
        result["high"] = c.high;
        result["low"] = c.low;
        result["close"] = c.close;
        result["volume"] = c.volume;
        result["bullish"] = c.close >= c.open;
    }
    return result;
}

void CandlestickBatched::setTimeWindow(qint64, qint64) {
    markDirty();
}

void CandlestickBatched::setAutoLOD(bool enabled) {
    setLodEnabled(enabled);
}

void CandlestickBatched::forceTimeFrame(int timeframeMs) {
    emit lodLevelChanged(timeframeMs);
}

double CandlestickBatched::calculateCurrentPixelsPerCandle() const {
    return std::max(1.0f, m_candleWidth + m_candleSpacing);
}

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
    // Rightmost candle's right edge sits at chartWidth; pan shifts everything left
    const float startX = chartWidth - static_cast<float>(m_candles.size()) * totalWidth + m_viewOffset;

    const float xInChart = x - startX;
    if (xInChart < 0.0f) return -1;

    int index = static_cast<int>(xInChart / totalWidth);
    if (index < 0 || index >= static_cast<int>(m_candles.size())) return -1;

    const float candleStartX = startX + static_cast<float>(index) * totalWidth;
    const float candleEndX   = candleStartX + m_candleWidth * m_zoomScale;
    if (x >= candleStartX && x <= candleEndX)
        return index;

    return -1;
}

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
    if (index >= 0) {
        emit candleClicked(index);
    }
}

QSGNode* CandlestickBatched::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QElapsedTimer timer;
    timer.start();

    auto* root = static_cast<CandleRootNode*>(oldNode);
    if (!root) {
        root = new CandleRootNode();
    }

    const float w = static_cast<float>(width());
    const float h = static_cast<float>(height());
    if (w <= 0.0f || h <= 0.0f || m_candles.empty()) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const int n = static_cast<int>(m_candles.size());
    const float totalWidth  = (m_candleWidth + m_candleSpacing) * m_zoomScale;
    const float scaledBody  = m_candleWidth * m_zoomScale;
    // Anchor right: most recent candle flush with right edge, pan shifts left
    const float startX = w - static_cast<float>(n) * totalWidth + m_viewOffset;

    // Visible range — only allocate and draw candles on screen
    const int firstVisible = std::max(0, static_cast<int>(std::floor((-startX) / totalWidth)));
    const int lastVisible  = std::min(n - 1, static_cast<int>(std::floor((w - startX) / totalWidth)));
    const int visCount     = std::max(0, lastVisible - firstVisible + 1);

    // If no candles are visible (fully panned into empty space), nothing to draw
    if (visCount == 0) {
        root->wickGeometry->allocate(0);
        root->bodyGeometry->allocate(0);
        root->wickNode->markDirty(QSGNode::DirtyGeometry);
        root->bodyNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    // Price range over visible candles only (feels more natural while panning)
    double visMin = m_candles[firstVisible].low;
    double visMax = m_candles[firstVisible].high;
    for (int i = firstVisible; i <= lastVisible; ++i) {
        visMin = std::min(visMin, m_candles[i].low);
        visMax = std::max(visMax, m_candles[i].high);
    }
    double visRange = visMax - visMin;
    if (visRange < 0.01) visRange = 1.0;
    const double padFrac = 0.05;
    visMin -= visRange * padFrac;
    visMax += visRange * padFrac;
    visRange = visMax - visMin;

    const float padding     = h * 0.05f;
    const float chartHeight = h - 2.0f * padding;

    auto priceToY = [&](double price) -> float {
        double normalized = (price - visMin) / visRange;
        return padding + static_cast<float>(1.0 - normalized) * chartHeight;
    };

    root->wickGeometry->allocate(visCount * 6);
    root->bodyGeometry->allocate(visCount * 6);

    auto* wickVerts = root->wickGeometry->vertexDataAsColoredPoint2D();
    auto* bodyVerts = root->bodyGeometry->vertexDataAsColoredPoint2D();

    const uchar wr = static_cast<uchar>(m_wickColor.red());
    const uchar wg = static_cast<uchar>(m_wickColor.green());
    const uchar wb = static_cast<uchar>(m_wickColor.blue());
    const uchar wa = static_cast<uchar>(m_wickColor.alpha());

    for (int i = firstVisible; i <= lastVisible; ++i) {
        const auto& c = m_candles[i];
        const bool bullish = c.close >= c.open;
        const bool hovered = (i == m_hoveredCandle);

        const float candleX       = startX + static_cast<float>(i) * totalWidth;
        const float candleCenterX = candleX + scaledBody * 0.5f;

        const float wickWidth = std::max(1.0f, scaledBody * 0.15f);
        const float wickX0    = candleCenterX - wickWidth * 0.5f;
        const float wickX1    = candleCenterX + wickWidth * 0.5f;

        addQuad(wickVerts, wickX0, priceToY(c.high), wickX1, priceToY(c.low), wr, wg, wb, wa);

        const float bodyY0 = priceToY(std::max(c.open, c.close));
        const float bodyY1 = priceToY(std::min(c.open, c.close));

        QColor bodyColor = bullish ? m_bullishColor : m_bearishColor;
        if (hovered) bodyColor = bodyColor.lighter(130);

        addQuad(bodyVerts,
                candleX,          bodyY0,
                candleX + scaledBody, bodyY1,
                static_cast<uchar>(bodyColor.red()),
                static_cast<uchar>(bodyColor.green()),
                static_cast<uchar>(bodyColor.blue()),
                static_cast<uchar>(bodyColor.alpha()));
    }

    root->wickNode->markDirty(QSGNode::DirtyGeometry);
    root->bodyNode->markDirty(QSGNode::DirtyGeometry);

    emit renderTimeChanged(timer.nsecsElapsed() / 1.0e6);
    return root;
}

void CandlestickBatched::markDirty() {
    update();
}
