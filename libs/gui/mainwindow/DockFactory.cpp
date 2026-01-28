#include "DockFactory.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/StatusBar.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../widgets/LabDock.hpp"
#include "../widgets/WatchlistDock.hpp"

DockFactory::DockFactory(QWidget* parent) : m_parent(parent) {
}

DockFactory::DockWidgets DockFactory::createDocks() {
    // Create bottom status bar
    m_docks.statusBar = new StatusBar(m_parent);
    
    // Create HeatmapDock (includes QML scene)
    m_docks.heatmapDock = new HeatmapDock(m_parent);
    
    // Create all other docks
    m_docks.secDock = new SecFilingDock(m_parent);
    m_docks.copenetDock = new CopenetFeedDock(m_parent);
    m_docks.aiCommentaryDock = new AICommentaryFeedDock(m_parent);
    m_docks.labDock = new LabDock(m_parent);
    m_docks.watchlistDock = new WatchlistDock(m_parent);
    
    // Keep minimum sizes aligned with each dock's own size hints.
    m_docks.heatmapDock->setMinimumSize(m_docks.heatmapDock->minimumSizeHint());
    m_docks.secDock->setMinimumSize(m_docks.secDock->minimumSizeHint());
    m_docks.labDock->setMinimumSize(m_docks.labDock->minimumSizeHint());
    m_docks.watchlistDock->setMinimumSize(m_docks.watchlistDock->minimumSizeHint());
    return m_docks;
}

DockFactory::SymbolControls DockFactory::getSymbolControls() const {
    SymbolControls controls;
    if (m_docks.heatmapDock) {
        controls.symbolInput = m_docks.heatmapDock->symbolInput();
        controls.subscribeButton = m_docks.heatmapDock->subscribeButton();
    }
    return controls;
}

