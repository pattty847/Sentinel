/*
Sentinel — LayoutOrchestrator
Role: Manages dock widget layout arrangement and persistence.
Inputs/Outputs: Takes dock widgets, arranges them in default layout, handles save/restore.
Threading: Runs on main GUI thread.
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU during UI setup and layout reset.
Observability: Logs layout operations via sLog_App.
Related: MainWindowGpu.cpp, LayoutManager.hpp.
*/
#pragma once

#include <QMainWindow>
#include <QScreen>
#include <QApplication>
#include <functional>
#include <QDockWidget>

// Forward declarations
class ChartDock;
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;
class LabDock;
class WatchlistDock;
class ScreenerDock;
class StockChartDock;
class OrderBookDock;
class PaperTradingDock;

class LayoutOrchestrator {
public:
    struct DockWidgets {
        ChartDock* heatmapDock = nullptr;
        SecFilingDock* secDock = nullptr;
        CopenetFeedDock* copenetDock = nullptr;
        AICommentaryFeedDock* aiCommentaryDock = nullptr;
        LabDock* labDock = nullptr;
        WatchlistDock* watchlistDock = nullptr;
        ScreenerDock* screenerDock = nullptr;
        StockChartDock* stockChartDock = nullptr;
        OrderBookDock* orderBookDock = nullptr;
        PaperTradingDock* paperTradingDock = nullptr;
        // Heatmap telemetry (S6c): tabbed with the right column; its visibility is
        // the chart's showTelemetry setting, not the default layout.
        QDockWidget* heatmapTelemetryDock = nullptr;
        bool heatmapTelemetryVisible = false;
    };

    explicit LayoutOrchestrator(QMainWindow* mainWindow);
    
    void arrangeDefaultLayout(const DockWidgets& docks);
    void resetLayoutToDefault(const DockWidgets& docks);
    bool restoreLayout(const DockWidgets& docks, const QString& layoutName);
    void saveLayout(const QString& layoutName);
    void setHeatmapHooks(std::function<void(const QString &)> save, std::function<void(const QString &)> restore) {
        m_saveHeatmap = std::move(save); m_restoreHeatmap = std::move(restore);
    }
    
private:
    void configureDockOptions();
    void removeAllDocks(const DockWidgets& docks);
    void addDocksToLayout(const DockWidgets& docks);
    void applyDockConstraints(const DockWidgets& docks);
    void setDockSizes(const DockWidgets& docks);
    void showAllDocks(const DockWidgets& docks);
    
    QMainWindow* m_mainWindow;
    std::function<void(const QString &)> m_saveHeatmap, m_restoreHeatmap;
};

