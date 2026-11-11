/*
Sentinel — AlgoOverlayRenderer
Role: GPU QSG renderer for algo order annotations on the heatmap/candles chart.
       Shows resting limit order lines and fill markers in price/time space.
Threading: Slot called on GUI thread; updatePaintNode on render thread.
*/
#pragma once

#include <QQuickItem>
#include <QPointer>
#include <QMetaObject>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QColor>
#include <QMutex>
#include <vector>
#include <deque>
#include <unordered_map>
#include <cstdint>
#include <QtQml/qqmlregistration.h>

#include "ITimeAxisMappingProvider.hpp"
#include "../../core/trading/AlgoEngine.hpp"
#include "AlgoOrderSpans.hpp"

#include <variant>

class AlgoOverlayRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* mappingProvider READ mappingProvider WRITE setMappingProvider NOTIFY mappingProviderChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)

public:
    explicit AlgoOverlayRenderer(QQuickItem* parent = nullptr);

    QObject* mappingProvider() const { return m_mappingProvider; }
    void setMappingProvider(QObject* provider);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool v);

public slots:
    void onAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    // Closes lines of orders the server no longer holds (reconnect, server restart).
    void onTradingSnapshot(const trading::TradingSnapshot& snapshot);
    void onMappingChanged();

signals:
    void mappingProviderChanged();
    void enabledChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct Reconcile {
        trading::TradingSnapshot snapshot;
        int64_t atMs = 0;
    };

    QPointer<QObject> m_mappingProvider;
    std::vector<QMetaObject::Connection> m_mappingConnections;
    bool m_enabled = true;
    bool m_dirty = false;

    QMutex m_pendingMutex;
    std::vector<std::variant<trading::AlgoOrderEvent, Reconcile>> m_pending; // GUI thread -> render thread
    AlgoOrderSpans m_spans; // render-thread only
};
