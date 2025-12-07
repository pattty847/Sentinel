#include "LayoutOrchestrator.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/MarketDataPanel.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../widgets/LayoutManager.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QScreen>
#include <QTabWidget>

LayoutOrchestrator::LayoutOrchestrator(QMainWindow* mainWindow) 
    : m_mainWindow(mainWindow) {
}

void LayoutOrchestrator::arrangeDefaultLayout(const DockWidgets& docks) {
    // Prevent repaint storms during layout changes
    m_mainWindow->setUpdatesEnabled(false);
    
    configureDockOptions();
    removeAllDocks(docks);
    addDocksToLayout(docks);
    applyDockConstraints(docks);
    setDockSizes(docks);
    showAllDocks(docks);
    
    m_mainWindow->setUpdatesEnabled(true);
}

void LayoutOrchestrator::resetLayoutToDefault(const DockWidgets& docks) {
    sLog_App("Resetting layout to default");
    arrangeDefaultLayout(docks);
    // Clear saved layout so next time it uses this fresh default
    LayoutManager::deleteLayout(LayoutManager::defaultLayoutName());
}

bool LayoutOrchestrator::restoreLayout(const DockWidgets& docks, const QString& layoutName) {
    // Ensure constraints (min/max) are applied even when restoring saved state
    applyDockConstraints(docks);
    return LayoutManager::restoreLayout(m_mainWindow, layoutName);
}

void LayoutOrchestrator::saveLayout(const QString& layoutName) {
    LayoutManager::saveLayout(m_mainWindow, layoutName);
}

void LayoutOrchestrator::configureDockOptions() {
    // Enable docking features - AllowNestedDocks enables side-by-side docking (Windows-style)
    m_mainWindow->setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);
    m_mainWindow->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
}

void LayoutOrchestrator::removeAllDocks(const DockWidgets& docks) {
    // Remove all docks from their current positions first
    // This ensures they're removed from any tab groups or nested layouts
    if (docks.heatmapDock && docks.heatmapDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.heatmapDock);
        docks.heatmapDock->setFloating(false);
    }
    if (docks.secDock && docks.secDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.secDock);
        docks.secDock->setFloating(false);
    }
    if (docks.marketDataDock && docks.marketDataDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.marketDataDock);
        docks.marketDataDock->setFloating(false);
    }
    if (docks.copenetDock && docks.copenetDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.copenetDock);
        docks.copenetDock->setFloating(false);
    }
    if (docks.aiCommentaryDock && docks.aiCommentaryDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.aiCommentaryDock);
        docks.aiCommentaryDock->setFloating(false);
    }
}

void LayoutOrchestrator::addDocksToLayout(const DockWidgets& docks) {
    // TradingView-like: Heatmap/Chart as the dominant left pane
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, docks.heatmapDock);
    
    // Right: SEC Filing Viewer with Market Data tabbed to it
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.secDock);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.marketDataDock);
    m_mainWindow->tabifyDockWidget(docks.secDock, docks.marketDataDock);  // Tab them together
    
    // Bottom: Commentary feeds (small height, split horizontally)
    // Add these AFTER the heatmap so they resize relative to it
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.copenetDock);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.aiCommentaryDock);
    m_mainWindow->tabifyDockWidget(docks.copenetDock, docks.aiCommentaryDock);
}

void LayoutOrchestrator::applyDockConstraints(const DockWidgets& docks) {
    // Get screen geometry for percentage-based sizing
    QScreen* screen = QApplication::primaryScreen();
    if (!screen) {
        sLog_Warning("Could not get primary screen, using fallback sizes");
        screen = m_mainWindow->screen();
    }
    
    QRect screenGeometry = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    int screenWidth = screenGeometry.width();
    int screenHeight = screenGeometry.height();
    
    // Calculate dock sizes as percentages of screen size
    // Heatmap left: target ~70% width, right column ~30%
    int heatmapMinWidth = static_cast<int>(screenWidth * 0.60);      // ensure it stays dominant
    int rightDockMinWidth = static_cast<int>(screenWidth * 0.25);    // comfortable sidebar
    // Bottom area: 10% of screen height (for commentary feeds) - keep this small
    int bottomDockHeight = static_cast<int>(screenHeight * 0.10);
    
    // Update minimum sizes based on screen percentages for better proportional layout
    if (docks.heatmapDock) {
        docks.heatmapDock->setMinimumWidth(heatmapMinWidth);               // Dominant width
        docks.heatmapDock->setMinimumHeight(static_cast<int>(screenHeight * 0.80)); // Prefer tall main area
    }
    if (docks.secDock) {
        docks.secDock->setMinimumWidth(rightDockMinWidth);
    }
    if (docks.marketDataDock) {
        docks.marketDataDock->setMinimumWidth(rightDockMinWidth);
    }
    // Bottom docks: Set small minimum height and maximum height to constrain them to 10%
    if (docks.copenetDock) {
        docks.copenetDock->setMinimumHeight(50);  // Small minimum (50px)
        docks.copenetDock->setMaximumHeight(bottomDockHeight);  // Cap at 10% of screen
    }
    if (docks.aiCommentaryDock) {
        docks.aiCommentaryDock->setMinimumHeight(50);  // Small minimum (50px)
        docks.aiCommentaryDock->setMaximumHeight(bottomDockHeight);  // Cap at 10% of screen
    }
}

void LayoutOrchestrator::setDockSizes(const DockWidgets& docks) {
    applyDockConstraints(docks);
    
    // Set proportional sizes based on screen dimensions
    // Use resizeDocks with relative proportions to control the split between areas
    
    // CRITICAL: Resize bottom vs main area FIRST to establish the vertical split (10% bottom, 90% main)
    // Include both top-area docks so they share the taller region evenly
    m_mainWindow->resizeDocks({docks.copenetDock, docks.heatmapDock, docks.secDock}, {10, 90, 90}, Qt::Vertical);
    
    // Left (heatmap) vs Right (SEC/MarketData): ~70/30 split
    m_mainWindow->resizeDocks({docks.heatmapDock, docks.secDock}, {70, 30}, Qt::Horizontal);
    
    // Left dock area: tabbed docks share space equally
    m_mainWindow->resizeDocks({docks.secDock, docks.marketDataDock}, {1, 1}, Qt::Horizontal);
    
    // Bottom dock area: split equally (tabbed docks share space)
    m_mainWindow->resizeDocks({docks.copenetDock, docks.aiCommentaryDock}, {1, 1}, Qt::Horizontal);
}

void LayoutOrchestrator::showAllDocks(const DockWidgets& docks) {
    if (docks.heatmapDock) docks.heatmapDock->show();
    if (docks.secDock) docks.secDock->show();
    if (docks.marketDataDock) docks.marketDataDock->show();
    if (docks.copenetDock) docks.copenetDock->show();
    if (docks.aiCommentaryDock) docks.aiCommentaryDock->show();
}

