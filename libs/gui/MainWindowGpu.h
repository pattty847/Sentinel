/*
Sentinel — MainWindowGpu
Role: Main QWidget-based window, hosting the QML GPU chart and native UI controls.
Inputs/Outputs: Manages the lifecycle of the remote data source and UI controllers.
Threading: Runs on the main GUI thread; connects worker thread data signals to the QML scene.
Performance: UI setup is a one-time cost; not on the real-time data hot path.
Integration: Wires the remote data source to the QML renderer.
Observability: Logs lifecycle and connection status via qDebug.
Related: MainWindowGpu.cpp, DepthChartView.qml.
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
#include "mainwindow/LayoutOrchestrator.h"
#include "datasources/IGridDataSource.hpp"

// Forward declarations
class ChartModeController;
class UnifiedGridRenderer;
class HeatmapDock;
class StatusBar;
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;
class LabDock;

class DockFactory;
class QmlSceneController;
class LayoutOrchestrator;
class MenuBuilder;
class ShortcutBinder;
class GuiApiServer;

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
    void showEvent(QShowEvent* event) override;

private:
    void setupUI();
    void setupMenuBar();
    void setupShortcuts();
    void setupConnections();
    void connectMarketDataSignals();
    void setWindowProperties();
    void setupGuiApiServer();
    void propagateSymbolChange(const QString& symbol);
    bool validateComponents();
    LayoutOrchestrator::DockWidgets getDockWidgets() const;
    
    // Callbacks for modular components
    void onSaveLayout();
    void onRestoreLayout();
    void onResetLayout();
    void onOpenSecFilingViewer();

    std::unique_ptr<IGridDataSource> m_dataSource;
    
    // Dock widgets (created via DockFactory)
    HeatmapDock* m_heatmapDock = nullptr;
    StatusBar* m_statusBar = nullptr;
    SecFilingDock* m_secDock = nullptr;
    CopenetFeedDock* m_copenetDock = nullptr;
    AICommentaryFeedDock* m_aiCommentaryDock = nullptr;
    LabDock* m_labDock = nullptr;
    
    // UI Controls (accessed through HeatmapDock)
    QLineEdit* m_symbolInput = nullptr;
    QPushButton* m_subscribeButton = nullptr;
    
    // QML scene (managed via QmlSceneController)
    QQuickView* m_qquickView = nullptr;
    QWidget* m_qmlContainer = nullptr;
    
    // Controllers
    ChartModeController* m_modeController = nullptr;
    
    // Modular components
    std::unique_ptr<QmlSceneController> m_qmlController;
    std::unique_ptr<LayoutOrchestrator> m_layoutOrchestrator;
    std::unique_ptr<MenuBuilder> m_menuBuilder;
    std::unique_ptr<ShortcutBinder> m_shortcutBinder;
    std::unique_ptr<GuiApiServer> m_guiApiServer;

    bool m_firstShow = true;
};
