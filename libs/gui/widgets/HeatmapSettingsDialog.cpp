#include "HeatmapSettingsDialog.hpp"
#include "../UnifiedGridRenderer.h"
#include "../render/heatmap/HeatmapSettingsModel.hpp"
#include "SentinelLogging.hpp"
#include "TopToolbar.hpp"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr uint64_t MiB = 1ull << 20, KiB = 1ull << 10;
const QStringList kPalettes{"Electric", "Fire", "Ocean", "Monochrome", "Matrix", "Custom"};

struct SliderWithLabel {
    QSlider *slider;
    QLabel *label;
};
SliderWithLabel makeSlider(QWidget *parent, int minVal, int maxVal, int defaultVal) {
    auto *slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minVal, maxVal);
    slider->setValue(defaultVal);
    slider->setMinimumWidth(200);
    slider->setStyleSheet(
        "QSlider::groove:horizontal { background: #252A31; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #2B5A7A; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; }"
        "QSlider::handle:horizontal:hover { background: #3472A0; }");
    auto *label = new QLabel(parent);
    label->setMinimumWidth(50);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setStyleSheet("QLabel { color: #E6EDF3; font-weight: 600; }");
    return {slider, label};
}
QDoubleSpinBox *doubleSpin(QWidget *parent, const char *name, double lo, double hi, double step, int decimals) {
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(lo, hi);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false); // apply on Enter, focus-out or a step, not per keystroke
    return spin;
}
QSpinBox *intSpin(QWidget *parent, const char *name, int lo, int hi, int step, const QString &suffix) {
    auto *spin = new QSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(lo, hi);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}
QLabel *note(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet("QLabel { color: #AABBC6; }");
    return label;
}
// A setting's name and control explain the same thing on hover.
void describe(QFormLayout *form, QWidget *field, const QString &text) {
    field->setToolTip(text);
    if (auto *label = form->labelForField(field)) label->setToolTip(text);
}
void describe(QFormLayout *form, QLayout *field, const QString &text) {
    if (auto *label = form->labelForField(field)) label->setToolTip(text);
    for (int i = 0; i < field->count(); ++i)
        if (auto *widget = field->itemAt(i)->widget()) widget->setToolTip(text);
}
QScrollArea *scrollTab(QWidget *content, const char *name) {
    auto *scroll = new QScrollArea;
    scroll->setObjectName(name);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->verticalScrollBar()->setToolTip("Scroll to settings below or above the visible rows.");
    scroll->horizontalScrollBar()->setToolTip("Scroll sideways to see the rest of the settings row.");
    return scroll;
}
QJsonArray gradientJson(const std::vector<heatmap::GradientStop> &stops) {
    QJsonArray out;
    for (const auto &s : stops) out.append(QJsonObject{{"position", s.position}, {"color", QString::fromStdString(s.color)}});
    return out;
}
// Black text on light swatches, white on dark (perceived luminance).
QString swatchStyle(const QString &color) {
    const QColor c(color.left(7));
    const double luma = 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue();
    return QStringLiteral("QPushButton { background: %1; color: %2; }").arg(color.left(7), luma > 140 ? "#000000" : "#FFFFFF");
}

class CandlePreview final : public QWidget {
public:
    CandlePreview(heatmap::HeatmapSettingsModel *model, QWidget *parent) : QWidget(parent), model_(model) {
        setObjectName("candlePreview");
        setFixedHeight(54);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#151B21"));
        if (!model_) return;
        const auto &s = model_->settings();
        const QColor up(QString::fromStdString(s.candleUpColor));
        const QColor down(QString::fromStdString(s.candleDownColor));
        const QColor wick(s.candleWickColor == "auto" ? QString{} : QString::fromStdString(s.candleWickColor));
        const int mid = width() / 2;
        const int wickWidth = s.candleWickWidth;
        auto draw = [&](int x, QColor body, int top, int bottom) {
            p.fillRect(QRect(x - wickWidth / 2, 5, wickWidth, 44), wick.isValid() ? wick : body);
            body.setAlphaF(s.candleBodyOpacity);
            p.fillRect(QRect(x - 9, top, 18, bottom - top), body);
        };
        draw(mid - 27, up, 14, 31);
        draw(mid + 27, down, 24, 42);
    }
private:
    heatmap::HeatmapSettingsModel *model_ = nullptr;
};
} // namespace

// ------------------------------------------------------------ gradient editor
HeatmapGradientEditor::HeatmapGradientEditor(const QString &objectName, QWidget *parent) : QWidget(parent) {
    setObjectName(objectName);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 22, 0, 0); // the preview strip is painted above the table
    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({"Position (0–1)", "Colour"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setMinimumHeight(110);
    table_->setToolTip("Set colours along the liquidity range, from 0 (low) to 1 (high). Editing a stop selects Custom.");
    table_->horizontalHeaderItem(0)->setToolTip("Where this colour sits between range low (0) and range high (1). The first stop stays at 0 and the last at 1. Keep the others in strictly increasing order.");
    table_->horizontalHeaderItem(1)->setToolTip("Choose the heatmap colour at this stop. Editing a colour selects Custom.");
    table_->verticalScrollBar()->setToolTip("Scroll to gradient stops below or above the visible rows.");
    table_->horizontalScrollBar()->setToolTip("Scroll sideways to see the stop position and colour.");
    layout->addWidget(table_);
    auto *buttons = new QHBoxLayout;
    add_ = new QPushButton("Add stop", this);
    remove_ = new QPushButton("Remove stop", this);
    add_->setToolTip("Add a colour stop between the selected stop and the next, up to 16 stops. Use it to shape the Custom palette.");
    remove_->setToolTip("Remove the selected interior colour stop to simplify the Custom palette. The two endpoints stay.");
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(add_, &QPushButton::clicked, this, &HeatmapGradientEditor::addStop);
    connect(remove_, &QPushButton::clicked, this, &HeatmapGradientEditor::removeStop);
}

void HeatmapGradientEditor::addRow(double position, const QString &color) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    auto *spin = new QDoubleSpinBox(table_);
    spin->setRange(0, 1);
    spin->setDecimals(3);
    spin->setSingleStep(0.05);
    spin->setKeyboardTracking(false);
    spin->setValue(position);
    spin->setToolTip(table_->horizontalHeaderItem(0)->toolTip());
    table_->setCellWidget(row, 0, spin);
    auto *button = new QPushButton(color, table_);
    button->setProperty("color", color);
    button->setToolTip(table_->horizontalHeaderItem(1)->toolTip());
    button->setStyleSheet(swatchStyle(color));
    table_->setCellWidget(row, 1, button);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
        if (loading_) return;
        update();
        emit edited();
    });
    connect(button, &QPushButton::clicked, this, [this, button] {
        for (int r = 0; r < table_->rowCount(); ++r)
            if (table_->cellWidget(r, 1) == button) {
                const QColor picked = QColorDialog::getColor(QColor(button->property("color").toString().left(7)), this,
                                                             "Stop colour");
                if (picked.isValid()) setStopColor(r, picked.name());
                return;
            }
    });
}

