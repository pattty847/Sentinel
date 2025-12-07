#include "MenuBuilder.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/MarketDataPanel.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include "../widgets/LayoutManager.hpp"

MenuBuilder::MenuBuilder(QMenuBar* menuBar) : m_menuBar(menuBar) {
}

void MenuBuilder::buildMenus(const DockWidgets& docks, const Callbacks& callbacks) {
    buildViewMenu(docks);
    buildLayoutsMenu(callbacks);
    buildToolsMenu(callbacks);
}

void MenuBuilder::buildViewMenu(const DockWidgets& docks) {
    m_viewMenu = m_menuBar->addMenu("&View");
    
    if (docks.heatmapDock) {
        m_viewMenu->addAction(docks.heatmapDock->toggleViewAction());
    }
    if (docks.marketDataDock) {
        m_viewMenu->addAction(docks.marketDataDock->toggleViewAction());
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
        if (callbacks.openMarketDataPanel) {
            callbacks.openMarketDataPanel();
        }
    });
    
    m_toolsMenu->addSeparator();
    QAction* settingsAction = m_toolsMenu->addAction("&Settings...");
    settingsAction->setEnabled(false);  // Placeholder for future
}

