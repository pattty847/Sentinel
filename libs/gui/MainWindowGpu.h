#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QtQuickWidgets/QQuickWidget>
#include "GridIntegrationAdapter.h"

// Forward declarations
class StreamController;
class StatisticsController;
class ChartModeController;

/**
 * 🚀 GPU-Powered Trading Terminal MainWindow
 * Clean, focused implementation for Phase 0 GPU rendering
 */
class MainWindowGPU : public QWidget {
    Q_OBJECT

public:
    explicit MainWindowGPU(QWidget* parent = nullptr);
    ~MainWindowGPU();

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
    
    // Phase 2: New single data pipeline architecture
    StreamController* m_streamController;
    StatisticsController* m_statsController;
    GridIntegrationAdapter* m_gridAdapter{nullptr};  // Replaces GPUDataAdapter
    ChartModeController* m_modeController{nullptr};
    
    // Connection retry tracking
    int m_connectionRetryCount{0};
    static const int MAX_CONNECTION_RETRIES = 50;  // 5 seconds max
};
