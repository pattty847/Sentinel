#include "MenuBuilder.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QQuickItem>
#include "../widgets/LayoutManager.hpp"

MenuBuilder::MenuBuilder(QMenuBar* menuBar) : m_menuBar(menuBar) {
}

void MenuBuilder::buildMenus(const DockWidgets& docks, const Callbacks& callbacks) {
    buildViewMenu(docks);
    buildLayoutsMenu(callbacks);
    buildToolsMenu(callbacks);
    buildDebugMenu();
}

void MenuBuilder::buildViewMenu(const DockWidgets& docks) {
    m_viewMenu = m_menuBar->addMenu("&View");
    
    if (docks.heatmapDock) {
        m_viewMenu->addAction(docks.heatmapDock->toggleViewAction());
    }
    if (docks.secDock) {
        m_viewMenu->addAction(docks.secDock->toggleViewAction());
    }
    if (docks.copenetDock) {
        m_viewMenu->addAction(docks.copenetDock->toggleViewAction());
    }
    if (docks.aiCommentaryDock) {
        m_viewMenu->addAction(docks.aiCommentaryDock->toggleViewAction());
    }
}

void MenuBuilder::buildLayoutsMenu(const Callbacks& callbacks) {
    m_layoutsMenu = m_menuBar->addMenu("&Layouts");
    
    QAction* saveLayoutAction = m_layoutsMenu->addAction("&Save Current Layout...");
    QObject::connect(saveLayoutAction, &QAction::triggered, [callbacks]() {
        if (callbacks.saveLayout) {
            callbacks.saveLayout();
        }
    });
    
    QAction* restoreLayoutAction = m_layoutsMenu->addAction("&Restore Layout...");
    QObject::connect(restoreLayoutAction, &QAction::triggered, [callbacks]() {
        if (callbacks.restoreLayout) {
            callbacks.restoreLayout();
        }
    });
    
    QAction* resetLayoutAction = m_layoutsMenu->addAction("&Reset to Default Layout");
    QObject::connect(resetLayoutAction, &QAction::triggered, [callbacks]() {
        if (callbacks.resetLayout) {
            callbacks.resetLayout();
        }
    });
    
    m_layoutsMenu->addSeparator();
}

void MenuBuilder::buildToolsMenu(const Callbacks& callbacks) {
    m_toolsMenu = m_menuBar->addMenu("&Tools");
    
    QAction* openSecAction = m_toolsMenu->addAction("Open &SEC Filing Viewer");
    QObject::connect(openSecAction, &QAction::triggered, [callbacks]() {
        if (callbacks.openSecFilingViewer) {
            callbacks.openSecFilingViewer();
        }
    });
    
    QAction* openMarketDataAction = m_toolsMenu->addAction("Open &Market Data Panel");
    QObject::connect(openMarketDataAction, &QAction::triggered, [callbacks]() {
    });
    
    m_toolsMenu->addSeparator();
    QAction* settingsAction = m_toolsMenu->addAction("&Settings...");
    settingsAction->setEnabled(false);  // Placeholder for future
}

void MenuBuilder::buildDebugMenu() {
    m_debugMenu = m_menuBar->addMenu("&Debug");

    // GPU Stats overlay toggle
    QAction* gpuStatsAction = m_debugMenu->addAction("GPU Stats");
    gpuStatsAction->setCheckable(true);
    gpuStatsAction->setChecked(false);
    QObject::connect(gpuStatsAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showGpuStatsOverlay", checked);
            }
        }
    });

    // Data Pipeline overlay toggle
    QAction* dataPipelineAction = m_debugMenu->addAction("Data Pipeline");
    dataPipelineAction->setCheckable(true);
    dataPipelineAction->setChecked(false);
    QObject::connect(dataPipelineAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showDataPipelineOverlay", checked);
            }
        }
    });

    // Render Strategy overlay toggle
    QAction* renderStrategyAction = m_debugMenu->addAction("Render Strategy");
    renderStrategyAction->setCheckable(true);
    renderStrategyAction->setChecked(false);
    QObject::connect(renderStrategyAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showRenderStrategyOverlay", checked);
            }
        }
    });

    // Viewport Math overlay toggle
    QAction* viewportMathAction = m_debugMenu->addAction("Viewport Math");
    viewportMathAction->setCheckable(true);
    viewportMathAction->setChecked(false);
    QObject::connect(viewportMathAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showViewportMathOverlay", checked);
            }
        }
    });

    // Memory/Cache overlay toggle
    QAction* memoryCacheAction = m_debugMenu->addAction("Memory/Cache");
    memoryCacheAction->setCheckable(true);
    memoryCacheAction->setChecked(false);
    QObject::connect(memoryCacheAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showMemoryCacheOverlay", checked);
            }
        }
    });

    // Mode Flags overlay toggle
    QAction* modeFlagsAction = m_debugMenu->addAction("Mode Flags");
    modeFlagsAction->setCheckable(true);
    modeFlagsAction->setChecked(false);
    QObject::connect(modeFlagsAction, &QAction::toggled, [this](bool checked) {
        if (m_heatmapDock && m_heatmapDock->qquickView()) {
            QObject* renderer = m_heatmapDock->qquickView()->rootObject()->findChild<QObject*>("unifiedGridRenderer");
            if (renderer) {
                renderer->setProperty("showModeFlagsOverlay", checked);
            }
        }
    });
}

