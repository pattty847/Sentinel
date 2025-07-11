#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QtQuickWidgets/QQuickWidget>
#include "unifiedviewport.h"
#include "zerolagarchitecture.h"
#include "dynamiclod.h"
#include "instanttestdata.h"

// Forward declarations
class StreamController;
class StatisticsController;
class GPUDataAdapter;
class ChartModeController;
class TestDataPlayer;

/**
 * 🚀 GPU-Powered Trading Terminal MainWindow
 * Clean, focused implementation for Phase 0 GPU rendering
 */
class MainWindowGPU : public QWidget {
    Q_OBJECT

public:
    explicit MainWindowGPU(bool testMode, QWidget* parent = nullptr);
    ~MainWindowGPU() = default;

private slots:
    void onSubscribe();
    void onCVDUpdated(double cvd);
    void onLodLevelChanged(int timeframe);  // 🎯 Handle fractal zoom LOD changes

private:
    void setupUI();
    void setupConnections();
    void connectToGPUChart();  // 🔥 Helper to establish GPU connections
    void populateHistoricalDataForFractalZoom();  // 🚀 NEW: Populate historical data for fractal zoom testing

    // 🔥 GPU CHART - Core component
    QQuickWidget* m_gpuChart;
    bool m_testMode;

    bool eventFilter(QObject* object, QEvent* event) override;

    ZeroLagArchitecture* m_zeroLagArch = nullptr;
    UnifiedViewport* m_unifiedViewport = nullptr;
    DynamicLOD* m_dynamicLOD = nullptr;
    InstantTestData* m_instantTestData = nullptr;
    
    // UI Controls
    QLabel* m_cvdLabel;
    QLabel* m_statusLabel;
    QLineEdit* m_symbolInput;
    QPushButton* m_subscribeButton;
    
    // Data controllers (keep existing proven pipeline)
    StreamController* m_streamController;
    StatisticsController* m_statsController;
    GPUDataAdapter* m_gpuAdapter{nullptr};
    ChartModeController* m_modeController{nullptr};
    
    // Test mode components
    TestDataPlayer* m_testDataPlayer{nullptr};
};