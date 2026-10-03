#pragma once
// Chart settings (S6c, S7b): every HeatmapChartSettings field in six tabs
// (Chart, Tick, Look, Budgets, Live, Debug) plus the chart's TPO controls. The
// Chart tab holds the liquidity labels and the candle style. Changes apply
// live through HeatmapSettingsModel::apply (persisted per chart; the Debug
// renderer only for this session unless "Make default" is ticked) and the dialog
// follows the model's changed() signal, so Agent API and toolbar changes show here.
// Widgets carry objectNames equal to the setting keys (tests and inspection).
#include "heatmap/HeatmapChartSettings.hpp"
#include "config/ConfigTypes.hpp"
#include "TopToolbar.hpp"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <vector>

class UnifiedGridRenderer;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTableWidget;
namespace heatmap { class HeatmapSettingsModel; }

// Edits one gradient: ordered stops (position 0..1, colour). Emits edited() on a
// user change; stops() may be invalid (the model's S6a validation decides).
class HeatmapGradientEditor : public QWidget {
    Q_OBJECT
public:
    explicit HeatmapGradientEditor(const QString &objectName, QWidget *parent = nullptr);
    void setStops(const std::vector<heatmap::GradientStop> &stops);
    std::vector<heatmap::GradientStop> stops() const;
    QTableWidget *table() const { return table_; }
    void addStop();
    void removeStop();
    void setStopColor(int row, const QString &color); // as the colour picker does
signals:
    void edited();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void addRow(double position, const QString &color);
    QTableWidget *table_ = nullptr;
    QPushButton *add_ = nullptr, *remove_ = nullptr;
    bool loading_ = false;
};

class HeatmapSettingsDialog : public QDialog {
    Q_OBJECT
public:
    HeatmapSettingsDialog(heatmap::HeatmapSettingsModel *model, UnifiedGridRenderer *renderer,
                          QWidget *parent = nullptr);
    void setRenderer(UnifiedGridRenderer *renderer);
    // The chart's tick state, as the toolbar shows it (HeatmapChartControls): the
    // preset combo lists only the offered presets; a locked Manual tick no loaded
    // data builds shows as the current value, marked unavailable.
    void setTickSelectorState(const TopToolbar::TickSelectorState &state);
    // The TPO tab's Reset (the configured tpo.* values).
    void setTpoDefaults(const ClientTpoConfig &defaults) { m_tpoDefaults = defaults; }
    void refreshFromModel();
    void refreshFromRenderer();
    QTabWidget *tabs() const { return m_tabs; }
    QString statusText() const;
    // The settings keys each tab owns (its Reset button restores them).
    static QStringList tabKeys(const QString &tab);

signals:
    // Tick actions go through the chart's controls, as the toolbar's do (a preset
    // locks Manual and is remembered; entering Manual restores the remembered
    // tick or locks the drawn one).
    void tickModeRequested(bool manual);
    void tickPresetRequested(qint64 units);

private:
    void buildUi();
    QWidget *buildChartTab();
    QWidget *buildTickTab();
    QWidget *buildLookTab();
    QWidget *buildBudgetsTab();
    QWidget *buildLiveTab();
    QWidget *buildDebugTab();
    QWidget *buildTpoTab();
    QPushButton *resetButton(const QString &tab, QWidget *parent);
    void apply(const QJsonObject &patch, bool persist = true);
    void applyBudgets();
    void showStatus(const QString &text, bool error);
    void logSettings() const;

    heatmap::HeatmapSettingsModel *m_model = nullptr;
    QPointer<UnifiedGridRenderer> m_renderer;
    bool m_loading = false;
    TopToolbar::TickSelectorState m_tickState;
    ClientTpoConfig m_tpoDefaults;

    QTabWidget *m_tabs = nullptr;
    QLabel *m_status = nullptr;
    // Chart (S7b labels: chart settings; candle style: renderer state)
    QCheckBox *m_showLabels = nullptr;
    QComboBox *m_labelCurrency = nullptr;
    QDoubleSpinBox *m_labelMinPx = nullptr;
    QDoubleSpinBox *m_labelMaxPx = nullptr;
    QComboBox *m_candleStyle = nullptr;
    QPushButton *m_candleUpColor = nullptr;
    QPushButton *m_candleDownColor = nullptr;
    QPushButton *m_candleWickColor = nullptr;
    QPushButton *m_candleWickAuto = nullptr;
    QSpinBox *m_candleBodyOpacity = nullptr;
    QSpinBox *m_candleWickWidth = nullptr;
    QWidget *m_candlePreview = nullptr;
    // Tick
    QComboBox *m_tickMode = nullptr;
    QComboBox *m_manualTick = nullptr;
    QDoubleSpinBox *m_minRowPx = nullptr;
    QDoubleSpinBox *m_hysteresis = nullptr;
    // Look
    QComboBox *m_palette = nullptr;
    HeatmapGradientEditor *m_bidGradient = nullptr;
    HeatmapGradientEditor *m_askGradient = nullptr;
    QDoubleSpinBox *m_sensitivityMin = nullptr;
    QDoubleSpinBox *m_sensitivityMax = nullptr;
    QDoubleSpinBox *m_opacity = nullptr;
    QCheckBox *m_crossfadeOn = nullptr;
    QSpinBox *m_crossfadeMs = nullptr;
    QCheckBox *m_bandEdges = nullptr;
    QSlider *m_gammaSlider = nullptr;
    QLabel *m_gammaLabel = nullptr;
    QSlider *m_contrastSlider = nullptr;
    QLabel *m_contrastLabel = nullptr;
    QSlider *m_floorSlider = nullptr;
    QLabel *m_floorLabel = nullptr;
    // Budgets
    QSpinBox *m_gpuCapMiB = nullptr;
    QSpinBox *m_uploadKiB = nullptr;
    QSpinBox *m_prefetchTiles = nullptr;
    QSpinBox *m_decodedMiB = nullptr;
    QSpinBox *m_spanMiB = nullptr;
    QSpinBox *m_ceilingMiB = nullptr;
    // Live
    QSpinBox *m_liveMinInterval = nullptr;
    // Debug
    QComboBox *m_rendererCombo = nullptr;
    QCheckBox *m_makeDefault = nullptr;
    QLabel *m_savedRenderer = nullptr;
    QCheckBox *m_showTelemetry = nullptr;
    // TPO (renderer state, not chart settings)
    QComboBox *m_tpoTimeframeCombo = nullptr;
    QComboBox *m_tpoSessionCombo = nullptr;
    QComboBox *m_tpoLayoutCombo = nullptr;
    QComboBox *m_tpoThemeCombo = nullptr;
};
