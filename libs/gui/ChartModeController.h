/*
Sentinel — ChartModeController
Role: A QML-compatible controller for managing the chart's display mode and UI settings.
Inputs/Outputs: Takes user settings from QML; emits signals when properties change.
Threading: Lives and operates on the main GUI thread.
Performance: All methods are simple setters/getters and are not performance-critical.
Integration: Exposed to QML; its properties control the behavior of UnifiedGridRenderer.
Observability: No internal logging.
Related: ChartModeController.cpp, UnifiedGridRenderer.h, MainWindowGpu.h.
Assumptions: A QML UI will connect to its signals and call its methods to drive changes.
*/
#pragma once
#include <QObject>
#include "ChartMode.h"

class ChartModeController : public QObject {
    Q_OBJECT
public:
    explicit ChartModeController(QObject* parent = nullptr) : QObject(parent) {}

    void setMode(ChartMode mode);
    ChartMode getCurrentMode() const { return m_currentMode; }

signals:
    void modeChanged(ChartMode newMode);
    void componentVisibilityChanged(const QString& component, bool visible);

private:
    ChartMode m_currentMode{ChartMode::TRADE_SCATTER};
    void updateComponentVisibility();
};

