#include "LayoutOrchestrator.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../widgets/LabDock.hpp"
#include "../widgets/LayoutManager.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QScreen>
#include <QGuiApplication>
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
    arrangeDefaultLayout(docks);
}

bool LayoutOrchestrator::restoreLayout(const DockWidgets& docks, const QString& layoutName) {
    bool success = LayoutManager::restoreLayout(m_mainWindow, layoutName);
    if (success) {
        // Apply constraints after restore to ensure minimum sizes are respected
        applyDockConstraints(docks);
    }
    return success;
}

void LayoutOrchestrator::saveLayout(const QString& layoutName) {
    LayoutManager::saveLayout(m_mainWindow, layoutName);
}

void LayoutOrchestrator::configureDockOptions() {
    // Enable docking features - AllowNestedDocks enables side-by-side docking (Windows-style)
    const QString platform = QGuiApplication::platformName().toLower();
    const bool isWayland = platform.contains("wayland");
    QMainWindow::DockOptions options = QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks;
    if (!isWayland) {
        options |= QMainWindow::AnimatedDocks;
    }
    m_mainWindow->setDockOptions(options);
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
    if (docks.copenetDock && docks.copenetDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.copenetDock);
        docks.copenetDock->setFloating(false);
    }
    if (docks.aiCommentaryDock && docks.aiCommentaryDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.aiCommentaryDock);
        docks.aiCommentaryDock->setFloating(false);
    }
    if (docks.labDock && docks.labDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.labDock);
        docks.labDock->setFloating(false);
    }
}

void LayoutOrchestrator::addDocksToLayout(const DockWidgets& docks) {
    // TradingView-like: Heatmap/Chart as the dominant left pane
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, docks.heatmapDock);
    
    // Right: SEC Filing Viewer with Market Data tabbed to it
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.secDock);

    if (docks.labDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.labDock);
        m_mainWindow->tabifyDockWidget(docks.secDock, docks.labDock);
    }
    
    // Bottom: Commentary feeds (small height, split horizontally)
    // Add these AFTER the heatmap so they resize relative to it
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.copenetDock);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.aiCommentaryDock);
    m_mainWindow->tabifyDockWidget(docks.copenetDock, docks.aiCommentaryDock);
}

void LayoutOrchestrator::applyDockConstraints(const DockWidgets& docks) {
    auto applyMinimum = [](QDockWidget* dock, const QSize& fallback) {
        if (!dock) return;
        QSize minHint = dock->minimumSizeHint();
        if (!minHint.isValid() || minHint.isEmpty()) {
            minHint = fallback;
        }
        dock->setMinimumSize(minHint);
        // Reset maximum to allow resizing
        dock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    };
    
    const QSize fallback(260, 160);
    applyMinimum(docks.heatmapDock, QSize(420, 300));
    applyMinimum(docks.secDock, QSize(440, 380));
    applyMinimum(docks.copenetDock, fallback);
    applyMinimum(docks.aiCommentaryDock, fallback);
    applyMinimum(docks.labDock, QSize(360, 240));
}

void LayoutOrchestrator::setDockSizes(const DockWidgets& docks) {
    applyDockConstraints(docks);
    
    // Vertical split: 10% bottom, 90% main area
    m_mainWindow->resizeDocks({docks.copenetDock, docks.heatmapDock, docks.secDock}, {10, 90, 90}, Qt::Vertical);
    
    // Horizontal split: 70% heatmap (left), 30% SEC (right)
    m_mainWindow->resizeDocks({docks.heatmapDock, docks.secDock}, {70, 30}, Qt::Horizontal);
    
    // Tabbed docks share space equally
    m_mainWindow->resizeDocks({docks.secDock}, {1}, Qt::Horizontal);
    m_mainWindow->resizeDocks({docks.copenetDock, docks.aiCommentaryDock}, {1, 1}, Qt::Horizontal);
}

void LayoutOrchestrator::showAllDocks(const DockWidgets& docks) {
    if (docks.heatmapDock) docks.heatmapDock->show();
    if (docks.secDock) docks.secDock->show();
    if (docks.copenetDock) docks.copenetDock->show();
    if (docks.aiCommentaryDock) docks.aiCommentaryDock->show();
    if (docks.labDock) docks.labDock->show();
}

