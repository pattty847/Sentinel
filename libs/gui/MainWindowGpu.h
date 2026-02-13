/* Main GUI window; main thread only. Hosts QML GPU chart and dockable widgets. */
#pragma once

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QCloseEvent>
#include <QPointer>
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
class WatchlistDock;
class ScreenerDock;
class TopToolbar;
class ThemeBridge;
class HeatmapSettingsDialog;

class DockFactory;
class QmlSceneController;
class LayoutOrchestrator;
class MenuBuilder;
class ShortcutBinder;
class GuiApiServer;

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
    void resetLayoutToDefault();

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
    void requestHeatmapHistoryForSymbol(const QString& symbol);
    void requestCandleHistoryForSymbol(const QString& symbol);
    bool validateComponents();
    LayoutOrchestrator::DockWidgets getDockWidgets() const;
    
    // Callbacks for modular components
    void onSaveLayout();
    void onRestoreLayout();
    void onResetLayout();
    void onOpenSecFilingViewer();
    void onOpenFontSettings();

    std::unique_ptr<IGridDataSource> m_dataSource;
    HeatmapDock* m_heatmapDock = nullptr;
    StatusBar* m_statusBar = nullptr;
    SecFilingDock* m_secDock = nullptr;
    CopenetFeedDock* m_copenetDock = nullptr;
    AICommentaryFeedDock* m_aiCommentaryDock = nullptr;
    LabDock* m_labDock = nullptr;
    WatchlistDock* m_watchlistDock = nullptr;
    ScreenerDock* m_screenerDock = nullptr;
    
    // UI Controls (accessed through HeatmapDock)
    QLineEdit* m_symbolInput = nullptr;
    QToolButton* m_subscribeButton = nullptr;
    QString m_currentSymbol;
    bool m_connected = false;
    bool m_userSubscribed = false;
    QQuickView* m_qquickView = nullptr;
    QWidget* m_qmlContainer = nullptr;
    
    // Controllers
    ChartModeController* m_modeController = nullptr;
    ThemeBridge* m_themeBridge = nullptr;
    std::unique_ptr<QmlSceneController> m_qmlController;
    std::unique_ptr<LayoutOrchestrator> m_layoutOrchestrator;
    std::unique_ptr<MenuBuilder> m_menuBuilder;
    std::unique_ptr<ShortcutBinder> m_shortcutBinder;
    std::unique_ptr<GuiApiServer> m_guiApiServer;
    QPointer<class FontSettingsDialog> m_fontDialog;
    QPointer<HeatmapSettingsDialog> m_heatmapSettingsDialog;
    QMetaObject::Connection m_candleViewportConn;

    bool m_firstShow = true;
};
