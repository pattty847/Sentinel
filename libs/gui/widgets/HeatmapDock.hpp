#pragma once

#include "DockablePanel.hpp"
#include <QWidget>
#include <QQuickView>
#include <QSurfaceFormat>
#include <QSGRendererInterface>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

/**
 * Dockable widget wrapping the QML heatmap chart.
 * Includes embedded symbol control bar at the bottom that stays attached.
 * Ensures threaded render loop for optimal GPU performance.
 */
class HeatmapDock : public DockablePanel {
    Q_OBJECT

public:
    explicit HeatmapDock(QWidget* parent = nullptr);
    void buildUi() override;
    
    QWidget* qmlContainer() const { return m_qmlContainer; }
    QQuickView* qquickView() const { return m_qquickView; }
    
    // Access to embedded symbol controls
    QLineEdit* symbolInput() const { return m_symbolInput; }
    QPushButton* subscribeButton() const { return m_subscribeButton; }

private:
    QQuickView* m_qquickView = nullptr;
    QWidget* m_qmlContainer = nullptr;
    
    // Embedded symbol control widgets
    QLineEdit* m_symbolInput = nullptr;
    QPushButton* m_subscribeButton = nullptr;
};