void HeatmapGradientEditor::setStopColor(int row, const QString &color) {
    auto *button = qobject_cast<QPushButton *>(table_->cellWidget(row, 1));
    if (!button) return;
    button->setText(color);
    button->setProperty("color", color);
    button->setStyleSheet(swatchStyle(color));
    update();
    if (!loading_) emit edited();
}

void HeatmapGradientEditor::setStops(const std::vector<heatmap::GradientStop> &stops) {
    if (stops == this->stops()) return;
    loading_ = true;
    table_->setRowCount(0);
    for (const auto &s : stops) addRow(s.position, QString::fromStdString(s.color));
    loading_ = false;
    update();
}

std::vector<heatmap::GradientStop> HeatmapGradientEditor::stops() const {
    std::vector<heatmap::GradientStop> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const auto *spin = qobject_cast<QDoubleSpinBox *>(table_->cellWidget(r, 0));
        const auto *button = qobject_cast<QPushButton *>(table_->cellWidget(r, 1));
        if (spin && button) out.push_back({spin->value(), button->property("color").toString().toStdString()});
    }
    return out;
}

// A new stop halfway between the selected stop and the next (or the last two).
void HeatmapGradientEditor::addStop() {
    auto current = stops();
    if (current.size() >= 16) return;
    const int at = std::clamp(table_->currentRow(), 0, std::max(0, int(current.size()) - 2));
    const double lo = current.empty() ? 0 : current[at].position;
    const double hi = current.size() > size_t(at + 1) ? current[at + 1].position : 1;
    const QString color = current.empty() ? "#808080" : QString::fromStdString(current[at].color);
    current.insert(current.begin() + std::min<int>(at + 1, int(current.size())), {(lo + hi) / 2, color.toStdString()});
    setStops(current);
    emit edited();
}

void HeatmapGradientEditor::removeStop() {
    auto current = stops();
    const int row = table_->currentRow();
    if (current.size() <= 2 || row <= 0 || row >= int(current.size()) - 1) return; // endpoints stay
    current.erase(current.begin() + row);
    setStops(current);
    emit edited();
}

void HeatmapGradientEditor::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    const QRect strip(0, 2, width(), 16);
    QLinearGradient gradient(strip.topLeft(), strip.topRight());
    for (const auto &s : stops())
        gradient.setColorAt(std::clamp(s.position, 0.0, 1.0), QColor(QString::fromStdString(s.color).left(7)));
    painter.fillRect(strip, gradient);
    painter.setPen(QColor("#3A4550"));
    painter.drawRect(strip.adjusted(0, 0, -1, -1));
}

// ------------------------------------------------------------------- dialog
HeatmapSettingsDialog::HeatmapSettingsDialog(heatmap::HeatmapSettingsModel *model, UnifiedGridRenderer *renderer,
                                             QWidget *parent)
    : QDialog(parent), m_model(model) {
    setObjectName("heatmapSettingsDialog");
    setWindowTitle("Chart Settings");
    setModal(false);
    resize(520, 560);
    setStyleSheet("QDialog { background-color: #1B1F24; } QLabel, QCheckBox { color: #C9D4DD; }");
    buildUi();
    if (m_model) connect(m_model, &heatmap::HeatmapSettingsModel::changed, this, [this] { refreshFromModel(); });
    if (m_model) connect(m_model, &heatmap::HeatmapSettingsModel::budgetsChanged, this, [this] { refreshFromModel(); });
    if (m_model)
        connect(m_model, &heatmap::HeatmapSettingsModel::savedRendererChanged, this, [this] {
            m_savedRenderer->setText(QString::fromStdString(m_model->savedRenderer()));
        });
    refreshFromModel();
    setRenderer(renderer); // binds its signals and refreshes the renderer-backed controls
}

void HeatmapSettingsDialog::setRenderer(UnifiedGridRenderer *renderer) {
    if (m_renderer) disconnect(m_renderer, nullptr, this, nullptr); // reopening rebinds: no duplicates
    m_renderer = renderer;
    if (renderer) {
        // Renderer-backed controls follow changes made elsewhere (toolbar, chart
        // menu, Agent API) without echoing them back.
        connect(renderer, &UnifiedGridRenderer::candleStyleChanged, this, [this] {
            if (!m_renderer) return;
            const QSignalBlocker block(m_candleStyle);
            m_candleStyle->setCurrentIndex(std::clamp(m_renderer->candleStyle(), 0, 2));
        });
        connect(renderer, &UnifiedGridRenderer::tpoStyleChanged, this, [this] {
            if (!m_renderer) return;
            const QSignalBlocker a(m_tpoLayoutCombo), b(m_tpoThemeCombo);
            m_tpoLayoutCombo->setCurrentIndex(std::max(0, m_tpoLayoutCombo->findData(m_renderer->tpoLayout())));
            m_tpoThemeCombo->setCurrentIndex(std::max(0, m_tpoThemeCombo->findData(m_renderer->tpoTheme())));
        });
    }
    refreshFromRenderer();
}

QStringList HeatmapSettingsDialog::tabKeys(const QString &tab) {
    if (tab == "Chart") return {"tradesAboveCandles", "showLabels", "labelCurrency", "labelMinPx", "labelMaxPx",
                                "candleUpColor", "candleDownColor", "candleWickColor", "candleBodyOpacity", "candleWickWidth"};
    if (tab == "Tick") return {"tickMode", "manualTick", "minRowPx", "hysteresis"};
    if (tab == "Look")
        return {"palettePreset", "bidGradient", "askGradient", "sensitivityMin", "sensitivityMax", "opacity",
                "crossfadeMs", "showBandEdges"};
    if (tab == "Budgets") return {"gpuCapBytes", "uploadBudgetBytes", "prefetchTiles"};
    if (tab == "Live") return {"liveMinIntervalMs"};
    if (tab == "Debug") return {"renderer", "showTelemetry"};
    return {};
}

