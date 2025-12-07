/*
Sentinel — MenuBuilder
Role: Builds and manages the menu bar for MainWindowGPU.
Inputs/Outputs: Takes dock widgets and callbacks, builds View/Layouts/Tools menus.
Threading: Runs on main GUI thread.
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU::setupMenuBar().
Observability: Logs menu actions via sLog_App.
Related: MainWindowGpu.cpp, LayoutManager.hpp.
*/
#pragma once

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QWidget>
#include <functional>

// Forward declarations
class HeatmapDock;
class MarketDataPanel;
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;

class MenuBuilder {
public:
    struct DockWidgets {
        HeatmapDock* heatmapDock = nullptr;
        MarketDataPanel* marketDataDock = nullptr;
        SecFilingDock* secDock = nullptr;
        CopenetFeedDock* copenetDock = nullptr;
        AICommentaryFeedDock* aiCommentaryDock = nullptr;
    };

    struct Callbacks {
        std::function<void()> saveLayout;
        std::function<void()> restoreLayout;
        std::function<void()> resetLayout;
        std::function<void()> openSecFilingViewer;
        std::function<void()> openMarketDataPanel;
    };

    explicit MenuBuilder(QMenuBar* menuBar);
    
    void buildMenus(const DockWidgets& docks, const Callbacks& callbacks);
    
private:
    void buildViewMenu(const DockWidgets& docks);
    void buildLayoutsMenu(const Callbacks& callbacks);
    void buildToolsMenu(const Callbacks& callbacks);
    
    QMenuBar* m_menuBar;
    QMenu* m_viewMenu = nullptr;
    QMenu* m_layoutsMenu = nullptr;
    QMenu* m_toolsMenu = nullptr;
};

