#include "LayoutOrchestrator.h"
#include "../widgets/ChartDock.hpp"
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
#include <QCoreApplication>
#include <QEventLoop>
#include <QTabWidget>

LayoutOrchestrator::LayoutOrchestrator(QMainWindow* mainWindow) 
    : m_mainWindow(mainWindow) {
}

void LayoutOrchestrator::arrangeDefaultLayout(const DockWidgets& docks) {
    const QMainWindow::DockOptions previousOptions = m_mainWindow->dockOptions();
    m_mainWindow->setUpdatesEnabled(false);
    
    configureDockOptions();
    m_mainWindow->setDockOptions(m_mainWindow->dockOptions() & ~QMainWindow::AnimatedDocks);
    // Flush any in-flight dock animations before rearranging — tabifyDockWidget crashes
    // if called while an animation abort triggers animationFinished/showTabBars on a
    // widget that's no longer in a valid state.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    removeAllDocks(docks);
    addDocksToLayout(docks);
    applyDockConstraints(docks);
    setDockSizes(docks);
    showAllDocks(docks);
    
    m_mainWindow->setDockOptions((m_mainWindow->dockOptions() & ~QMainWindow::AnimatedDocks) |
                                 (previousOptions & QMainWindow::AnimatedDocks));
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
    auto remove = [this](QDockWidget* dock) {
        if (dock && dock->parent() == m_mainWindow) {
            m_mainWindow->removeDockWidget(dock);
            dock->setFloating(false);
        }
    };
    remove(docks.heatmapDock);
    remove(docks.orderBookDock);
    remove(docks.watchlistDock);
    remove(docks.secDock);
    remove(docks.labDock);
    remove(docks.screenerDock);
    remove(docks.stockChartDock);
    remove(docks.paperTradingDock);
    remove(docks.copenetDock);
    remove(docks.aiCommentaryDock);
}

void LayoutOrchestrator::addDocksToLayout(const DockWidgets& docks) {
    // Left column: heatmap
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, docks.heatmapDock);

    // Middle column: order book (narrow DOM, adjacent to heatmap)
    if (docks.orderBookDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.orderBookDock);
        m_mainWindow->splitDockWidget(docks.heatmapDock, docks.orderBookDock, Qt::Horizontal);
    }

    // Right column: tabbed stack.
    // Watchlist is the tab anchor; all others tabify onto it (or secDock if no watchlist).
    QDockWidget* rightAnchor = nullptr;
    if (docks.watchlistDock) {
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.watchlistDock);
        if (docks.orderBookDock) {
            m_mainWindow->splitDockWidget(docks.orderBookDock, docks.watchlistDock, Qt::Horizontal);
        }
        rightAnchor = docks.watchlistDock;
    }

    // secDock: anchor if no watchlist, otherwise tab onto watchlist
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, docks.secDock);
    if (!rightAnchor && docks.orderBookDock) {
        m_mainWindow->splitDockWidget(docks.orderBookDock, docks.secDock, Qt::Horizontal);
    }
    if (rightAnchor) {
        m_mainWindow->tabifyDockWidget(rightAnchor, docks.secDock);
    } else {
        rightAnchor = docks.secDock;
    }

    // Remaining right-column tabs: Lab, Screener, StockChart, PaperTrading
    auto tabifyRight = [&](QDockWidget* dock) {
        if (!dock) return;
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        m_mainWindow->tabifyDockWidget(rightAnchor, dock);
    };
    tabifyRight(docks.labDock);
    tabifyRight(docks.screenerDock);
    tabifyRight(docks.stockChartDock);
    tabifyRight(docks.paperTradingDock);

    // Bottom strip: CopeNet and AI Commentary — added to layout but hidden by default.
    // They are not production-ready; users can show them via the View menu.
    if (docks.copenetDock) {
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.copenetDock);
    }
    if (docks.aiCommentaryDock) {
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.aiCommentaryDock);
        if (docks.copenetDock) {
            m_mainWindow->tabifyDockWidget(docks.copenetDock, docks.aiCommentaryDock);
        }
    }
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
    applyMinimum(docks.heatmapDock,      QSize(420, 300));
    applyMinimum(docks.orderBookDock,    QSize(280, 360));
    applyMinimum(docks.watchlistDock,    QSize(320, 360));
    applyMinimum(docks.secDock,          QSize(440, 380));
    applyMinimum(docks.labDock,          QSize(360, 240));
    applyMinimum(docks.screenerDock,     QSize(360, 280));
    applyMinimum(docks.stockChartDock,   QSize(360, 280));
    applyMinimum(docks.paperTradingDock, QSize(360, 280));
    applyMinimum(docks.copenetDock,      fallback);
    applyMinimum(docks.aiCommentaryDock, fallback);
}

void LayoutOrchestrator::setDockSizes(const DockWidgets& docks) {
    applyDockConstraints(docks);

    // Horizontal: heatmap takes ~3/4, order book ~1/4 of their combined width.
    if (docks.orderBookDock) {
        m_mainWindow->resizeDocks({docks.heatmapDock, docks.orderBookDock}, {75, 25}, Qt::Horizontal);
    }

    // Give the right-side tab stack a minimal horizontal hint — it auto-fills the remainder.
    QDockWidget* rightAnchor = docks.watchlistDock ? static_cast<QDockWidget*>(docks.watchlistDock)
                                                   : docks.secDock;
    if (rightAnchor) {
        m_mainWindow->resizeDocks({rightAnchor}, {1}, Qt::Horizontal);
    }
}

void LayoutOrchestrator::showAllDocks(const DockWidgets& docks) {
    if (docks.heatmapDock)      docks.heatmapDock->show();
    if (docks.orderBookDock)    docks.orderBookDock->show();
    if (docks.watchlistDock)    docks.watchlistDock->show();
    if (docks.secDock)          docks.secDock->show();
    if (docks.labDock)          docks.labDock->show();
    if (docks.screenerDock)     docks.screenerDock->show();
    if (docks.stockChartDock)   docks.stockChartDock->show();
    if (docks.paperTradingDock) docks.paperTradingDock->show();

    // CopeNet and AI Commentary are in the layout but hidden by default — not ready for production.
    // Users can show them via the View menu.
    if (docks.copenetDock)      docks.copenetDock->hide();
    if (docks.aiCommentaryDock) docks.aiCommentaryDock->hide();
}

