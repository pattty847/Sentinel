#pragma once
#include <QObject>
#include "chartmode.h"

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