void HeatmapSettingsDialog::buildUi() {
    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("heatmapSettingsTabs");
    m_tabs->addTab(scrollTab(buildChartTab(), "chartSettingsScroll"), "Chart");
    m_tabs->addTab(scrollTab(buildTickTab(), "tickSettingsScroll"), "Tick");
    m_tabs->addTab(scrollTab(buildLookTab(), "lookSettingsScroll"), "Look");
    m_tabs->addTab(scrollTab(buildBudgetsTab(), "budgetsSettingsScroll"), "Budgets");
    m_tabs->addTab(scrollTab(buildLiveTab(), "liveSettingsScroll"), "Live");
    m_tabs->addTab(scrollTab(buildDebugTab(), "debugSettingsScroll"), "Debug");
    m_tabs->addTab(scrollTab(buildTpoTab(), "tpoSettingsScroll"), "TPO");
    for (int i = 0; i < m_tabs->count(); ++i)
        m_tabs->setTabToolTip(i, m_tabs->tabText(i) == "Budgets"
            ? QStringLiteral("Open memory settings. The GPU group is for this chart; the RAM group applies to all charts.")
            : "Open " + m_tabs->tabText(i) + " settings for this chart.");
    for (auto *button : m_tabs->tabBar()->findChildren<QAbstractButton *>())
        button->setToolTip("Scroll the tab strip to reach more settings tabs.");
    layout->addWidget(note("Changes take effect immediately. Close keeps saved chart settings; session-only controls are labelled.", this));
    layout->addWidget(m_tabs);

    m_status = new QLabel(this);
    m_status->setObjectName("settingsStatus");
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    auto *logButton = buttons->addButton("Log Settings", QDialogButtonBox::ActionRole);
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);
    logButton->setToolTip("Write this chart's settings and gamma, contrast and floor values to the Sentinel run log. Use it when diagnosing the chart.");
    closeButton->setToolTip("Close settings; changes already applied stay in effect. Session-only changes last until the app closes.");
    buttons->setStyleSheet(
        "QDialogButtonBox QPushButton { background-color: #2B5A7A; color: #FFFFFF; border: none; padding: 8px 16px; border-radius: 4px; }"
        "QDialogButtonBox QPushButton:hover { background-color: #3472A0; }");
    layout->addWidget(buttons);
    connect(logButton, &QPushButton::clicked, this, &HeatmapSettingsDialog::logSettings);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
}

QPushButton *HeatmapSettingsDialog::resetButton(const QString &tab, QWidget *parent) {
    auto *button = new QPushButton("Reset " + tab + " to defaults", parent);
    button->setObjectName("reset" + tab);
    button->setToolTip("Restore the configured defaults for " + tab + ". Use it to undo changes on this tab."
                      + (tab == "Chart" ? " Candle style is not reset." : ""));
    connect(button, &QPushButton::clicked, this, [this, tab] {
        if (!m_model) return;
        const auto error = m_model->resetKeys(tabKeys(tab));
        QString budgetError;
        if (tab == "Budgets") budgetError = m_model->setBudgets(m_model->defaultBudgets());
        // Renderer-backed controls return to their configured values too.
        if (tab == "Look" && m_renderer) {
            const auto &d = m_model->configDefaults();
            m_renderer->setHeatmapGamma(d.gamma);
            m_renderer->setHeatmapContrast(d.contrast);
            m_renderer->setHeatmapShaderFloor(d.shaderFloor);
        }
        if (tab == "TPO" && m_renderer) m_renderer->applyTpoConfig(m_tpoDefaults);
        refreshFromRenderer();
        if (!error.isEmpty() || !budgetError.isEmpty()) showStatus(error.isEmpty() ? budgetError : error, true);
        else showStatus(tab + " reset to defaults", false);
        refreshFromModel();
    });
    return button;
}

