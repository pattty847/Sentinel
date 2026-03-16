/*
Sentinel — PaperTradeOverlayRenderer
Role: Renderer-backed geometry overlay for manual paper orders and active position state.
      Uses TimeAxisMapping as the single coordinate authority for chart-anchored lines/bands.
Threading: Model snapshots captured on GUI thread; geometry built on render thread.
*/
#pragma once

#include <QQuickItem>
#include <QPointer>
#include <QMetaObject>
#include <QMutex>
#include <QtQml/qqmlregistration.h>

#include "ITimeAxisMappingProvider.hpp"

#include <vector>

class PaperTradeOverlayModel;

class PaperTradeOverlayRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* mappingProvider READ mappingProvider WRITE setMappingProvider NOTIFY mappingProviderChanged)
    Q_PROPERTY(QObject* overlayModel READ overlayModel WRITE setOverlayModel NOTIFY overlayModelChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(qulonglong mappingRevision READ mappingRevision NOTIFY mappingRevisionChanged)

public:
    explicit PaperTradeOverlayRenderer(QQuickItem* parent = nullptr);

    QObject* mappingProvider() const { return m_mappingProviderObject; }
    void setMappingProvider(QObject* provider);

    QObject* overlayModel() const { return m_overlayModelObject; }
    void setOverlayModel(QObject* model);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    qulonglong mappingRevision() const { return m_mappingRevision; }

    Q_INVOKABLE double screenYForPrice(double price) const;

public slots:
    void onMappingChanged();
    void syncFromModel();

signals:
    void mappingProviderChanged();
    void overlayModelChanged();
    void enabledChanged();
    void mappingRevisionChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct OrderSnapshot {
        double price = 0.0;
        bool isBuy = false;
    };

    struct PositionSnapshot {
        bool active = false;
        bool isLong = true;
        double entryPrice = 0.0;
        double markPrice = 0.0;
        double totalPnl = 0.0;
    };

    QPointer<QObject> m_mappingProviderObject;
    ITimeAxisMappingProvider* m_mappingProvider = nullptr;
    std::vector<QMetaObject::Connection> m_mappingConnections;

    QPointer<QObject> m_overlayModelObject;
    PaperTradeOverlayModel* m_overlayModel = nullptr;
    std::vector<QMetaObject::Connection> m_modelConnections;

    bool m_enabled = true;
    bool m_dirty = true;
    qulonglong m_mappingRevision = 1;

    QMutex m_snapshotMutex;
    std::vector<OrderSnapshot> m_orders;
    PositionSnapshot m_position;
};
