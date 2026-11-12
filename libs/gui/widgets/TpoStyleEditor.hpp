#pragma once
// TPO style editor: a preset picker, the level-line reach and one row per
// style element (show, colour, opacity, width), TradingView-style. It edits a
// tpo::StyleModel and reports every change; it never touches the renderer.
#include "chart/core/TpoStyle.hpp"
#include <QWidget>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QSpinBox;
class QToolButton;

class TpoStyleEditor : public QWidget {
    Q_OBJECT
public:
    explicit TpoStyleEditor(QWidget* parent = nullptr);
    // Shows style without emitting anything.
    void setStyleModel(const tpo::StyleModel& style);
    tpo::StyleModel styleModel() const { return m_style; }
    static constexpr int kElements = 10;

signals:
    void styleEdited(const tpo::StyleModel& style);
    void presetChosen(const QString& preset);

private:
    struct Row {
        QCheckBox* visible = nullptr;
        QToolButton* color = nullptr;
        QSpinBox* opacity = nullptr;
        QDoubleSpinBox* width = nullptr;
    };
    void buildRow(int index, QGridLayout* grid);
    void readControls();
    void showColor(QToolButton* button, const tpo::Rgba& color);
    void syncPresetCombo();

    tpo::StyleModel m_style;
    QComboBox* m_preset = nullptr;
    QComboBox* m_reach = nullptr;
    std::array<Row, kElements> m_rows{};
    bool m_loading = false;
};