QWidget *HeatmapSettingsDialog::buildChartTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    m_showLabels = new QCheckBox("Show liquidity labels on heatmap cells", page);
    m_showLabels->setObjectName("showLabels");
    form->addRow("Labels", m_showLabels);
    m_labelCurrency = new QComboBox(page);
    m_labelCurrency->setObjectName("labelCurrency");
    m_labelCurrency->addItem("USD ($1.24M)", "usd");
    m_labelCurrency->addItem("Asset (12.5)", "asset");
    form->addRow("Label currency", m_labelCurrency);
    m_labelMinPx = doubleSpin(page, "labelMinPx", 8, 24, 0.5, 1);
    m_labelMinPx->setSuffix(" px");
    m_labelMaxPx = doubleSpin(page, "labelMaxPx", 8, 32, 0.5, 1);
    m_labelMaxPx->setSuffix(" px");
    form->addRow("Label size, smallest", m_labelMinPx);
    form->addRow("Label size, largest", m_labelMaxPx);
    form->addRow(note("GPU labels appear on coloured cells above the range low when the text fits. "
                      "Their size grows with the cell, up to the largest size.",
                      page));
    m_tradesAboveCandles = new QCheckBox("Trades above candles", page);
    m_tradesAboveCandles->setObjectName("tradesAboveCandles");
    m_tradesAboveCandles->setToolTip("Keep translucent executions visible over opaque candle bodies (GPU renderer).");
    form->addRow("Trades", m_tradesAboveCandles);
    connect(m_tradesAboveCandles, &QCheckBox::toggled, this, [this](bool on) { apply({{"tradesAboveCandles", on}}); });
    auto *candles = new QGroupBox("Candles", page);
    auto *candleForm = new QFormLayout(candles);
    m_candleStyle = new QComboBox(candles);
    m_candleStyle->setObjectName("candleStyle");
    m_candleStyle->addItems({"Candle", "Hollow", "Line"});
    candleForm->addRow("Style (this session)", m_candleStyle);
    m_candleUpColor = new QPushButton(candles);
    m_candleUpColor->setObjectName("candleUpColor");
    m_candleDownColor = new QPushButton(candles);
    m_candleDownColor->setObjectName("candleDownColor");
    m_candleWickColor = new QPushButton(candles);
    m_candleWickColor->setObjectName("candleWickColor");
    m_candleWickAuto = new QPushButton("Use body colour", candles);
    m_candleWickAuto->setObjectName("candleWickAuto");
    auto *wickRow = new QWidget(candles);
    auto *wickLayout = new QHBoxLayout(wickRow);
    wickLayout->setContentsMargins(0, 0, 0, 0);
    wickLayout->addWidget(m_candleWickColor);
    wickLayout->addWidget(m_candleWickAuto);
    candleForm->addRow("Up colour", m_candleUpColor);
    candleForm->addRow("Down colour", m_candleDownColor);
    candleForm->addRow("Wick colour", wickRow);
    m_candleBodyOpacity = intSpin(candles, "candleBodyOpacity", 0, 100, 5, "%");
    m_candleWickWidth = intSpin(candles, "candleWickWidth", 1, 3, 1, " px");
    candleForm->addRow("Body opacity", m_candleBodyOpacity);
    candleForm->addRow("Wick width (device pixels)", m_candleWickWidth);
    m_candlePreview = new CandlePreview(m_model, candles);
    candleForm->addRow("Preview", m_candlePreview);
    form->addRow(candles);
    form->addRow(resetButton("Chart", page));
    describe(form, m_showLabels, "Show liquidity sizes on GPU heatmap cells above the range low when text fits. Turn off to reduce clutter.");
    describe(form, m_labelCurrency, "Show liquidity as USD notional or base-asset size. Use USD to compare value, or Asset to compare quantity.");
    describe(form, m_labelMinPx, "Smallest GPU liquidity text, in pixels (8-24); labels that cannot fit at this size are hidden. Raise it for readability.");
    describe(form, m_labelMaxPx, "Largest GPU liquidity text, in pixels (8-32). Raise it for bigger labels when cells have room.");
    describe(form, m_tradesAboveCandles, m_tradesAboveCandles->toolTip());
    describe(candleForm, m_candleStyle, "Choose filled candles, hollow up candles, or a line through closes. Use the line for a simpler price view; this session only.");
    describe(candleForm, m_candleUpColor, "Choose the colour for candles that close at or above their open. Change it to make up bars easier to spot.");
    describe(candleForm, m_candleDownColor, "Choose the colour for candles that close below their open. Change it to make down bars easier to spot.");
    describe(candleForm, wickRow, "Use a fixed wick colour to separate highs and lows from candle bodies, or Use body colour to match each bar.");
    m_candleWickColor->setToolTip(wickRow->toolTip());
    m_candleWickAuto->setToolTip(wickRow->toolTip());
    describe(candleForm, m_candleBodyOpacity, "Candle body opacity: 0% = transparent, 100% = solid; also applies to the close-price line. Lower it to see the heatmap underneath.");
    describe(candleForm, m_candleWickWidth, "Wick thickness in device pixels (1-3). Raise it when highs and lows are hard to see.");
    connect(m_showLabels, &QCheckBox::toggled, this, [this](bool on) { apply({{"showLabels", on}}); });
    connect(m_labelCurrency, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) apply({{"labelCurrency", m_labelCurrency->itemData(index).toString()}});
    });
    connect(m_labelMinPx, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        apply({{"labelMinPx", v}, {"labelMaxPx", std::max(v, m_labelMaxPx->value())}});
    });
    connect(m_labelMaxPx, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        apply({{"labelMinPx", std::min(v, m_labelMinPx->value())}, {"labelMaxPx", v}});
    });
    connect(m_candleStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_renderer && index >= 0 && !m_loading) m_renderer->setCandleStyle(index);
    });
    auto pick = [this](QPushButton *button, const char *key) {
        connect(button, &QPushButton::clicked, this, [this, key] {
            const auto current = m_model ? heatmap::settingsJson(m_model->settings())[key].toString() : QString{};
            const QColor picked = QColorDialog::getColor(QColor(current == "auto" ? "#E6EDF3" : current), this,
                "Candle colour", QColorDialog::DontUseNativeDialog);
            if (picked.isValid()) apply({{key, picked.name(QColor::HexRgb).toUpper()}});
        });
    };
    pick(m_candleUpColor, "candleUpColor");
    pick(m_candleDownColor, "candleDownColor");
    pick(m_candleWickColor, "candleWickColor");
    connect(m_candleWickAuto, &QPushButton::clicked, this, [this] { apply({{"candleWickColor", "auto"}}); });
    connect(m_candleBodyOpacity, &QSpinBox::valueChanged, this, [this](int value) {
        apply({{"candleBodyOpacity", value / 100.0}});
    });
    connect(m_candleWickWidth, &QSpinBox::valueChanged, this, [this](int value) {
        apply({{"candleWickWidth", value}});
    });
    return page;
}

QWidget *HeatmapSettingsDialog::buildTickTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    m_tickMode = new QComboBox(page);
    m_tickMode->setObjectName("tickMode");
    m_tickMode->addItem("Auto", "auto");
    m_tickMode->addItem("Manual", "manual");
    form->addRow("Tick mode", m_tickMode);
    m_manualTick = new QComboBox(page);
    m_manualTick->setObjectName("manualTick");
    form->addRow("Tick preset", m_manualTick);
    m_minRowPx = doubleSpin(page, "minRowPx", 0.5, 32, 0.25, 2);
    m_minRowPx->setSuffix(" px");
    form->addRow("Min row height (Auto)", m_minRowPx);
    m_hysteresis = doubleSpin(page, "hysteresis", 0, 0.9, 0.05, 2);
    form->addRow("Hysteresis h (Auto)", m_hysteresis);
    form->addRow(note("GPU heatmap: Auto chooses a tick that keeps rows at least the minimum height; "
                      "hysteresis limits switching near that height. Picking a preset locks Manual. "
                      "The choice is remembered per symbol and timeframe; entering Manual restores it or locks the drawn tick. "
                      "Loaded data offers the presets. Columns that cannot build a locked tick are veiled.", page));
    form->addRow(resetButton("Tick", page));
    describe(form, m_tickMode, "Auto adjusts GPU heatmap price rows as you zoom. Manual restores the tick remembered for this symbol and timeframe, or locks the drawn tick.");
    describe(form, m_manualTick, "Presets some loaded data can build. Picking one locks Manual.");
    describe(form, m_minRowPx, "Target minimum GPU heatmap row height in physical pixels (0.5-32) in Auto. Raise it for coarser, taller rows.");
    describe(form, m_hysteresis, "Margin around Auto's row-height threshold (0-0.9; 0 = no margin). Raise it to reduce tick switching during small zoom changes.");

    connect(m_tickMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_loading) return;
        emit tickModeRequested(index == 1);
        refreshFromModel();
    });
    // activated: a user's pick, also of the tick shown now (locks it in Auto).
    connect(m_manualTick, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (m_loading || index < 0) return;
        emit tickPresetRequested(m_manualTick->itemData(index).toLongLong());
        refreshFromModel();
    });
    connect(m_minRowPx, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply({{"minRowPx", v}}); });
    connect(m_hysteresis, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply({{"hysteresis", v}}); });
    return page;
}

