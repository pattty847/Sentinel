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

class AlgoOverlayRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* mappingProvider READ mappingProvider WRITE setMappingProvider NOTIFY mappingProviderChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    static constexpr int kMaxEvents = 2000; // rolling buffer size

    explicit AlgoOverlayRenderer(QQuickItem* parent = nullptr);

    QObject* mappingProvider() const { return m_mappingProvider; }
    void setMappingProvider(QObject* provider);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool v);

public slots:
    void onAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    void onMappingChanged();

signals:
    void mappingProviderChanged();
    void enabledChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct OrderSpan {
        trading::OrderSide side = trading::OrderSide::Unknown;
        double price = 0.0;
        int64_t openedMs = 0;
        int64_t closedMs = 0;
        trading::OrderStatus terminalStatus = trading::OrderStatus::New;
        bool active = false;
    };

    QPointer<QObject> m_mappingProvider;
    std::vector<QMetaObject::Connection> m_mappingConnections;
    bool m_enabled = true;
    bool m_dirty = false;

    QMutex m_pendingMutex;
    std::vector<trading::AlgoOrderEvent> m_pending; // GUI thread -> render thread
    std::unordered_map<std::string, OrderSpan> m_orderSpans; // render-thread only
    std::deque<trading::AlgoOrderEvent> m_markers; // render-thread only
};
