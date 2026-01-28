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
class SecFilingDock;
class CopenetFeedDock;
class AICommentaryFeedDock;
class LabDock;
class WatchlistDock;

class MenuBuilder {
public:
    struct DockWidgets {
        HeatmapDock* heatmapDock = nullptr;
        SecFilingDock* secDock = nullptr;
        CopenetFeedDock* copenetDock = nullptr;
        AICommentaryFeedDock* aiCommentaryDock = nullptr;
        LabDock* labDock = nullptr;
        WatchlistDock* watchlistDock = nullptr;
    };

    struct Callbacks {
        std::function<void()> saveLayout;
        std::function<void()> restoreLayout;
        std::function<void()> resetLayout;
        std::function<void()> openSecFilingViewer;
        std::function<void()> openFontSettings;
    };

    explicit MenuBuilder(QMenuBar* menuBar);

    void buildMenus(const DockWidgets& docks, const Callbacks& callbacks);
    void setHeatmapDock(HeatmapDock* heatmapDock) { m_heatmapDock = heatmapDock; }

private:
    void buildViewMenu(const DockWidgets& docks);
    void buildLayoutsMenu(const Callbacks& callbacks);
    void buildToolsMenu(const Callbacks& callbacks);
    void buildDebugMenu();

    QMenuBar* m_menuBar;
    QMenu* m_viewMenu = nullptr;
    QMenu* m_layoutsMenu = nullptr;
    QMenu* m_toolsMenu = nullptr;
    QMenu* m_debugMenu = nullptr;
    HeatmapDock* m_heatmapDock = nullptr;
};

