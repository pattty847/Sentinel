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
#include <QProcess>
#include <memory>
#include <optional>
#include "mainwindow/AgentApiTypes.hpp"
#include "mainwindow/LayoutOrchestrator.h"
#include "datasources/IGridDataSource.hpp"
#include "../core/trading/TradingTypes.hpp"

// Forward declarations
class ChartModeController;
class UnifiedGridRenderer;
class ChartDock;
class StatusBar;
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;
class LabDock;
class WatchlistDock;
class ScreenerDock;
class StockChartDock;
class OrderBookDock;
class PaperTradingDock;
class TopToolbar;
class ThemeBridge;
class HeatmapSettingsDialog;
class TradeInputManager;
class TradeBlotterDock;

class DockFactory;
class QmlSceneController;
class LayoutOrchestrator;
class MenuBuilder;
class ShortcutBinder;
class GuiApiServer;
class QDoubleSpinBox;
namespace AgentApi { struct ControlBody; }

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
    // Shared routing slot for screener and watchlist symbol selections.
    void onAssetSymbolSelected(const QString& symbol, const QString& assetType);

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
    bool subscribeSymbol(const QString& symbol);
    void selectTimeframe(int ms);
    AgentApi::ControlApply agentApiApplyControl(const QString& kind, const AgentApi::ControlBody& body);
    AgentApi::Metadata agentApiMetadata() const;
    AgentApi::StateSnapshot agentApiStateSnapshot() const;
    AgentApi::ViewportSnapshot agentApiViewportSnapshot() const;
    std::optional<AgentApi::CandleSnapshot> agentApiCandlesSnapshot(const AgentApi::ValidationResult& query) const;
    AgentApi::BookSnapshot agentApiBookSnapshot(int levels) const;
    AgentApi::TradesSnapshot agentApiTradesSnapshot(qint64 windowMs, int limit) const;
    void propagateSymbolChange(const QString& symbol);
    bool canRequestConfiguredHistoryForSymbol(const QString& symbol) const;
    void requestConfiguredHistoryForSymbol(const QString& symbol);
    void requestHeatmapHistoryForSymbol(const QString& symbol);
    void requestFootprintHistoryForSymbol(const QString& symbol);
    void requestTpoHistoryForSymbol(const QString& symbol);
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
    ChartDock* m_heatmapDock = nullptr;
    StatusBar* m_statusBar = nullptr;
    SecFilingDock* m_secDock = nullptr;
    CopenetFeedDock* m_copenetDock = nullptr;
    AICommentaryFeedDock* m_aiCommentaryDock = nullptr;
    LabDock* m_labDock = nullptr;
    WatchlistDock* m_watchlistDock = nullptr;
    ScreenerDock* m_screenerDock = nullptr;
    StockChartDock* m_stockChartDock = nullptr;
    OrderBookDock* m_orderBookDock = nullptr;
    PaperTradingDock* m_paperTradingDock = nullptr;

    // UI Controls (accessed through ChartDock)
    QLineEdit* m_symbolInput = nullptr;
    QToolButton* m_subscribeButton = nullptr;
    QString m_currentSymbol;
    bool m_connected = false;
    bool m_serverConfigReady = false;
    bool m_userSubscribed = false;
    QString m_agentApiSessionId;
    quint64 m_agentApiSelectionEpoch = 1;
    std::optional<qint64> m_heatmapReceivedAtMs;
    std::optional<qint64> m_candlesReceivedAtMs;
    std::optional<qint64> m_bookReceivedAtMs;
    std::optional<qint64> m_tradesReceivedAtMs;
    AgentApi::TradeTape m_agentApiTradeTape;
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
    std::unique_ptr<TradeInputManager> m_tradeInputManager;
    TradeBlotterDock* m_tradeBlotterDock = nullptr;
    QLabel* m_positionOverlayLabel = nullptr;
    QDoubleSpinBox* m_orderQtyInput = nullptr;

    bool m_firstShow = true;

    // Screener Python server subprocess (owned lifetime = window lifetime)
    QProcess* m_screenerProcess = nullptr;
    int       m_screenerRestartCount = 0;
    static constexpr int kMaxScreenerRestarts = 3;
    void startScreenerServer();
    void stopScreenerServer();
};
