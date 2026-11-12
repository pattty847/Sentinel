#include "TradeBubbleStyleEditor.hpp"
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <cmath>

namespace {
using Field = trade_bubbles::Element trade_bubbles::StyleModel::*;
constexpr Field fields[] = {&trade_bubbles::StyleModel::buy, &trade_bubbles::StyleModel::sell, &trade_bubbles::StyleModel::ring};
constexpr const char* keys[] = {"buy", "sell", "ring"};
constexpr const char* labels[] = {"Buy aggressor", "Sell aggressor", "Dark ring"};
}
TradeBubbleStyleEditor::TradeBubbleStyleEditor(QWidget* parent) : QWidget(parent) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    const char* headers[] = {"Show", "Colour", "Opacity", "", "Size (px)", ""};
    for (int c = 0; c < 6; ++c) grid->addWidget(new QLabel(headers[c], this), 0, c);
    for (int i = 0; i < 3; ++i) buildRow(i, grid);
    minimum_ = new QSlider(Qt::Horizontal, this);
    minimum_->setObjectName("bubbleStyle.minimumBaseSize");
    minimum_->setRange(0, 1000);
    minimum_->setAccessibleName("Minimum trade size in base units");
    minimum_->setToolTip("Minimum execution size for this product, in base units. Zero shows all sizes.");
    minimumValue_ = new QDoubleSpinBox(this);
    minimumValue_->setObjectName("bubbleStyle.minimumBaseSizeValue");
    minimumValue_->setRange(0, 1e12);
    minimumValue_->setDecimals(8);
    minimumValue_->setMinimumWidth(80);
    minimumValue_->setMaximumWidth(140);
    minimumValue_->setAccessibleName("Minimum trade size in base units");
    minimumValue_->setKeyboardTracking(false);
    minimumValue_->setToolTip(minimum_->toolTip());
    grid->addWidget(new QLabel("Minimum (base)", this), 4, 0, 1, 2);
    grid->addWidget(minimum_, 4, 2, 1, 2);
    grid->addWidget(minimumValue_, 4, 4, 1, 2);
    connect(minimum_, &QSlider::valueChanged, this, [this](int v) {
        if (loading_) return;
        adjustingMinimumSlider_ = true;
        minimumValue_->setValue(double(v) / 1000 * minimum_->property("scale").toDouble());
        adjustingMinimumSlider_ = false;
    });
    connect(minimumValue_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (loading_) return;
        style_.minimumBaseSize = v;
        setStyleModel(style_);
        emit styleEdited(style_);
    });
    setStyleModel(style_);
}
void TradeBubbleStyleEditor::buildRow(int i, QGridLayout* grid) {
    auto& row = rows_[i];
    const QString prefix = "bubbleStyle." + QString::fromLatin1(keys[i]) + '.';
    row.visible = new QCheckBox(labels[i], this);
    row.visible->setObjectName(prefix + "visible");
    row.visible->setToolTip("Show or hide " + QString::fromLatin1(labels[i]).toLower() + '.');
    row.color = new QToolButton(this);
    row.color->setObjectName(prefix + "color");
    row.color->setToolTip("Choose the " + QString::fromLatin1(labels[i]).toLower() + " colour.");
    row.color->setAccessibleName(row.color->toolTip());
    row.opacity = new QSlider(Qt::Horizontal, this);
    row.opacity->setObjectName(prefix + "opacity");
    row.opacity->setRange(0, 100);
    row.opacity->setMinimumWidth(50);
    row.opacity->setToolTip("Opacity from 0% (transparent) to 100% (opaque).");
    row.opacity->setAccessibleName(QString::fromLatin1(labels[i]) + " opacity");
    row.width = new QSlider(Qt::Horizontal, this);
    row.width->setObjectName(prefix + "width");
    row.width->setMinimumWidth(50);
    row.width->setRange(i == 2 ? 0 : 40, i == 2 ? 30 : 480);
    row.width->setToolTip(i == 2 ? "Ring width in logical pixels (0–3)." : "Maximum bubble radius in logical pixels (4–48).");
    row.width->setAccessibleName(QString::fromLatin1(labels[i]) + (i == 2 ? " width" : " maximum radius"));
    row.opacityValue = new QLabel(this);
    row.widthValue = new QLabel(this);
    grid->addWidget(row.visible, i+1, 0);
    grid->addWidget(row.color, i+1, 1);
    grid->addWidget(row.opacity, i+1, 2);
    grid->addWidget(row.opacityValue, i+1, 3);
    grid->addWidget(row.width, i+1, 4);
    grid->addWidget(row.widthValue, i+1, 5);
    connect(row.visible, &QCheckBox::toggled, this, [this] { readControls(); });
    connect(row.opacity, &QSlider::valueChanged, this, [this] { readControls(); });
    connect(row.width, &QSlider::valueChanged, this, [this] { readControls(); });
    connect(row.color, &QToolButton::clicked, this, [this, i] {
        auto& element = style_.*fields[i];
        const auto c = element.color;
        const QColor picked = QColorDialog::getColor(QColor(c.r,c.g,c.b), this, labels[i]);
        if (!picked.isValid()) return;
        element.color = {uint8_t(picked.red()), uint8_t(picked.green()), uint8_t(picked.blue()), 255};
        setStyleModel(style_);
        emit styleEdited(style_);
    });
}
void TradeBubbleStyleEditor::setStyleModel(const trade_bubbles::StyleModel& style) {
    loading_ = true;
    style_ = trade_bubbles::clamped(style);
    for (int i = 0; i < 3; ++i) {
        const auto& e = style_.*fields[i];
        auto& row = rows_[i];
        row.visible->setChecked(e.visible);
        const QString hex = QString::fromStdString(tpo::hexColor(e.color));
        row.color->setText(hex);
        row.color->setStyleSheet("QToolButton { border-bottom: 3px solid " + hex + "; }");
        row.opacity->setValue(int(std::lround(e.opacity * 100)));
        row.width->setValue(int(std::lround(e.width * 10)));
        row.opacityValue->setText(QString::number(row.opacity->value()) + '%');
        row.widthValue->setText(QString::number(e.width, 'f', 1) + " px");
    }
    const double n = style_.minimumBaseSize;
    // A product-scaled slider plus exact numeric entry supports small and large assets.
    const double scale = adjustingMinimumSlider_ ? minimum_->property("scale").toDouble() :
                         n > 0 ? std::pow(10.0, std::ceil(std::log10(std::min(n, 1e12) * 2))) : 1.0;
    minimum_->setProperty("scale", scale);
    minimum_->setValue(int(std::lround(std::clamp(n / scale * 1000, 0.0, 1000.0))));
    minimumValue_->setValue(n);
    loading_ = false;
}
void TradeBubbleStyleEditor::readControls() {
    if (loading_) return;
    for (int i = 0; i < 3; ++i) {
        auto& e = style_.*fields[i];
        e.visible = rows_[i].visible->isChecked();
        e.opacity = rows_[i].opacity->value() / 100.0;
        e.width = rows_[i].width->value() / 10.0;
    }
    setStyleModel(style_);
    emit styleEdited(style_);
}
