#pragma once
#include <QToolBar>
#include <QLineEdit>
#include <QComboBox>
#include <QSlider>
#include "ChartMode.h"

class TopToolbar : public QToolBar {
    Q_OBJECT
public:
    explicit TopToolbar(QWidget* parent = nullptr);

    QLineEdit* symbolSearch() const { return m_symbolSearch; }
    QSlider* liquiditySlider() const { return m_liquiditySlider; }
    QToolButton* subscribeButton() const { return m_subscribeButton; }
    QComboBox* liquidityModeCombo() const { return m_liquidityModeCombo; }

signals:
    void subscribeRequested();
    void chartModeSelected(ChartMode mode);
    void timeframeSelected(const QString& timeframe);
    void chartTypeSelected(const QString& chartType);
    void indicatorsRequested();
    void layoutsRequested();
    void settingsRequested();
    void quickSearchRequested();
    void fullscreenToggled();
    void screenshotRequested();
    void liquidityThresholdChanged(double threshold);
    void liquidityLabelModeChanged(int mode);

private:
    QAction* addIconAction(const QString& iconPath, const QString& text, const QString& tooltip);
    QToolButton* addIconButton(const QString& iconPath, const QString& tooltip);

    QLineEdit* m_symbolSearch = nullptr;
    QComboBox* m_timeframeCombo = nullptr;
    QComboBox* m_chartTypeCombo = nullptr;
    QSlider* m_liquiditySlider = nullptr;
    QComboBox* m_liquidityModeCombo = nullptr;
    QToolButton* m_subscribeButton = nullptr;
};