QWidget *HeatmapSettingsDialog::buildLookTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    m_palette = new QComboBox(page);
    m_palette->setObjectName("palettePreset");
    m_palette->addItems(kPalettes);
    form->addRow("Palette", m_palette);
    m_bidGradient = new HeatmapGradientEditor("bidGradient", page);
    m_askGradient = new HeatmapGradientEditor("askGradient", page);
    form->addRow("Bid gradient (Custom)", m_bidGradient);
    form->addRow("Ask gradient (Custom)", m_askGradient);
    m_sensitivityMin = doubleSpin(page, "sensitivityMin", 0.0001, 1e12, 0.01, 4);
    m_sensitivityMax = doubleSpin(page, "sensitivityMax", 0.0001, 1e15, 1, 4);
    m_sensitivityMin->setToolTip("Base-asset size per heatmap cell. Below the toolbar's low handle, cells have no colour or label.");
    m_sensitivityMax->setToolTip("Base-asset size per heatmap cell. Colour saturates at the toolbar's high handle.");
    form->addRow("Range low (base asset)", m_sensitivityMin);
    form->addRow("Range high (base asset)", m_sensitivityMax);
    m_opacity = doubleSpin(page, "opacity", 0, 1, 0.05, 2);
    form->addRow("Opacity (0–1)", m_opacity);
    auto *fadeRow = new QHBoxLayout;
    m_crossfadeOn = new QCheckBox("Crossfade tick changes", page);
    m_crossfadeOn->setObjectName("crossfadeOn");
    m_crossfadeMs = intSpin(page, "crossfadeMs", 1, 2000, 25, " ms");
    // The value "on" restores while crossfade is off (0): the configured default.
    m_crossfadeMs->setValue(m_model && m_model->defaultSettings().crossfadeMs > 0 ? m_model->defaultSettings().crossfadeMs : 150);
    fadeRow->addWidget(m_crossfadeOn);
    fadeRow->addWidget(m_crossfadeMs);
    form->addRow("Transition", fadeRow);
    m_bandEdges = new QCheckBox("Show near-band edges", page);
    m_bandEdges->setObjectName("showBandEdges");
    m_bandEdges->setToolTip("Saved with the chart. sentinel-lab draws the band edges; the main chart does not yet.");
    form->addRow("Band edges (lab only)", m_bandEdges);
    form->setRowVisible(m_bandEdges, false); // lab-only setting has no main-chart effect

    auto *tone = new QGroupBox("Tone mapping (both renderers, this session)", page);
    auto *toneForm = new QFormLayout(tone);
    auto [gammaSlider, gammaLabel] = makeSlider(tone, 10, 500, 85);
    m_gammaSlider = gammaSlider;
    m_gammaLabel = gammaLabel;
    auto *gammaRow = new QHBoxLayout;
    gammaRow->addWidget(m_gammaSlider);
    gammaRow->addWidget(m_gammaLabel);
    toneForm->addRow("Gamma", gammaRow);
    auto [contrastSlider, contrastLabel] = makeSlider(tone, 10, 500, 160);
    m_contrastSlider = contrastSlider;
    m_contrastLabel = contrastLabel;
    auto *contrastRow = new QHBoxLayout;
    contrastRow->addWidget(m_contrastSlider);
    contrastRow->addWidget(m_contrastLabel);
    toneForm->addRow("Contrast", contrastRow);
    auto [floorSlider, floorLabel] = makeSlider(tone, 0, 50, 0);
    m_floorSlider = floorSlider;
    m_floorLabel = floorLabel;
    auto *floorRow = new QHBoxLayout;
    floorRow->addWidget(m_floorSlider);
    floorRow->addWidget(m_floorLabel);
    toneForm->addRow("Shader floor", floorRow);
    form->addRow(tone);
    form->addRow(resetButton("Look", page));
    describe(form, m_palette, "Choose the heatmap's bid and ask colour scale. Pick Custom to use the gradient stops below.");
    describe(form, m_bidGradient, "Set the bid-side colours from low to high liquidity. Edit stops to select the Custom palette.");
    describe(form, m_askGradient, "Set the ask-side colours from low to high liquidity. Edit stops to select the Custom palette.");
    describe(form, m_sensitivityMin, m_sensitivityMin->toolTip());
    describe(form, m_sensitivityMax, m_sensitivityMax->toolTip());
    describe(form, m_opacity, "GPU heatmap opacity: 0 = transparent, 1 = solid. Lower it to see other chart layers through the heatmap. The legacy renderer ignores it.");
    describe(form, fadeRow, "Blend old and new GPU heatmap rows over 1-2000 milliseconds when the tick changes. Turn off for an immediate switch, or lower the duration for a quicker blend.");
    describe(form, m_bandEdges, m_bandEdges->toolTip());
    describe(toneForm, gammaRow, "Curve of heatmap colour intensity (0.1-5). Lower it to bring out weaker liquidity; higher values favour stronger liquidity, this session only.");
    describe(toneForm, contrastRow, "Spread heatmap colour intensity around its midpoint (0.1-5). Raise it for stronger separation, or lower it for a flatter scale; this session only.");
    describe(toneForm, floorRow, "Minimum intensity before the colour curve (0-0.5; 0 = off). Raise it to boost faint visible cells; cells below range low stay hidden, this session only.");

    connect(m_palette, &QComboBox::currentTextChanged, this, [this](const QString &p) { apply({{"palettePreset", p}}); });
    // Editing a gradient selects the Custom palette (the presets ignore them).
    connect(m_bidGradient, &HeatmapGradientEditor::edited, this, [this] {
        apply({{"bidGradient", gradientJson(m_bidGradient->stops())}, {"palettePreset", "Custom"}});
    });
    connect(m_askGradient, &HeatmapGradientEditor::edited, this, [this] {
        apply({{"askGradient", gradientJson(m_askGradient->stops())}, {"palettePreset", "Custom"}});
    });
    connect(m_sensitivityMin, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply({{"sensitivityMin", v}}); });
    connect(m_sensitivityMax, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply({{"sensitivityMax", v}}); });
    connect(m_opacity, &QDoubleSpinBox::valueChanged, this, [this](double v) { apply({{"opacity", v}}); });
    connect(m_crossfadeOn, &QCheckBox::toggled, this, [this](bool on) {
        apply({{"crossfadeMs", on ? m_crossfadeMs->value() : 0}});
    });
    connect(m_crossfadeMs, &QSpinBox::valueChanged, this, [this](int ms) {
        if (m_crossfadeOn->isChecked()) apply({{"crossfadeMs", ms}});
    });
    connect(m_bandEdges, &QCheckBox::toggled, this, [this](bool on) { apply({{"showBandEdges", on}}); });
    connect(m_gammaSlider, &QSlider::valueChanged, this, [this](int v) {
        m_gammaLabel->setText(QString::number(v / 100.0, 'f', 2));
        if (m_renderer && !m_loading) m_renderer->setHeatmapGamma(v / 100.0);
    });
    connect(m_contrastSlider, &QSlider::valueChanged, this, [this](int v) {
        m_contrastLabel->setText(QString::number(v / 100.0, 'f', 2));
        if (m_renderer && !m_loading) m_renderer->setHeatmapContrast(v / 100.0);
    });
    connect(m_floorSlider, &QSlider::valueChanged, this, [this](int v) {
        m_floorLabel->setText(QString::number(v / 100.0, 'f', 3));
        if (m_renderer && !m_loading) m_renderer->setHeatmapShaderFloor(v / 100.0);
    });
    return page;
}

