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
#include <QQuickView>
#include <memory>
#include "../core/marketdata/MarketDataCore.hpp"
#include "../core/marketdata/auth/Authenticator.hpp"
#include "../core/marketdata/cache/DataCache.hpp"
#include "../core/SentinelMonitor.hpp"

// Forward declarations
class ChartModeController;

/**
 * 🚀 GPU-Powered Trading Terminal MainWindow
 * Clean, focused implementation for GPU rendering
 */
class MainWindowGPU : public QWidget {
    Q_OBJECT

public:
    explicit MainWindowGPU(QWidget* parent = nullptr);
    ~MainWindowGPU();

private slots:
    void onSubscribe();

private:
    void setupUI();
    void setupConnections();
    void initializeQMLComponents();
    void connectMarketDataSignals();

    // GPU CHART - Core component (QQuickView within a QWidget container)
    QQuickView* m_qquickView = nullptr;
    QWidget* m_qmlContainer = nullptr;
    
    // UI Controls
    QLabel* m_statusLabel;
    QLineEdit* m_symbolInput;
    QPushButton* m_subscribeButton;
    
    std::unique_ptr<MarketDataCore> m_marketDataCore;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DataCache> m_dataCache;
    std::shared_ptr<SentinelMonitor> m_sentinelMonitor;
    ChartModeController* m_modeController{nullptr};
};
