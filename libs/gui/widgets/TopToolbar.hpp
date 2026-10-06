#pragma once
#include <QToolBar>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <QMenu>
#include <QHash>
#include <QVector>
#include <QPointer>
#include "LiquidityRangeSlider.hpp"
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
    // Fills a preset combo (the toolbar's or the settings dialog's): only offered
    // presets are selectable; a shown value no loaded data builds (a locked Manual
    // tick) is listed as the current item, marked unavailable and disabled; with
    // nothing to list the combo shows `emptyText` and is disabled.
    static void fillTickPresetCombo(QComboBox* combo, const TickSelectorState& state, const QString& emptyText);
    void setLayerToggleStates(bool heatmapEnabled,
                              bool footprintEnabled,
                              bool tpoEnabled,
                              bool volumeProfileEnabled = false);

    // The toolbar adapts to the chart's active layers (owner request 2026-10-02).
    // controlVisibility() is the ONE place the rules live:
    // - heatmap-only: tick and range (GPU) or threshold (legacy);
    // - palette and label appearance: the gear menu, leaving the primary strip
    //   for navigation and data controls;
    // - candle style: candles are on;
    // - TPO session: TPO or volume profile is on (the profile follows the TPO
    //   session); TPO layout: TPO is on.
    struct ModeState {
        bool heatmap = true, footprint = false, tpo = false, volumeProfile = false, candles = true, gpu = true;
        bool operator==(const ModeState &) const = default;
    };
    struct ControlVisibility {
        bool tickSelector = false, palette = false, liquidity = false, rangeSlider = false, thresholdSlider = false;
        bool candleStyle = false, tpoSession = false, tpoLayout = false;
        bool labelsToggle = false; // gpu heatmap only (legacy draws its own labels, always)
        bool operator==(const ControlVisibility &) const = default;
    };
    static ControlVisibility controlVisibility(const ModeState &mode);
    void setModeState(const ModeState &mode);
    const ModeState &modeState() const { return m_mode; }
    // Controls available for the active mode, including those moved into overflow.
    ControlVisibility shownControls() const;
    bool candlesChecked() const;
    void setCandlesChecked(bool checked); // no signal

    // Liquidity labels and the range filter (S7b). The model drives them; user
    // edits come back as signals. Values are base-asset sizes.
    LiquidityRangeSlider *rangeSlider() const { return m_rangeSlider; }
    QLabel *rangeLabel() const { return m_rangeLabel; }
    QToolButton *labelsButton() const { return m_labelsButton; }
    void setLiquidityRange(double domainLo, double domainHi, double low, double high);
    void setLabelOptions(bool show, bool usd);

    // TPO controls (renderer state): session type id and layout ("collapsed", "split").
    QComboBox *tpoSessionCombo() const { return m_tpoSessionCombo; }
    QComboBox *tpoLayoutCombo() const { return m_tpoLayoutCombo; }
    void setTpoState(int sessionType, const QString &layout);

    // The chart settings menu (gear): one entry point for chart-level settings.
    QToolButton *chartMenuButton() const { return m_chartMenuButton; }
    QMenu *chartMenu() const { return m_chartMenu; }
    QToolButton *controlsButton() const { return m_controlsButton; }
    // Full labelled keyboard menu; the visible extension uses overflowMenu().
    QMenu *controlsMenu() const { return m_controlsMenu; }
    QMenu *overflowMenu() const { return m_overflowMenu; }
    QComboBox *chartTypeCombo() const { return m_chartTypeCombo; }
    void setBaseAssetSymbol(const QString &symbol);
    void showLayoutsMenu();
    void showControlsMenu();
    void setFullscreen(bool fullscreen);

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
    void liquidityRangeSettingsRequested();
    void quickSearchRequested();
    void fullscreenToggled();
    void screenshotRequested();
    void liquidityThresholdChanged(double threshold);
    void liquidityLabelModeChanged(int mode);
    void colorPresetSelected(const QString& preset);
    void tickModeRequested(bool manual);    // user changed Auto/Manual
    void tickPresetRequested(qint64 units); // user picked a preset (locks Manual)
    void liquidityRangeEdited(double low, double high, bool final);
    void labelsToggled(bool show);
    void tpoSessionSelected(int sessionType);
    void tpoLayoutSelected(const QString &layout);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scheduleFit();
    void fitControls();
    void refreshOverflowMenu();
    void refreshGearCommands();
    void prepareControlsMenu(QMenu *menu);
    void prepareControlsEditor(QWidget *editor);
    void setInlineVisible(QAction *action, bool visible);
    QAction* addIconAction(const QString& iconPath, const QString& text, const QString& tooltip);
    QToolButton* addIconButton(const QString& iconPath, const QString& tooltip);
    void applyVisibility();
    void buildControlsMenu();
    void refreshRangeLabel();
    void refreshControlsMenu();

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
    QAction* m_candleAction = nullptr;
    LiquidityRangeSlider* m_rangeSlider = nullptr;
    QLabel* m_rangeLabel = nullptr;
    QToolButton* m_labelsButton = nullptr;
    QComboBox* m_tpoSessionCombo = nullptr;
    QComboBox* m_tpoLayoutCombo = nullptr;
    QToolButton* m_chartMenuButton = nullptr;
    QMenu* m_chartMenu = nullptr;
    QToolButton* m_controlsButton = nullptr;
    QMenu* m_controlsMenu = nullptr;
    QMenu* m_overflowMenu = nullptr;
    QPointer<LiquidityRangeSlider> m_overflowRangeSlider;
    QPointer<QLabel> m_overflowRangeLabel;
    bool m_overflowMenuDirty = false;
    QMenu* m_tickMenu = nullptr;
    QMenu* m_labelsMenu = nullptr;
    QMenu* m_legacyThresholdMenu = nullptr;
    QMenu* m_layoutsMenu = nullptr;
    QAction* m_quickSearchAction = nullptr;
    QAction* m_screenshotAction = nullptr;
    QAction* m_overflowAction = nullptr;
    QAction* m_spacerAction = nullptr;
    QAction* m_gearAction = nullptr;
    QList<QAction*> m_inlineActions;
    QVector<QList<QAction*>> m_inlineGroups;
    QHash<QAction*, bool> m_modeVisibility;
    QVector<int> m_fitInputs;
    bool m_fitPending = false;
    bool m_fitting = false;
    bool m_gearRefreshPending = false;
    bool m_dispatchingEditorKey = false;
    QAction* m_rangeMenuAction = nullptr;
    QAction* m_indicatorsAction = nullptr;
    QAction* m_layoutsAction = nullptr;
    QAction* m_fullscreenAction = nullptr;
    QString m_baseAssetSymbol = "BTC";
    ModeState m_mode;
    // Toolbar actions of the widgets above (a toolbar widget hides with its action).
    QAction *m_tickLabelAction = nullptr, *m_tickModeAction = nullptr, *m_tickPresetAction = nullptr, *m_tickVeilAction = nullptr;
    QAction *m_chartTypeAction = nullptr, *m_paletteAction = nullptr;
    QAction *m_liqLabelAction = nullptr, *m_modeLabelAction = nullptr, *m_modeComboAction = nullptr;
    QAction *m_labelsAction = nullptr, *m_rangeAction = nullptr, *m_rangeLabelAction = nullptr;
    QAction *m_thresholdAction = nullptr, *m_tpoSessionAction = nullptr, *m_tpoLayoutAction = nullptr;
};