QWidget *HeatmapSettingsDialog::buildBudgetsTab() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *chart = new QGroupBox("This chart", page);
    auto *chartForm = new QFormLayout(chart);
    m_gpuCapMiB = intSpin(chart, "gpuCapBytes", 1, 4096, 32, " MiB");
    chartForm->addRow("GPU memory cap", m_gpuCapMiB);
    m_uploadKiB = intSpin(chart, "uploadBudgetBytes", 1, 128 * 1024, 512, " KiB");
    chartForm->addRow("Upload budget per frame", m_uploadKiB);
    m_prefetchTiles = intSpin(chart, "prefetchTiles", 0, 16, 1, "");
    m_prefetchTiles->setToolTip("Saved with the chart. The span planner prefetches max(2, view width) tiles and "
                                "does not read this setting yet.");
    chartForm->addRow("Prefetch tiles (not used yet)", m_prefetchTiles);
    chartForm->setRowVisible(m_prefetchTiles, false); // retain the saved field without offering an inactive control
    chartForm->addRow(note("The upload budget is at most 128 MiB and at most the GPU cap.", chart));
    layout->addWidget(chart);

    auto *process = new QGroupBox("Process RAM tiers (all charts)", page);
    auto *processForm = new QFormLayout(process);
    m_decodedMiB = intSpin(process, "decodedChunks", 1, 4096, 64, " MiB");
    m_spanMiB = intSpin(process, "spanSources", 1, 4096, 64, " MiB");
    m_ceilingMiB = intSpin(process, "cpuCeiling", 1, 4096, 64, " MiB");
    processForm->addRow("Decoded chunks", m_decodedMiB);
    processForm->addRow("Span sources", m_spanMiB);
    processForm->addRow("CPU ceiling", m_ceilingMiB);
    processForm->addRow(note("These apply process-wide, to every chart, at once. The CPU ceiling must be at least "
                             "decoded chunks + span sources; each tier is 1..4096 MiB.", process));
    layout->addWidget(process);
    layout->addWidget(resetButton("Budgets", page));
    layout->addStretch();
    describe(chartForm, m_gpuCapMiB, "GPU heatmap cache budget for this chart, in MiB. Raise it if panning reloads tiles; tiles on screen or kept as a stand-in while detail loads can keep usage above the cap.");
    describe(chartForm, m_uploadKiB, "GPU heatmap upload budget per frame, in KiB; capped at 128 MiB and the GPU memory cap. Lower it to spread uploads across frames, or raise it to load tiles sooner.");
    describe(chartForm, m_prefetchTiles, m_prefetchTiles->toolTip());
    describe(processForm, m_decodedMiB, "Budget for recorded data unpacked in RAM across all charts, in MiB (1-4096). Raise it to keep more history cached when revisiting it.");
    describe(processForm, m_spanMiB, "RAM budget for ready-to-draw heatmap data built from recordings across all charts, in MiB (1-4096). Raise it to keep more chart data ready for reuse when panning.");
    describe(processForm, m_ceilingMiB, "Total RAM allowed for both of the above across all charts, in MiB (1-4096); must be at least their combined budgets. Raise it when the memory limit prevents loading more heatmap data.");

    connect(m_gpuCapMiB, &QSpinBox::valueChanged, this, [this](int mib) { apply({{"gpuCapBytes", qint64(mib) * qint64(MiB)}}); });
    connect(m_uploadKiB, &QSpinBox::valueChanged, this, [this](int kib) {
        apply({{"uploadBudgetBytes", qint64(kib) * qint64(KiB)}});
    });
    connect(m_prefetchTiles, &QSpinBox::valueChanged, this, [this](int n) { apply({{"prefetchTiles", n}}); });
    for (auto *spin : {m_decodedMiB, m_spanMiB, m_ceilingMiB})
        connect(spin, &QSpinBox::valueChanged, this, [this] { applyBudgets(); });
    return page;
}

QWidget *HeatmapSettingsDialog::buildLiveTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    m_liveMinInterval = intSpin(page, "liveMinIntervalMs", 100, 5000, 50, " ms");
    form->addRow("Live min interval", m_liveMinInterval);
    form->addRow(note("The shortest spacing of live-edge compositions (the server publishes at 1 Hz). Slow "
                      "compositions back off to at least 5 s.", page));
    form->addRow(resetButton("Live", page));
    describe(form, m_liveMinInterval, "Minimum spacing of GPU refreshes of the newest heatmap columns in milliseconds (100-5000); the server publishes at 1 Hz. Raise it to reduce refresh work; slow refreshes back off to at least 5 seconds.");
    connect(m_liveMinInterval, &QSpinBox::valueChanged, this, [this](int ms) { apply({{"liveMinIntervalMs", ms}}); });
    return page;
}

