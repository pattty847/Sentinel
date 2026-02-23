#include "HeatmapSettingsDialog.hpp"
#include "../UnifiedGridRenderer.h"
#include "SentinelLogging.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>

namespace {
struct SliderWithLabel {
    QSlider* slider;
    QLabel* label;
};

SliderWithLabel makeSlider(QWidget* parent, int minVal, int maxVal, int defaultVal) {
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minVal, maxVal);
    slider->setValue(defaultVal);
    slider->setMinimumWidth(200);
    slider->setStyleSheet(
        "QSlider::groove:horizontal { background: #252A31; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #2B5A7A; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; }"
        "QSlider::handle:horizontal:hover { background: #3472A0; }"
    );

    auto* label = new QLabel(parent);
    label->setMinimumWidth(50);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setStyleSheet("QLabel { color: #E6EDF3; font-weight: 600; }");

    return {slider, label};
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

    auto* header = new QLabel("Heatmap Color Controls", this);
    header->setStyleSheet("QLabel { color: #B6C2CF; font-weight: 600; font-size: 14px; padding-bottom: 8px; }");
    layout->addWidget(header);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);

    // Gamma: 0.1-5.0, step 0.05, default 1.05
    auto [gammaSlider, gammaLabel] = makeSlider(this, 10, 500, 105);
    m_gammaSlider = gammaSlider;
    m_gammaLabel = gammaLabel;
    auto* gammaRow = new QHBoxLayout();
    gammaRow->addWidget(m_gammaSlider);
    gammaRow->addWidget(m_gammaLabel);
    form->addRow("Gamma", gammaRow);
    auto [contrastSlider, contrastLabel] = makeSlider(this, 10, 500, 115);
    m_contrastSlider = contrastSlider;
    m_contrastLabel = contrastLabel;
    auto* contrastRow = new QHBoxLayout();
    contrastRow->addWidget(m_contrastSlider);
    contrastRow->addWidget(m_contrastLabel);
    form->addRow("Contrast", contrastRow);

    // Floor: 0.0-0.5, step 0.01, default 0.01
    auto [floorSlider, floorLabel] = makeSlider(this, 0, 50, 1);
    m_floorSlider = floorSlider;
    m_floorLabel = floorLabel;
    auto* floorRow = new QHBoxLayout();
    floorRow->addWidget(m_floorSlider);
    floorRow->addWidget(m_floorLabel);
    form->addRow("Shader Floor", floorRow);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(this);
    m_logButton = buttons->addButton("Log Settings", QDialogButtonBox::ActionRole);
    auto* closeButton = buttons->addButton(QDialogButtonBox::Close);
    buttons->setStyleSheet(
        "QDialogButtonBox QPushButton { background-color: #2B5A7A; color: #FFFFFF; border: none; padding: 8px 16px; border-radius: 4px; }"
        "QDialogButtonBox QPushButton:hover { background-color: #3472A0; }"
    );
    layout->addWidget(buttons);

    // Connect sliders to apply functions (live preview!)
    connect(m_gammaSlider, &QSlider::valueChanged, this, &HeatmapSettingsDialog::applyGamma);
    connect(m_contrastSlider, &QSlider::valueChanged, this, &HeatmapSettingsDialog::applyContrast);
    connect(m_floorSlider, &QSlider::valueChanged, this, &HeatmapSettingsDialog::applyShaderFloor);
    connect(m_logButton, &QPushButton::clicked, this, &HeatmapSettingsDialog::logSettings);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
}

void HeatmapSettingsDialog::refreshFromRenderer() {
    if (!m_renderer) {
        return;
    }
    const QSignalBlocker blockGamma(m_gammaSlider);
    const QSignalBlocker blockContrast(m_contrastSlider);
    const QSignalBlocker blockFloor(m_floorSlider);

    m_gammaSlider->setValue(static_cast<int>(m_renderer->heatmapGamma() * 100));
    m_contrastSlider->setValue(static_cast<int>(m_renderer->heatmapContrast() * 100));
    m_floorSlider->setValue(static_cast<int>(m_renderer->heatmapShaderFloor() * 100));
    m_gammaLabel->setText(QString::number(m_renderer->heatmapGamma(), 'f', 2));
    m_contrastLabel->setText(QString::number(m_renderer->heatmapContrast(), 'f', 2));
    m_floorLabel->setText(QString::number(m_renderer->heatmapShaderFloor(), 'f', 3));
}

void HeatmapSettingsDialog::applyGamma(int sliderValue) {
    const double value = sliderValue / 100.0;
    m_gammaLabel->setText(QString::number(value, 'f', 2));
    if (m_renderer) {
        m_renderer->setHeatmapGamma(value);
    }
}

void HeatmapSettingsDialog::applyContrast(int sliderValue) {
    const double value = sliderValue / 100.0;
    m_contrastLabel->setText(QString::number(value, 'f', 2));
    if (m_renderer) {
        m_renderer->setHeatmapContrast(value);
    }
}

void HeatmapSettingsDialog::applyShaderFloor(int sliderValue) {
    const double value = sliderValue / 100.0;
    m_floorLabel->setText(QString::number(value, 'f', 3));
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
             << " (client config overrides)");
}
