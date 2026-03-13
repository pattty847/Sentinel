#include "LayoutOrchestrator.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../widgets/LabDock.hpp"
#include "../widgets/WatchlistDock.hpp"
#include "../widgets/ScreenerDock.hpp"
#include "../widgets/StockChartDock.hpp"
#include "../widgets/OrderBookDock.hpp"
#include "../widgets/PaperTradingDock.hpp"
#include "../widgets/LayoutManager.hpp"
#include <QScreen>
#include <QGuiApplication>
#include <QTabWidget>

LayoutOrchestrator::LayoutOrchestrator(QMainWindow* mainWindow) 
    : m_mainWindow(mainWindow) {
}

void LayoutOrchestrator::arrangeDefaultLayout(const DockWidgets& docks) {
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
    // Wayland: animated docks can glitch during layout restores.
    if (!isWayland) {
        options |= QMainWindow::AnimatedDocks;
    }
    m_mainWindow->setDockOptions(options);
    m_mainWindow->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
}

void LayoutOrchestrator::removeAllDocks(const DockWidgets& docks) {
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
    if (docks.watchlistDock && docks.watchlistDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.watchlistDock);
        docks.watchlistDock->setFloating(false);
    }
    if (docks.stockChartDock && docks.stockChartDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.stockChartDock);
        docks.stockChartDock->setFloating(false);
    }
    if (docks.orderBookDock && docks.orderBookDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.orderBookDock);
        docks.orderBookDock->setFloating(false);
    }
    if (docks.paperTradingDock && docks.paperTradingDock->parent() == m_mainWindow) {
        m_mainWindow->removeDockWidget(docks.paperTradingDock);
        docks.paperTradingDock->setFloating(false);
    }
}

void LayoutOrchestrator::addDocksToLayout(const DockWidgets& docks) {
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, docks.heatmapDock);

    // Place DOM adjacent to the chart (center/right of heatmap), not tabbed into the right-side stack.
    if (docks.orderBookDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.orderBookDock);
        m_mainWindow->splitDockWidget(docks.heatmapDock, docks.orderBookDock, Qt::Horizontal);
    }
    if (docks.watchlistDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.watchlistDock);
        if (docks.orderBookDock) {
            // Create a third (rightmost) column for the right-side stack.
            m_mainWindow->splitDockWidget(docks.orderBookDock, docks.watchlistDock, Qt::Horizontal);
        }
    }
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.secDock);
    if (!docks.watchlistDock && docks.orderBookDock) {
        // No watchlist: make SEC the anchor for the rightmost column.
        m_mainWindow->splitDockWidget(docks.orderBookDock, docks.secDock, Qt::Horizontal);
    }
    if (docks.watchlistDock) {
        m_mainWindow->tabifyDockWidget(docks.watchlistDock, docks.secDock);
    }

    if (docks.labDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.labDock);
        if (docks.watchlistDock) {
            m_mainWindow->tabifyDockWidget(docks.watchlistDock, docks.labDock);
        } else {
            m_mainWindow->tabifyDockWidget(docks.secDock, docks.labDock);
        }
    }
    if (docks.screenerDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.screenerDock);
        // Tab with the right-side group — screener sits alongside watchlist/SEC/lab
        if (docks.labDock) {
            m_mainWindow->tabifyDockWidget(docks.labDock, docks.screenerDock);
        } else if (docks.watchlistDock) {
            m_mainWindow->tabifyDockWidget(docks.watchlistDock, docks.screenerDock);
        } else {
            m_mainWindow->tabifyDockWidget(docks.secDock, docks.screenerDock);
        }
    }
    if (docks.stockChartDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.stockChartDock);
        // Tab alongside screener/lab/watchlist on the right
        if (docks.screenerDock) {
            m_mainWindow->tabifyDockWidget(docks.screenerDock, docks.stockChartDock);
        } else if (docks.labDock) {
            m_mainWindow->tabifyDockWidget(docks.labDock, docks.stockChartDock);
        } else {
            m_mainWindow->tabifyDockWidget(docks.secDock, docks.stockChartDock);
        }
    }
    if (docks.paperTradingDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.paperTradingDock);
        if (docks.stockChartDock) {
            m_mainWindow->tabifyDockWidget(docks.stockChartDock, docks.paperTradingDock);
        } else if (docks.screenerDock) {
            m_mainWindow->tabifyDockWidget(docks.screenerDock, docks.paperTradingDock);
        } else {
            m_mainWindow->tabifyDockWidget(docks.secDock, docks.paperTradingDock);
        }
    }
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
    applyMinimum(docks.watchlistDock, QSize(320, 360));
    applyMinimum(docks.orderBookDock, QSize(280, 360));
    applyMinimum(docks.paperTradingDock, QSize(360, 280));
}

void LayoutOrchestrator::setDockSizes(const DockWidgets& docks) {
    applyDockConstraints(docks);
    if (docks.watchlistDock) {
        m_mainWindow->resizeDocks({docks.copenetDock, docks.heatmapDock, docks.watchlistDock}, {10, 90, 90}, Qt::Vertical);
    } else {
        m_mainWindow->resizeDocks({docks.copenetDock, docks.heatmapDock, docks.secDock}, {10, 90, 90}, Qt::Vertical);
    }
    
    // Horizontal split:
    // - If DOM is present: charts + DOM take most width, right stack is separate.
    // - Otherwise: classic charts vs right pane.
    if (docks.orderBookDock) {
        m_mainWindow->resizeDocks({docks.heatmapDock, docks.orderBookDock}, {75, 25}, Qt::Horizontal);
    }
    if (docks.watchlistDock) {
        m_mainWindow->resizeDocks({docks.watchlistDock}, {1}, Qt::Horizontal);
    } else {
        m_mainWindow->resizeDocks({docks.secDock}, {1}, Qt::Horizontal);
    }
    if (docks.watchlistDock) {
        m_mainWindow->resizeDocks({docks.watchlistDock}, {1}, Qt::Horizontal);
    } else {
        m_mainWindow->resizeDocks({docks.secDock}, {1}, Qt::Horizontal);
    }
    m_mainWindow->resizeDocks({docks.copenetDock, docks.aiCommentaryDock}, {1, 1}, Qt::Horizontal);
}

void LayoutOrchestrator::showAllDocks(const DockWidgets& docks) {
    if (docks.heatmapDock) docks.heatmapDock->show();
    if (docks.orderBookDock) docks.orderBookDock->show();
    if (docks.secDock) docks.secDock->show();
    if (docks.copenetDock) docks.copenetDock->show();
    if (docks.aiCommentaryDock) docks.aiCommentaryDock->show();
    if (docks.labDock) docks.labDock->show();
    if (docks.watchlistDock) docks.watchlistDock->show();
    if (docks.stockChartDock) docks.stockChartDock->show();
    if (docks.paperTradingDock) docks.paperTradingDock->show();
}

