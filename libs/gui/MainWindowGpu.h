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

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QCloseEvent>
#include <memory>
#include "../core/marketdata/MarketDataCore.hpp"
#include "../core/marketdata/auth/Authenticator.hpp"
#include "../core/marketdata/cache/DataCache.hpp"

// Forward declarations
class ChartModeController;
class UnifiedGridRenderer;

    // Include widget headers (needed for MOC to see full type definitions)
    #include "widgets/HeatmapDock.hpp"
    #include "widgets/StatusDock.hpp"
    #include "widgets/StatusBar.hpp"
    #include "widgets/MarketDataPanel.hpp"
    #include "widgets/SecFilingDock.hpp"
    #include "widgets/CopenetFeedDock.hpp"
    #include "widgets/AICommentaryFeedDock.hpp"

/**
 *  GPU-Powered Trading Terminal MainWindow
 * Clean, focused implementation for GPU rendering with dockable widgets
 */
class MainWindowGPU : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindowGPU(QWidget* parent = nullptr);
    ~MainWindowGPU();

signals:
    /**
     * Emitted when the active symbol changes.
     * All dock widgets can connect to this for symbol-aware behavior.
     */
    void symbolChanged(const QString& symbol);

private slots:
    void onSubscribe();
    void onConnectionStatusChanged(bool connected);
    void resetLayoutToDefault();  // Slot for LayoutManager to call

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUI();
    void setupConnections();
    void setupMenuBar();
    void setupShortcuts();
    void initializeQMLComponents();
    void connectMarketDataSignals();
    void initializeDataComponents();
    void loadQmlSource();
    void verifyGpuAcceleration();
    void applyStyles();
    void setWindowProperties();
    void updateSymbolInContext(const QString& symbol);
    void propagateSymbolChange(const QString& symbol);
    bool validateComponents();
    UnifiedGridRenderer* getUnifiedGridRenderer() const;
    QString graphicsApiName(QSGRendererInterface::GraphicsApi api);
    
    /**
     * Arrange all docks to their default positions.
     * Called during setupUI() and resetLayoutToDefault().
     */
    void arrangeDefaultLayout();

    // Dock widgets
    HeatmapDock* m_heatmapDock = nullptr;
    StatusDock* m_statusDock = nullptr;  // Central widget (can be minimal/empty)
    StatusBar* m_statusBar = nullptr;    // Bottom dock-like status bar
    MarketDataPanel* m_marketDataDock = nullptr;
    SecFilingDock* m_secDock = nullptr;
    CopenetFeedDock* m_copenetDock = nullptr;
    AICommentaryFeedDock* m_aiCommentaryDock = nullptr;
    
    // GPU CHART - Core component (now owned by HeatmapDock)
    QQuickView* m_qquickView = nullptr;  // Keep reference for compatibility
    QWidget* m_qmlContainer = nullptr;   // Keep reference for compatibility
    
    // UI Controls (now accessed through HeatmapDock)
    QLineEdit* m_symbolInput;
    QPushButton* m_subscribeButton;
    
    std::unique_ptr<MarketDataCore> m_marketDataCore;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DataCache> m_dataCache;
    ChartModeController* m_modeController{nullptr};
};