QWidget *HeatmapSettingsDialog::buildDebugTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    m_rendererCombo = new QComboBox(page);
    m_rendererCombo->setObjectName("renderer");
    m_rendererCombo->addItem("GPU (default)", "gpu");
    m_rendererCombo->addItem("Legacy", "legacy");
    form->addRow("Renderer", m_rendererCombo);
    m_makeDefault = new QCheckBox("Make default (saved; otherwise this session only)", page);
    m_makeDefault->setObjectName("makeDefault");
    form->addRow("", m_makeDefault);
    m_savedRenderer = new QLabel(page);
    m_savedRenderer->setObjectName("savedRenderer");
    form->addRow("Saved default", m_savedRenderer);
    m_showTelemetry = new QCheckBox("Show the heatmap telemetry dock", page);
    m_showTelemetry->setObjectName("showTelemetry");
    form->addRow("Telemetry", m_showTelemetry);
    form->addRow(note("GPU heatmap cell labels can be toggled in Chart; the legacy renderer draws its own labels.", page));
    form->addRow(resetButton("Debug", page));
    describe(form, m_rendererCombo, "Choose GPU or Legacy heatmap rendering for this chart. Switch to compare output or diagnose rendering; this session only unless Make default is checked.");
    describe(form, m_makeDefault, "Save the selected renderer as this chart's default for future launches. Leave unchecked for a session-only comparison.");
    describe(form, m_showTelemetry, "Show the heatmap telemetry dock with tick, cache, upload and timing values. Use it to diagnose missing data or slow rendering.");
    connect(m_rendererCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        apply({{"renderer", m_rendererCombo->currentData().toString()}}, m_makeDefault->isChecked());
    });
    connect(m_makeDefault, &QCheckBox::toggled, this, [this](bool on) {
        if (on) apply({{"renderer", m_rendererCombo->currentData().toString()}}, true);
    });
    connect(m_showTelemetry, &QCheckBox::toggled, this, [this](bool on) { apply({{"showTelemetry", on}}); });
    return page;
}

QWidget *HeatmapSettingsDialog::buildTpoTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->addRow(note("Changes in this tab apply to this session.", page));
    m_tpoTimeframeCombo = new QComboBox(page);
    m_tpoTimeframeCombo->addItem("15m", 900000);
    m_tpoTimeframeCombo->addItem("30m", 1800000);
    m_tpoTimeframeCombo->addItem("1h", 3600000);
    m_tpoTimeframeCombo->addItem("4h", 14400000);
    m_tpoTimeframeCombo->addItem("1D", 86400000);
    form->addRow("TPO Bracket", m_tpoTimeframeCombo);
    m_tpoSessionCombo = new QComboBox(page);
    for (const auto &[name, id] : std::initializer_list<std::pair<const char *, int>>{
             {"New York", 0}, {"London", 1}, {"Asia", 2}, {"Australia", 3}, {"24H", 4}, {"1W", 5}, {"1M", 6}})
        m_tpoSessionCombo->addItem(name, id);
    form->addRow("TPO Session", m_tpoSessionCombo);
    m_tpoLayoutCombo = new QComboBox(page);
    m_tpoLayoutCombo->setObjectName("tpoLayout");
    m_tpoLayoutCombo->addItem("Collapsed", "collapsed");
    m_tpoLayoutCombo->addItem("Split", "split");
    form->addRow("TPO Layout", m_tpoLayoutCombo);
    m_tpoThemeCombo = new QComboBox(page);
    m_tpoThemeCombo->setObjectName("tpoTheme");
    m_tpoThemeCombo->addItem("Rainbow", "rainbow");
    m_tpoThemeCombo->addItem("Calm", "calm");
    m_tpoThemeCombo->addItem("Sage", "sage");
    form->addRow("TPO Theme", m_tpoThemeCombo);
    form->addRow(resetButton("TPO", page));
    describe(form, m_tpoTimeframeCombo, "Time covered by each TPO letter (15 minutes to 1 day). Use shorter brackets for more detail. A bracket that does not divide the session evenly, or gives too many columns, is changed to the nearest one that does. This session only.");
    describe(form, m_tpoSessionCombo, "Group TPO letters into a regional session, UTC day, week or month. Change it to compare profiles over that trading window; this session only.");
    describe(form, m_tpoLayoutCombo, "Collapsed packs each price row into a profile; Split keeps brackets in separate time columns. Use Split to follow the session's sequence, this session only.");
    describe(form, m_tpoThemeCombo, "Choose the TPO cell colours, including value-area and point-of-control highlights. Change it for clearer period or profile contrast; this session only.");
    connect(m_tpoTimeframeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_renderer && idx >= 0 && !m_loading) m_renderer->setTpoTimeframeMs(m_tpoTimeframeCombo->itemData(idx).toInt());
    });
    connect(m_tpoSessionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_renderer && idx >= 0 && !m_loading) m_renderer->setTpoSessionType(m_tpoSessionCombo->itemData(idx).toInt());
    });
    connect(m_tpoLayoutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_renderer && idx >= 0 && !m_loading) m_renderer->setTpoLayout(m_tpoLayoutCombo->itemData(idx).toString());
    });
    connect(m_tpoThemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_renderer && idx >= 0 && !m_loading) m_renderer->setTpoTheme(m_tpoThemeCombo->itemData(idx).toString());
    });
    return page;
}

void HeatmapSettingsDialog::apply(const QJsonObject &patch, bool persist) {
    if (m_loading || !m_model) return;
    const auto error = m_model->apply(patch, persist);
    if (!error.isEmpty()) {
        showStatus(error, true);
        refreshFromModel(); // the widgets return to the model's values
        return;
    }
    // The model may clamp (S6a): show what it kept, e.g. max raised above min.
    refreshFromModel();
    showStatus(persist ? QStringLiteral("Applied and saved") : QStringLiteral("Applied for this session"), false);
}

void HeatmapSettingsDialog::applyBudgets() {
    if (m_loading || !m_model) return;
    heatmap::HeatmapBudgets b = m_model->budgets();
    b.decodedChunks = size_t(m_decodedMiB->value()) * MiB;
    b.spanSources = size_t(m_spanMiB->value()) * MiB;
    b.cpuCeiling = size_t(m_ceilingMiB->value()) * MiB;
    // An invalid combination stays in the widgets (the user may be mid-edit) and
    // changes nothing until it is valid.
    if (const auto error = m_model->setBudgets(b); !error.isEmpty()) showStatus(error, true);
    else showStatus(QStringLiteral("Process budgets applied and saved"), false);
}

void HeatmapSettingsDialog::setTickSelectorState(const TopToolbar::TickSelectorState &state) {
    m_tickState = state;
    TopToolbar::fillTickPresetCombo(m_manualTick, state,
                                    state.enabled ? QStringLiteral("Loading presets...")
                                                  : QStringLiteral("Presets come from loaded data (GPU renderer)"));
    m_manualTick->setEnabled(!state.offeredUnits.empty());
    m_manualTick->setToolTip(state.offeredUnits.empty()
        ? QStringLiteral("No loaded data offers presets yet (the GPU renderer loads them).")
        : QStringLiteral("Presets some loaded data can build. Picking one locks Manual."));
    auto *form = qobject_cast<QFormLayout *>(m_manualTick->parentWidget()->layout());
    if (form) describe(form, m_manualTick, m_manualTick->toolTip());
}

