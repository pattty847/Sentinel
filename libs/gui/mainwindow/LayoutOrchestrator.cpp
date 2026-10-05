#include "LayoutOrchestrator.h"
#include "../widgets/ChartDock.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/WatchlistDock.hpp"
#include "../widgets/ScreenerDock.hpp"
#include "../widgets/StockChartDock.hpp"
#include "../widgets/OrderBookDock.hpp"
#include "../widgets/PaperTradingDock.hpp"
#include "../widgets/HeatmapTelemetryDock.hpp"
#include "../widgets/LayoutManager.hpp"
#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTabWidget>
#include <algorithm>

LayoutOrchestrator::LayoutOrchestrator(QMainWindow* mainWindow) 
    : m_mainWindow(mainWindow) {
}

QList<QPair<QString, QDockWidget*>> LayoutOrchestrator::apiDocks(const DockWidgets& docks) {
    return {{"heatmap", docks.heatmapDock}, {"orderBook", docks.orderBookDock},
            {"watchlist", docks.watchlistDock}, {"sec", docks.secDock}, {"copenet", docks.copenetDock},
            {"screener", docks.screenerDock},
            {"stockChart", docks.stockChartDock}, {"paperTrading", docks.paperTradingDock},
            {"telemetry", docks.heatmapTelemetryDock}};
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
    showAllDocks(docks);
    setDockSizes(docks);
    
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
        if (m_restoreHeatmap) m_restoreHeatmap(layoutName);
        applyDockConstraints(docks);
    }
    return success;
}

void LayoutOrchestrator::saveLayout(const QString& layoutName) {
    LayoutManager::saveLayout(m_mainWindow, layoutName);
    if (m_saveHeatmap) m_saveHeatmap(layoutName);
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
    remove(docks.screenerDock);
    remove(docks.stockChartDock);
    remove(docks.paperTradingDock);
    remove(docks.copenetDock);
    remove(docks.heatmapTelemetryDock);
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

    // In a compact default workspace, the DOM becomes a side tab so the chart
    // keeps a usable plot width. Saved layouts and later manual splits are left
    // alone; this applies only while arranging/resetting the default layout.
    if (rightAnchor && docks.orderBookDock && m_mainWindow->width() < 1200)
        m_mainWindow->tabifyDockWidget(rightAnchor, docks.orderBookDock);

    // Remaining right-column tabs: Screener, StockChart, PaperTrading
    auto tabifyRight = [&](QDockWidget* dock) {
        if (!dock) return;
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        m_mainWindow->tabifyDockWidget(rightAnchor, dock);
    };
    tabifyRight(docks.screenerDock);
    tabifyRight(docks.stockChartDock);
    tabifyRight(docks.paperTradingDock);
    tabifyRight(docks.heatmapTelemetryDock);

    // Bottom strip: CopeNet is available from the View menu.
    if (docks.copenetDock) {
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, docks.copenetDock);
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
    applyMinimum(docks.heatmapDock,      QSize(480, 300));
    applyMinimum(docks.orderBookDock,    QSize(280, 360));
    const int railMaximum = docks.watchlistDock ? docks.watchlistDock->maximumWidth() : QWIDGETSIZE_MAX;
    applyMinimum(docks.watchlistDock,    QSize(320, 360));
    if (docks.watchlistDock) docks.watchlistDock->setMaximumWidth(railMaximum);
    applyMinimum(docks.secDock,          QSize(440, 380));
    applyMinimum(docks.screenerDock,     QSize(360, 280));
    applyMinimum(docks.stockChartDock,   QSize(360, 280));
    applyMinimum(docks.paperTradingDock, QSize(360, 280));
    applyMinimum(docks.copenetDock,      fallback);
    if (docks.heatmapTelemetryDock) { // its scroll area hints too narrow a minimum for the table
        docks.heatmapTelemetryDock->setMinimumSize(HeatmapTelemetryDock::kMinimumWidth, 240);
        docks.heatmapTelemetryDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
}

void LayoutOrchestrator::setDockSizes(const DockWidgets& docks) {
    applyDockConstraints(docks);

    // Size the three columns together: independent hints redistribute the remaining
    // space and can stretch the DOM. Saved layouts and manual resizing stay untouched.
    QDockWidget* rightAnchor = docks.watchlistDock ? static_cast<QDockWidget*>(docks.watchlistDock)
                                                   : docks.secDock;
    if (docks.heatmapDock && docks.orderBookDock && rightAnchor) {
        int rightWidth = std::max(320, rightAnchor->minimumWidth());
        for (auto* tab : m_mainWindow->tabifiedDockWidgets(rightAnchor))
            if (!tab->isHidden()) rightWidth = std::max(rightWidth, tab->minimumWidth());
        if (m_mainWindow->tabifiedDockWidgets(rightAnchor).contains(docks.orderBookDock)) {
            const int chartWidth = std::max(480, m_mainWindow->width() - rightWidth - 8);
            m_mainWindow->resizeDocks({docks.heatmapDock, rightAnchor},
                                     {chartWidth, rightWidth}, Qt::Horizontal);
            return;
        }
        const int domWidth = 260;
        const int chartWidth = std::max(480, m_mainWindow->width() - rightWidth - domWidth - 12);
        m_mainWindow->resizeDocks({docks.heatmapDock, docks.orderBookDock, rightAnchor},
                                 {chartWidth, domWidth, rightWidth}, Qt::Horizontal);
    }
}

void LayoutOrchestrator::showAllDocks(const DockWidgets& docks) {
    if (docks.heatmapDock)      docks.heatmapDock->show();
    if (docks.orderBookDock)    docks.orderBookDock->show();
    if (docks.watchlistDock)    docks.watchlistDock->show();
    if (docks.secDock)          docks.secDock->show();
    if (docks.screenerDock)     docks.screenerDock->show();
    if (docks.stockChartDock)   docks.stockChartDock->show();
    if (docks.paperTradingDock) docks.paperTradingDock->show();

    // CopeNet is in the layout but hidden by default.
    // Users can show them via the View menu.
    if (docks.copenetDock)      docks.copenetDock->hide();
    if (docks.heatmapTelemetryDock) {
        docks.heatmapTelemetryDock->setVisible(docks.heatmapTelemetryVisible);
        if (docks.heatmapTelemetryVisible) docks.heatmapTelemetryDock->raise();
    }
}
