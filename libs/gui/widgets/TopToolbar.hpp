#pragma once
#include <QToolBar>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <cstdint>
#include <vector>

class TopToolbar : public QToolBar {
    Q_OBJECT
public:
    explicit TopToolbar(QWidget* parent = nullptr);

    QLineEdit* symbolSearch() const { return m_symbolSearch; }
    QSlider* liquiditySlider() const { return m_liquiditySlider; }
    QToolButton* subscribeButton() const { return m_subscribeButton; }
    QComboBox* liquidityModeCombo() const { return m_liquidityModeCombo; }
    void setTimeframeMs(int64_t ms);
    // Shows the chart's palette preset without emitting colorPresetSelected.
    void setColorPreset(const QString& preset);
    void setAvailableTimeframes(const std::vector<int64_t>& servedTimeframesMs);
    // The heatmap tick selector beside the timeframe combo (S6c, spec rules 2 and 4).
    // Auto shows the drawn tick, Manual the locked preset; the preset list is what
    // some loaded data can build (rule 4 option B). Disabled (with reason) when the
    // chart draws with the legacy renderer.
    struct TickSelectorState {
        bool enabled = false;
        QString disabledReason;
        bool manual = false;
        int64_t drawnUnits = 0;           // the tick drawn now (0: none yet)
        int64_t manualUnits = 0;          // the locked Manual preset
        std::vector<int64_t> offeredUnits; // presets some loaded data can build
        double priceScale = 100;           // price units per price
        QString indicator;                 // veiled availability (empty: none)
        bool operator==(const TickSelectorState &) const = default;
    };
    void setTickSelectorState(const TickSelectorState& state);
    const TickSelectorState& tickSelectorState() const { return m_tickState; }
    QComboBox* tickModeCombo() const { return m_tickModeCombo; }
    QComboBox* tickPresetCombo() const { return m_tickPresetCombo; }
    QLabel* tickVeilLabel() const { return m_tickVeilLabel; }
    static QString tickText(int64_t units, double priceScale);
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
    void colorPresetSelected(const QString& preset);
    void tickModeRequested(bool manual);    // user changed Auto/Manual
    void tickPresetRequested(qint64 units); // user picked a preset (locks Manual)

private:
    QAction* addIconAction(const QString& iconPath, const QString& text, const QString& tooltip);
    QToolButton* addIconButton(const QString& iconPath, const QString& tooltip);

    QLineEdit* m_symbolSearch = nullptr;
    QComboBox* m_timeframeCombo = nullptr;
    QComboBox* m_chartTypeCombo = nullptr;
    QComboBox* m_colorPresetCombo = nullptr;
    QComboBox* m_tickModeCombo = nullptr;
    QComboBox* m_tickPresetCombo = nullptr;
    QLabel* m_tickVeilLabel = nullptr;
    TickSelectorState m_tickState;
    bool m_tickStateSet = false;
    QSlider* m_liquiditySlider = nullptr;
    QComboBox* m_liquidityModeCombo = nullptr;
    QToolButton* m_subscribeButton = nullptr;
    QToolButton* m_heatmapButton = nullptr;
    QToolButton* m_footprintButton = nullptr;
    QToolButton* m_tpoButton = nullptr;
    QToolButton* m_volumeProfileButton = nullptr;
};
