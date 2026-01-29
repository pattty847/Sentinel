#pragma once

#include <QDialog>
#include <QPointer>

class UnifiedGridRenderer;
class QDoubleSpinBox;
class QPushButton;

class HeatmapSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit HeatmapSettingsDialog(UnifiedGridRenderer* renderer, QWidget* parent = nullptr);
    void setRenderer(UnifiedGridRenderer* renderer);
    void refreshFromRenderer();

private:
    void buildUi();
    void applyGamma(double value);
    void applyContrast(double value);
    void applyShaderFloor(double value);
    void logSettings() const;

    QPointer<UnifiedGridRenderer> m_renderer;
    QDoubleSpinBox* m_gammaSpin = nullptr;
    QDoubleSpinBox* m_contrastSpin = nullptr;
    QDoubleSpinBox* m_floorSpin = nullptr;
    QPushButton* m_logButton = nullptr;
};
