#pragma once

#include <QDialog>
#include <QPointer>

class UnifiedGridRenderer;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class QLabel;

class HeatmapSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit HeatmapSettingsDialog(UnifiedGridRenderer* renderer, QWidget* parent = nullptr);
    void setRenderer(UnifiedGridRenderer* renderer);
    void refreshFromRenderer();

private:
    void buildUi();
    void applyGamma(int sliderValue);
    void applyContrast(int sliderValue);
    void applyShaderFloor(int sliderValue);
    void logSettings() const;

    QPointer<UnifiedGridRenderer> m_renderer;
    QSlider* m_gammaSlider = nullptr;
    QLabel* m_gammaLabel = nullptr;
    QSlider* m_contrastSlider = nullptr;
    QLabel* m_contrastLabel = nullptr;
    QSlider* m_floorSlider = nullptr;
    QLabel* m_floorLabel = nullptr;
    QPushButton* m_logButton = nullptr;
};
