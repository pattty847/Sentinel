#pragma once
// Draws the TPO overlay above the candle overlay item, so the profile covers
// candles (and they show through its slight transparency) whatever other layers
// are on: heatmap/footprint (UGR) < candles < TPO. The TPO renderer and its
// data stay owned by the chart (its `tpo` sub-object); this item only hosts the
// scene-graph nodes and renders them with the chart's published frame camera.
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
#include <atomic>

class ITimeAxisMappingProvider;
class TpoOverlayRenderer;
namespace chart { class ChartTpoQml; class LayerVisibility; }

class TpoOverlayItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // The UnifiedGridRenderer: its mapping provider, `tpo` and `layers` objects.
    Q_PROPERTY(QObject* chart READ chart WRITE setChart NOTIFY chartChanged)
public:
    explicit TpoOverlayItem(QQuickItem* parent = nullptr);
    ~TpoOverlayItem() override;
    QObject* chart() const { return m_chart.data(); }
    void setChart(QObject* chart);
    TpoOverlayRenderer* rendererForTest() const { return m_renderer; }
signals:
    void chartChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
private:
    void unbind();
    void readLayers();
    QPointer<QObject> m_chart;
    ITimeAxisMappingProvider* m_mapping = nullptr;
    chart::ChartTpoQml* m_tpo = nullptr;
    chart::LayerVisibility* m_layers = nullptr;
    TpoOverlayRenderer* m_renderer = nullptr;
    bool m_enabled = false;
    std::atomic_bool m_retryQueued = false;
    QList<QMetaObject::Connection> m_connections;
};