void HeatmapSettingsDialog::refreshFromModel() {
    if (!m_model) return;
    const auto &s = m_model->settings();
    const bool was = m_loading;
    m_loading = true;
    const auto set = [](auto *widget, auto value) {
        const QSignalBlocker block(widget);
        widget->setValue(value);
    };
    {
        const QSignalBlocker block(m_tickMode);
        m_tickMode->setCurrentIndex(s.tickMode == heatmap::TickMode::Manual ? 1 : 0);
    }
    set(m_minRowPx, s.minRowPx);
    set(m_hysteresis, s.hysteresis);
    {
        const QSignalBlocker a(m_showLabels), b(m_labelCurrency);
        m_showLabels->setChecked(s.showLabels);
        const QSignalBlocker trades(m_tradesAboveCandles);
        m_tradesAboveCandles->setChecked(s.tradesAboveCandles);
        m_tradesAboveCandles->setEnabled(s.renderer == "gpu");
        m_labelCurrency->setCurrentIndex(std::max(0, m_labelCurrency->findData(QString::fromStdString(s.labelCurrency))));
    }
    set(m_labelMinPx, s.labelMinPx);
    set(m_labelMaxPx, s.labelMaxPx);
    auto swatch = [](QPushButton *button, const std::string &value) {
        const QString color = QString::fromStdString(value);
        button->setText(color == "auto" ? "Body colour" : color);
        button->setStyleSheet(color == "auto" ? QString{} : swatchStyle(color));
    };
    swatch(m_candleUpColor, s.candleUpColor);
    swatch(m_candleDownColor, s.candleDownColor);
    swatch(m_candleWickColor, s.candleWickColor);
    m_candleWickAuto->setEnabled(s.candleWickColor != "auto");
    set(m_candleBodyOpacity, int(std::lround(s.candleBodyOpacity * 100)));
    set(m_candleWickWidth, s.candleWickWidth);
    m_candlePreview->update();
    {
        const QSignalBlocker block(m_palette);
        m_palette->setCurrentIndex(std::max<qsizetype>(0, kPalettes.indexOf(QString::fromStdString(s.palettePreset))));
    }
    m_bidGradient->setStops(s.bidGradient);
    m_askGradient->setStops(s.askGradient);
    set(m_sensitivityMin, s.sensitivityMin);
    set(m_sensitivityMax, s.sensitivityMax);
    set(m_opacity, s.opacity);
    {
        const QSignalBlocker block(m_crossfadeOn);
        m_crossfadeOn->setChecked(s.crossfadeMs > 0);
    }
    if (s.crossfadeMs > 0) set(m_crossfadeMs, s.crossfadeMs);
    m_crossfadeMs->setEnabled(s.crossfadeMs > 0);
    {
        const QSignalBlocker block(m_bandEdges);
        m_bandEdges->setChecked(s.showBandEdges);
    }
    set(m_gpuCapMiB, int(std::max<uint64_t>(1, s.gpuCapBytes / MiB)));
    set(m_uploadKiB, int(std::max<uint64_t>(1, s.uploadBudgetBytes / KiB)));
    set(m_prefetchTiles, s.prefetchTiles);
    const auto b = m_model->budgets();
    set(m_decodedMiB, int(b.decodedChunks / MiB));
    set(m_spanMiB, int(b.spanSources / MiB));
    set(m_ceilingMiB, int(b.cpuCeiling / MiB));
    set(m_liveMinInterval, s.liveMinIntervalMs);
    {
        const QSignalBlocker block(m_rendererCombo);
        m_rendererCombo->setCurrentIndex(std::max(0, m_rendererCombo->findData(QString::fromStdString(s.renderer))));
    }
    m_savedRenderer->setText(QString::fromStdString(m_model->savedRenderer()));
    {
        const QSignalBlocker block(m_showTelemetry);
        m_showTelemetry->setChecked(s.showTelemetry);
    }
    m_loading = was;
}

void HeatmapSettingsDialog::refreshFromRenderer() {
    if (!m_renderer) return;
    const bool was = m_loading;
    m_loading = true;
    m_gammaSlider->setValue(static_cast<int>(m_renderer->heatmapGamma() * 100));
    m_contrastSlider->setValue(static_cast<int>(m_renderer->heatmapContrast() * 100));
    m_floorSlider->setValue(static_cast<int>(m_renderer->heatmapShaderFloor() * 100));
    m_gammaLabel->setText(QString::number(m_renderer->heatmapGamma(), 'f', 2));
    m_contrastLabel->setText(QString::number(m_renderer->heatmapContrast(), 'f', 2));
    m_floorLabel->setText(QString::number(m_renderer->heatmapShaderFloor(), 'f', 3));
    m_candleStyle->setCurrentIndex(std::clamp(m_renderer->candleStyle(), 0, 2));
    if (const int i = m_tpoTimeframeCombo->findData(m_renderer->tpoTimeframeMs()); i >= 0) m_tpoTimeframeCombo->setCurrentIndex(i);
    if (const int i = m_tpoSessionCombo->findData(m_renderer->tpoSessionType()); i >= 0) m_tpoSessionCombo->setCurrentIndex(i);
    m_tpoLayoutCombo->setCurrentIndex(std::max(0, m_tpoLayoutCombo->findData(m_renderer->tpoLayout())));
    m_tpoThemeCombo->setCurrentIndex(std::max(0, m_tpoThemeCombo->findData(m_renderer->tpoTheme())));
    m_loading = was;
}

void HeatmapSettingsDialog::showStatus(const QString &text, bool error) {
    m_status->setText(text);
    m_status->setStyleSheet(error ? "QLabel { color: #F07A6A; }" : "QLabel { color: #8198A6; }");
}

QString HeatmapSettingsDialog::statusText() const { return m_status->text(); }

void HeatmapSettingsDialog::logSettings() const {
    if (m_model)
        sLog_App("Heatmap settings chart=" << m_model->chartId() << " "
                 << QJsonDocument(heatmap::settingsJson(m_model->settings())).toJson(QJsonDocument::Compact));
    if (m_renderer)
        sLog_App("Heatmap tone: gamma=" << m_renderer->heatmapGamma() << " contrast=" << m_renderer->heatmapContrast()
                 << " shader_floor=" << m_renderer->heatmapShaderFloor());
}
