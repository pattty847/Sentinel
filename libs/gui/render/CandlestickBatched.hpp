// GPU-batched candlestick renderer for QML; updates on GUI thread, renders on render thread.
#pragma once

#include <QColor>
#include <QHash>
#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <vector>

struct CandleData {
    qint64 timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

class CandlestickBatched : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool lodEnabled READ lodEnabled WRITE setLodEnabled NOTIFY lodEnabledChanged)
    Q_PROPERTY(float candleWidth READ candleWidth WRITE setCandleWidth NOTIFY candleWidthChanged)
    Q_PROPERTY(float candleSpacing READ candleSpacing WRITE setCandleSpacing NOTIFY candleSpacingChanged)
    Q_PROPERTY(bool volumeScaling READ volumeScaling WRITE setVolumeScaling NOTIFY volumeScalingChanged)
    Q_PROPERTY(int maxCandles READ maxCandles WRITE setMaxCandles NOTIFY maxCandlesChanged)
    Q_PROPERTY(int hoveredCandle READ hoveredCandle NOTIFY hoveredCandleChanged)
    Q_PROPERTY(int candleCount READ candleCount NOTIFY candleCountChanged)
    // Viewport: offset in pixels (pan) and zoom scale multiplier
    Q_PROPERTY(float viewOffset READ viewOffset WRITE setViewOffset NOTIFY viewOffsetChanged)
    Q_PROPERTY(float zoomScale  READ zoomScale  WRITE setZoomScale  NOTIFY zoomScaleChanged)
    // Visible price range (GUI thread, updated after each viewport change for axis/overlay use)
    Q_PROPERTY(double visiblePriceMin   READ visiblePriceMin   NOTIFY visibleRangeChanged)
    Q_PROPERTY(double visiblePriceMax   READ visiblePriceMax   NOTIFY visibleRangeChanged)
    Q_PROPERTY(int    firstVisibleIndex READ firstVisibleIndex NOTIFY visibleRangeChanged)
    Q_PROPERTY(int    lastVisibleIndex  READ lastVisibleIndex  NOTIFY visibleRangeChanged)
    // Constant fraction of item height reserved for inline volume bars (bottom portion)
    Q_PROPERTY(float volumeHeightFraction READ volumeHeightFraction CONSTANT)

public:
    explicit CandlestickBatched(QQuickItem* parent = nullptr);

    bool lodEnabled() const { return m_lodEnabled; }
    float candleWidth() const { return m_candleWidth; }
    float candleSpacing() const { return m_candleSpacing; }
    bool volumeScaling() const { return m_volumeScaling; }
    int maxCandles() const { return m_maxCandles; }
    int hoveredCandle() const { return m_hoveredCandle; }
    int candleCount() const { return static_cast<int>(m_candles.size()); }
    float viewOffset() const { return m_viewOffset; }
    float zoomScale()  const { return m_zoomScale; }
    double visiblePriceMin()   const { return m_visMin; }
    double visiblePriceMax()   const { return m_visMax; }
    int    firstVisibleIndex() const { return m_firstVis; }
    int    lastVisibleIndex()  const { return m_lastVis; }
    float  volumeHeightFraction() const { return kVolFraction; }

    void setLodEnabled(bool enabled);
    void setCandleWidth(float width);
    void setCandleSpacing(float spacing);
    void setVolumeScaling(bool enabled);
    void setMaxCandles(int maxCandles);
    void setViewOffset(float offset);
    void setZoomScale(float scale);

    // Data management
    Q_INVOKABLE void clearCandles();
    Q_INVOKABLE void setCandles(const QVariantList& candles);
    Q_INVOKABLE void addCandle(qint64 timestamp, double open, double high, double low, double close, double volume);
    Q_INVOKABLE void loadDemoData(int count = 30);
    Q_INVOKABLE QVariantMap getCandleAt(int index) const;

    Q_INVOKABLE void setTimeWindow(qint64 startTimeMs, qint64 endTimeMs);
    Q_INVOKABLE void setAutoLOD(bool enabled);
    Q_INVOKABLE void forceTimeFrame(int timeframeMs);
    Q_INVOKABLE double calculateCurrentPixelsPerCandle() const;

    // SEC signal color overrides: list of {index: int, signalType: int (1=buy, 2=sell, 3=mixed)}
    // Causes those candle bodies to render with the signal color instead of bull/bear color.
    Q_INVOKABLE void setSecSignalOverrides(const QVariantList& overrides);

    // Appearance
    Q_INVOKABLE void setBullishColor(const QColor& color);
    Q_INVOKABLE void setBearishColor(const QColor& color);
    Q_INVOKABLE void setWickColor(const QColor& color);

signals:
    void candleCountChanged(int count);
    void lodLevelChanged(int timeframe);
    void renderTimeChanged(double ms);
    void lodEnabledChanged();
    void candleWidthChanged();
    void candleSpacingChanged();
    void volumeScalingChanged();
    void maxCandlesChanged();
    void hoveredCandleChanged(int index);
    void candleClicked(int index);
    void viewOffsetChanged();
    void zoomScaleChanged();
    // Emitted on GUI thread whenever visible price range or index range changes.
    void visibleRangeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void markDirty();
    int candleAtX(float x) const;
    void updatePriceRange();
    // Recomputes m_firstVis/m_lastVis/m_visMin/m_visMax on the GUI thread and emits
    // visibleRangeChanged so QML price/time axes stay in sync with pan/zoom.
    void recomputeViewport();

    // Fraction of item height used for inline volume bars at the bottom.
    static constexpr float kVolFraction = 0.18f;

    std::vector<CandleData> m_candles;
    double m_priceMin = 0.0;
    double m_priceMax = 1.0;

    // Visible range cache (GUI thread)
    double m_visMin   = 0.0;
    double m_visMax   = 1.0;
    int    m_firstVis = -1;
    int    m_lastVis  = -1;

    // SEC signal color overrides: candle index → signal type (1=buy, 2=sell, 3=mixed)
    QHash<int, int> m_signalOverrides;

    bool m_lodEnabled = true;
    float m_candleWidth = 12.0f;
    float m_candleSpacing = 4.0f;
    bool m_volumeScaling = true;
    int m_maxCandles = 10000;
    int m_hoveredCandle = -1;
    float m_viewOffset = 0.0f;
    float m_zoomScale  = 1.0f;

    QColor m_bullishColor = QColor(47, 221, 122, 220);
    QColor m_bearishColor = QColor(239, 92, 85, 220);
    QColor m_wickColor = QColor(255, 255, 255, 200);
};
