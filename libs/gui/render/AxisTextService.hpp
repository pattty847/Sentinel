#pragma once

#include <QObject>
#include <QPointer>
#include <QMetaObject>
#include <mutex>
#include <vector>

#include "AxisLayout.hpp"
#include "ChartTextAtlas.hpp"
#include "ChartTextRenderer.hpp"
#include "models/AxisModel.hpp"
class QQuickWindow;
class QScreen;

class AxisTextService : public QObject {
    Q_OBJECT

public:
    struct AxisTickSnapshot {
        double position = 0.0;
        QString label;
        bool isMajorTick = false;
    };

    struct AxisLayoutSnapshot {
        float axisLabelPx = static_cast<float>(AxisLayout::kAutoAxisLabelBasePx);
        float axisScale = 1.0f;
        int priceAxisWidthPx = 90;
        int timeAxisHeightPx = 30;
    };

    explicit AxisTextService(const ChartTextAtlas& atlas, QObject* parent = nullptr);

    void setPriceAxisSource(QObject* source);
    void setTimeAxisSource(QObject* source);

    void setAxisLabelPxOverride(int px);
    void refreshAxisLayout();
    void bindAxisLayoutWindow(QQuickWindow* window);

    /// Called on the render thread between beginFrame/endFrame.
    void submitAxisText(ChartTextRenderer& renderer,
                        const ChartTextAtlas& atlas,
                        qreal canvasWidth,
                        qreal canvasHeight);

    QObject* priceAxisSource() const { return m_priceAxisSource; }
    QObject* timeAxisSource() const { return m_timeAxisSource; }
    double effectiveAxisLabelPx() const { return m_effectiveAxisLabelPx; }
    int priceAxisWidthPx() const { return m_priceAxisWidthPx; }
    int timeAxisHeightPx() const { return m_timeAxisHeightPx; }

    AxisModel* priceAxisModel() const { return m_priceAxisSource; }
    AxisModel* timeAxisModel() const { return m_timeAxisSource; }

signals:
    void axisSourcesChanged();
    void layoutChanged();
    void needsUpdate();

private:
    void syncAxisTicks(AxisModel* currentModel,
                       std::vector<QMetaObject::Connection>& connections,
                       std::vector<AxisTickSnapshot>& storage,
                       AxisModel*& target,
                       QObject* source);
    void refreshAxisTickSnapshot(AxisModel* model,
                                 std::vector<AxisTickSnapshot>& storage);

    const ChartTextAtlas& m_atlas;

    AxisModel* m_priceAxisSource = nullptr;
    AxisModel* m_timeAxisSource = nullptr;
    std::vector<QMetaObject::Connection> m_priceAxisConnections;
    std::vector<QMetaObject::Connection> m_timeAxisConnections;
    mutable std::mutex m_axisSnapshotMutex;
    std::vector<AxisTickSnapshot> m_priceAxisTicks;
    std::vector<AxisTickSnapshot> m_timeAxisTicks;
    mutable std::mutex m_axisLayoutMutex;
    AxisLayoutSnapshot m_axisLayoutSnapshot;
    double m_effectiveAxisLabelPx = AxisLayout::kAutoAxisLabelBasePx;
    int m_priceAxisWidthPx = 90;
    int m_timeAxisHeightPx = 30;
    int m_axisLabelPxOverride = 0;
    QPointer<QScreen> m_axisLayoutWindowScreen;
    QMetaObject::Connection m_axisLayoutWindowConnection;
    QMetaObject::Connection m_axisLayoutScreenConnection;
};
