/*
Sentinel — LabDock
Role: Experimental dock for rapid visual/prototype work.
*/
#pragma once

#include "DockablePanel.hpp"
#include <QWidget>
#include <QQuickView>
#include <QSurfaceFormat>
#include <QSGRendererInterface>

class LabDock : public DockablePanel {
    Q_OBJECT

public:
    explicit LabDock(QWidget* parent = nullptr);
    void buildUi() override;
    QSize minimumSizeHint() const override;

    QWidget* qmlContainer() const { return m_qmlContainer; }
    QQuickView* qquickView() const { return m_qquickView; }

private:
    QQuickView* m_qquickView = nullptr;
    QWidget* m_qmlContainer = nullptr;
};
