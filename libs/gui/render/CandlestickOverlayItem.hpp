/*
Sentinel — CandlestickOverlayItem
Role: GPU-batched candlestick overlay renderer (viewport-locked, no QML churn).
Threading: Update on GUI thread; rendering on render thread.
*/
#pragma once

#include <QQuickItem>
#include <QPointer>
#include <vector>
#include <cstdint>
#include <limits>
#include <QtQml/qqmlregistration.h>
#include "TimeAxisMapping.hpp"
#include "ITimeAxisMappingProvider.hpp"

class CandleSeriesBuffer;

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

signals:
    void candleBufferChanged();
    void mappingProviderChanged();
    void symbolChanged();
    void timeframeSecChanged();

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
    std::vector<CandleOverlayBar> m_visibleCandles;
};
