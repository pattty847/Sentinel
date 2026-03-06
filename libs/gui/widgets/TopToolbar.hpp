#pragma once
#include <QToolBar>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSlider>

class TopToolbar : public QToolBar {
    Q_OBJECT
public:
    explicit TopToolbar(QWidget* parent = nullptr);

    QLineEdit* symbolSearch() const { return m_symbolSearch; }
    QSlider* liquiditySlider() const { return m_liquiditySlider; }
    QToolButton* subscribeButton() const { return m_subscribeButton; }
    QComboBox* liquidityModeCombo() const { return m_liquidityModeCombo; }
    void setTimeframeMs(int64_t ms);
    void setLayerToggleStates(bool heatmapEnabled,
                              bool footprintEnabled,
                              bool tpoEnabled,
                              bool volumeProfileEnabled = false);

signals:
    void subscribeRequested();
    void primaryFieldRequested(int field);
    void heatmapToggled(bool enabled);
    void footprintToggled(bool enabled);
    void tpoToggled(bool enabled);
    void volumeProfileToggled(bool enabled);
    void candlesToggled(bool enabled);
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
    QToolButton* m_heatmapButton = nullptr;
    QToolButton* m_footprintButton = nullptr;
    QToolButton* m_tpoButton = nullptr;
    QToolButton* m_volumeProfileButton = nullptr;
};
