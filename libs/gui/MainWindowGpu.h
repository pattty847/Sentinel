/*
Sentinel — MainWindowGpu
Role: Main QWidget-based window, hosting the QML GPU chart and native UI controls.
Inputs/Outputs: Manages the lifecycle of the CoinbaseStreamClient and UI controllers.
Threading: Runs on the main GUI thread; connects worker thread data signals to the QML scene.
Performance: UI setup is a one-time cost; not on the real-time data hot path.
Integration: Instantiated in main.cpp; hosts DepthChartView.qml in a QQuickWidget.
Observability: Logs lifecycle and connection status via qDebug.
Related: MainWindowGpu.cpp, CoinbaseStreamClient.hpp, DepthChartView.qml, UnifiedGridRenderer.h.
Assumptions: The hosted QML scene exposes a 'unifiedGridRenderer' object.
*/
/*
Sentinel — MainWindowGpu
Role: Main QWidget-based window, hosting the QML GPU chart and native UI controls.
Inputs/Outputs: Manages the lifecycle of the CoinbaseStreamClient and UI controllers.
Threading: Runs on the main GUI thread; connects worker thread data signals to the QML scene.
Performance: UI setup is a one-time cost; not on the real-time data hot path.
Integration: Uses CoinbaseStreamClient to create/manage MarketDataCore; wires it to the QML renderer.
Observability: Logs lifecycle and connection status via qDebug.
Related: MainWindowGpu.cpp, CoinbaseStreamClient.hpp, MarketDataCore.hpp, DepthChartView.qml.
Assumptions: The hosted QML scene exposes a 'unifiedGridRenderer' object.
*/
#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QtQuickWidgets/QQuickWidget>
#include <memory>
#include "../core/CoinbaseStreamClient.hpp"

// Forward declarations
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
    
    // 🔥 PHASE 1.1: Direct MarketDataCore connection (StreamController OBLITERATED)
    std::unique_ptr<CoinbaseStreamClient> m_client;
    StatisticsController* m_statsController;
    ChartModeController* m_modeController{nullptr};
    
    // Connection retry tracking
    int m_connectionRetryCount{0};
    static const int MAX_CONNECTION_RETRIES = 50;  // 5 seconds max
};
