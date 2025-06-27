#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QtQuickWidgets/QQuickWidget>

// Forward declarations
class StreamController;
class StatisticsController;

/**
 * 🚀 GPU-Powered Trading Terminal MainWindow
 * Clean, focused implementation for Phase 0 GPU rendering
 */
class MainWindowGPU : public QWidget {
    Q_OBJECT

public:
    explicit MainWindowGPU(QWidget* parent = nullptr);
    ~MainWindowGPU() = default;

private slots:
    void onSubscribe();
    void onCVDUpdated(double cvd);

private:
    void setupUI();
    void setupConnections();
    void connectToGPUChart();  // 🔥 Helper to establish GPU connections

    // 🔥 GPU CHART - Core component
    QQuickWidget* m_gpuChart;
    
    // UI Controls
    QLabel* m_cvdLabel;
    QLabel* m_statusLabel;
    QLineEdit* m_symbolInput;
    QPushButton* m_subscribeButton;
    
    // Data controllers (keep existing proven pipeline)
    StreamController* m_streamController;
    StatisticsController* m_statsController;
}; 