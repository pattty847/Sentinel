#pragma once
#include "render/TradeBubbleStyle.hpp"
#include <QWidget>
#include <array>
class QCheckBox;
class QToolButton;
class QSlider;
class QLabel;
class QDoubleSpinBox;
class QGridLayout;

// Same value-editing contract as TpoStyleEditor; persistence belongs to the dialog.
class TradeBubbleStyleEditor : public QWidget {
    Q_OBJECT
public:
    explicit TradeBubbleStyleEditor(QWidget* parent = nullptr);
    void setStyleModel(const trade_bubbles::StyleModel& style);
    trade_bubbles::StyleModel styleModel() const { return style_; }
signals:
    void styleEdited(const trade_bubbles::StyleModel& style);
private:
    struct Row {
        QCheckBox* visible;
        QToolButton* color;
        QSlider* opacity;
        QSlider* width;
        QLabel* opacityValue;
        QLabel* widthValue;
    };
    void buildRow(int index, QGridLayout* grid);
    void readControls();
    trade_bubbles::StyleModel style_;
    std::array<Row, 3> rows_{};
    QSlider* minimum_ = nullptr;
    QDoubleSpinBox* minimumValue_ = nullptr;
    bool loading_ = false, adjustingMinimumSlider_ = false;
};
