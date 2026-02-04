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
#include <QtQml/qqmlregistration.h>
#include "HeatmapTimeMapping.hpp"

class GridViewState;
class CandleSeriesBuffer;
class UnifiedGridRenderer;

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

    Q_PROPERTY(QObject* viewState READ viewState WRITE setViewState NOTIFY viewStateChanged)
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer WRITE setCandleBuffer NOTIFY candleBufferChanged)
    Q_PROPERTY(QObject* heatmapRenderer READ heatmapRenderer WRITE setHeatmapRenderer NOTIFY heatmapRendererChanged)
    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY symbolChanged)
    Q_PROPERTY(int timeframeSec READ timeframeSec WRITE setTimeframeSec NOTIFY timeframeSecChanged)

public:
    explicit CandlestickOverlayItem(QQuickItem* parent = nullptr);

    QObject* viewState() const;
    void setViewState(QObject* viewState);
    QObject* candleBuffer() const;
    void setCandleBuffer(QObject* buffer);
    QObject* heatmapRenderer() const;
    void setHeatmapRenderer(QObject* renderer);
    QString symbol() const { return m_symbol; }
    void setSymbol(const QString& symbol);
    int timeframeSec() const { return m_timeframeSec; }
    void setTimeframeSec(int sec);

signals:
    void viewStateChanged();
    void candleBufferChanged();
    void heatmapRendererChanged();
    void symbolChanged();
    void timeframeSecChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void connectViewStateSignals();
    void disconnectViewStateSignals();
    void connectCandleSignals();
    void disconnectCandleSignals();
    void markGeometryDirty();

    QPointer<GridViewState> m_viewState;
    QPointer<CandleSeriesBuffer> m_candleBuffer;
    QPointer<UnifiedGridRenderer> m_heatmapRenderer;
    QMetaObject::Connection m_viewportChangedConn;
    QMetaObject::Connection m_panChangedConn;
    QMetaObject::Connection m_rendererViewportConn;
    QMetaObject::Connection m_rendererTimeframeConn;
    QMetaObject::Connection m_candleDirtyConn;

    uint64_t m_lastViewportVersion = 0;
    QSizeF m_lastSize;
    bool m_geometryDirty = true;
    HeatmapTimeMapping m_lastMapping;

    QString m_symbol;
    int m_timeframeSec = 1;
    std::vector<CandleOverlayBar> m_visibleCandles;
};
