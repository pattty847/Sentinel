/*
Sentinel — CandlestickOverlayItem
Role: GPU-batched candlestick overlay renderer (viewport-locked, no QML churn).
Threading: Update on GUI thread; rendering on render thread.
*/
#pragma once

#include <QQuickItem>
#include <QPointer>
#include <QColor>
#include <vector>
#include <cstdint>
#include <limits>
#include <QtQml/qqmlregistration.h>
#include "TimeAxisMapping.hpp"
#include "ITimeAxisMappingProvider.hpp"
#include "../datasources/CandleSeriesBuffer.hpp"


struct CandleOverlayBar {
    qint64 timeStartMs = 0;
    qint64 timeEndMs = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
};

class CandlestickOverlayItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* candleBuffer READ candleBuffer WRITE setCandleBuffer NOTIFY candleBufferChanged)
    Q_PROPERTY(QObject* mappingProvider READ mappingProvider WRITE setMappingProvider NOTIFY mappingProviderChanged)
    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY symbolChanged)
    Q_PROPERTY(int timeframeSec READ timeframeSec WRITE setTimeframeSec NOTIFY timeframeSecChanged)
    // 0=Candle, 1=Hollow, 2=Line
    Q_PROPERTY(int candleStyle READ candleStyle WRITE setCandleStyle NOTIFY candleStyleChanged)
    Q_PROPERTY(QColor upColor READ upColor WRITE setUpColor NOTIFY appearanceChanged)
    Q_PROPERTY(QColor downColor READ downColor WRITE setDownColor NOTIFY appearanceChanged)
    Q_PROPERTY(QString wickColor READ wickColor WRITE setWickColor NOTIFY appearanceChanged)
    Q_PROPERTY(double bodyOpacity READ bodyOpacity WRITE setBodyOpacity NOTIFY appearanceChanged)
    Q_PROPERTY(int wickWidth READ wickWidth WRITE setWickWidth NOTIFY appearanceChanged)

public:
    explicit CandlestickOverlayItem(QQuickItem* parent = nullptr);

    QObject* candleBuffer() const;
    void setCandleBuffer(QObject* buffer);
    QObject* mappingProvider() const;
    void setMappingProvider(QObject* provider);
    QString symbol() const { return m_symbol; }
    void setSymbol(const QString& symbol);
    int timeframeSec() const { return m_timeframeSec; }
    void setTimeframeSec(int sec);
    int candleStyle() const { return m_candleStyle; }
    void setCandleStyle(int style);
    QColor upColor() const { return m_upColor; }
    QColor downColor() const { return m_downColor; }
    QString wickColor() const { return m_wickColor; }
    double bodyOpacity() const { return m_bodyOpacity; }
    int wickWidth() const { return m_wickWidth; }
    void setUpColor(const QColor& value);
    void setDownColor(const QColor& value);
    void setWickColor(const QString& value);
    void setBodyOpacity(double value);
    void setWickWidth(int value);

signals:
    void candleBufferChanged();
    void mappingProviderChanged();
    void symbolChanged();
    void timeframeSecChanged();
    void candleStyleChanged();
    void appearanceChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private slots:
    void onPanVisualOffsetChanged();

private:
    void connectCandleSignals();
    void disconnectCandleSignals();
    void markGeometryDirty();

    QPointer<CandleSeriesBuffer> m_candleBuffer;
    QPointer<QObject> m_mappingProviderObject;
    ITimeAxisMappingProvider* m_mappingProvider = nullptr;
    QMetaObject::Connection m_mappingViewportConn;
    QMetaObject::Connection m_mappingPanConn;
    QMetaObject::Connection m_mappingTimeframeConn;
    QMetaObject::Connection m_candleDirtyConn;

    uint64_t m_lastCandleGeneration = 0;
    qint64 m_lastBoundarySequence = std::numeric_limits<qint64>::min();
    QSizeF m_lastSize;
    bool m_geometryDirty = true;
    TimeAxisMapping m_lastMapping;

    QString m_symbol;
    int m_timeframeSec = 1;
    int m_candleStyle = 0; // 0=Candle, 1=Hollow, 2=Line
    QColor m_upColor{"#2EBD85"}, m_downColor{"#F6465D"};
    QString m_wickColor = "auto";
    QColor m_customWickColor;
    double m_bodyOpacity = 1;
    int m_wickWidth = 1;
    std::vector<CandleOverlayBar> m_visibleCandles;
    std::vector<CandleOverlayBar> m_filteredCandles;
    std::vector<CandleOverlayBar> m_continuousCandles;
    std::vector<CandleSeriesBuffer::CandleBar> m_bufferSlice;
    struct ClosePoint { float cx, cy; uchar r, g, b, a; };
    std::vector<ClosePoint> m_closePoints;
};
