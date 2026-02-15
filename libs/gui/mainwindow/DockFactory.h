/*
Sentinel — DockFactory
Role: Creates and configures all dock widgets.
Inputs/Outputs: Takes parent widget, returns all dock widgets and symbol controls.
Threading: Runs on main GUI thread.
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU during UI setup.
Observability: None (creation is silent).
Related: MainWindowGpu.cpp, DockablePanel.hpp.
*/
#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QToolButton>

// Forward declarations
class HeatmapDock;
class StatusBar;
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;
class LabDock;
class WatchlistDock;
class ScreenerDock;
class StockChartDock;

class DockFactory {
public:
    struct DockWidgets {
        HeatmapDock* heatmapDock = nullptr;
        StatusBar* statusBar = nullptr;
        SecFilingDock* secDock = nullptr;
        CopenetFeedDock* copenetDock = nullptr;
        AICommentaryFeedDock* aiCommentaryDock = nullptr;
        LabDock* labDock = nullptr;
        WatchlistDock* watchlistDock = nullptr;
        ScreenerDock*    screenerDock    = nullptr;
        StockChartDock*  stockChartDock  = nullptr;
    };

    struct SymbolControls {
        QLineEdit* symbolInput = nullptr;
        QToolButton* subscribeButton = nullptr;
    };

    explicit DockFactory(QWidget* parent);
    
    DockWidgets createDocks();
    SymbolControls getSymbolControls() const;
    
private:
    QWidget* m_parent;
    DockWidgets m_docks;
};

