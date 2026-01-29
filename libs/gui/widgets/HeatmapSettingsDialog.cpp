#include "HeatmapSettingsDialog.hpp"
#include "../UnifiedGridRenderer.h"
#include "SentinelLogging.hpp"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>

namespace {
QDoubleSpinBox* makeSpin(QWidget* parent, double minVal, double maxVal, double step, int decimals) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(minVal, maxVal);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setStyleSheet(
        "QDoubleSpinBox { background-color: #252A31; color: #E6EDF3; border: 1px solid #3A3F46; padding: 6px; }"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 14px; }"
    );
    return spin;
}
} // namespace

HeatmapSettingsDialog::HeatmapSettingsDialog(UnifiedGridRenderer* renderer, QWidget* parent)
    : QDialog(parent)
    , m_renderer(renderer) {
    setWindowTitle("Heatmap Settings");
    setModal(false);
    resize(360, 220);
    setStyleSheet("QDialog { background-color: #1B1F24; }");
    buildUi();
    refreshFromRenderer();
}

void HeatmapSettingsDialog::setRenderer(UnifiedGridRenderer* renderer) {
    m_renderer = renderer;
    refreshFromRenderer();
}

void HeatmapSettingsDialog::buildUi() {
    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel("Heatmap Rendering", this);
    header->setStyleSheet("QLabel { color: #B6C2CF; font-weight: 600; padding-bottom: 6px; }");
    layout->addWidget(header);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);

    m_gammaSpin = makeSpin(this, 0.1, 5.0, 0.05, 2);
    m_contrastSpin = makeSpin(this, 0.1, 5.0, 0.05, 2);
    m_floorSpin = makeSpin(this, 0.0, 0.5, 0.01, 3);

    form->addRow("Gamma", m_gammaSpin);
    form->addRow("Contrast", m_contrastSpin);
    form->addRow("Shader floor", m_floorSpin);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(this);
    m_logButton = buttons->addButton("Log settings", QDialogButtonBox::ActionRole);
    auto* closeButton = buttons->addButton(QDialogButtonBox::Close);
    buttons->setStyleSheet(
        "QDialogButtonBox QPushButton { background-color: #2B5A7A; color: #FFFFFF; border: none; padding: 6px 12px; border-radius: 4px; }"
        "QDialogButtonBox QPushButton:hover { background-color: #3472A0; }"
    );
    layout->addWidget(buttons);

    connect(m_gammaSpin, &QDoubleSpinBox::valueChanged, this, &HeatmapSettingsDialog::applyGamma);
    connect(m_contrastSpin, &QDoubleSpinBox::valueChanged, this, &HeatmapSettingsDialog::applyContrast);
    connect(m_floorSpin, &QDoubleSpinBox::valueChanged, this, &HeatmapSettingsDialog::applyShaderFloor);
    connect(m_logButton, &QPushButton::clicked, this, &HeatmapSettingsDialog::logSettings);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
}

void HeatmapSettingsDialog::refreshFromRenderer() {
    if (!m_renderer) {
        return;
    }
    const QSignalBlocker blockGamma(m_gammaSpin);
    const QSignalBlocker blockContrast(m_contrastSpin);
    const QSignalBlocker blockFloor(m_floorSpin);
    m_gammaSpin->setValue(m_renderer->heatmapGamma());
    m_contrastSpin->setValue(m_renderer->heatmapContrast());
    m_floorSpin->setValue(m_renderer->heatmapShaderFloor());
}

void HeatmapSettingsDialog::applyGamma(double value) {
    if (m_renderer) {
        m_renderer->setHeatmapGamma(value);
    }
}

void HeatmapSettingsDialog::applyContrast(double value) {
    if (m_renderer) {
        m_renderer->setHeatmapContrast(value);
    }
}

void HeatmapSettingsDialog::applyShaderFloor(double value) {
    if (m_renderer) {
        m_renderer->setHeatmapShaderFloor(value);
    }
}

void HeatmapSettingsDialog::logSettings() const {
    if (!m_renderer) {
        return;
    }
    sLog_App("Heatmap settings: gamma=" << m_renderer->heatmapGamma()
             << " contrast=" << m_renderer->heatmapContrast()
             << " shader_floor=" << m_renderer->heatmapShaderFloor()
             << " (env: SENTINEL_HEATMAP_GAMMA, SENTINEL_HEATMAP_CONTRAST, SENTINEL_HEATMAP_SHADER_FLOOR)");
}
