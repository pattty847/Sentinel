#include "DockFactory.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/StatusBar.hpp"
#include "../widgets/SecFilingDock.hpp"
#include "../widgets/CopenetFeedDock.hpp"
#include "../widgets/AICommentaryFeedDock.hpp"
#include "../widgets/LabDock.hpp"

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
    
    // Set initial minimum sizes for better default layout
    // (will be updated in LayoutOrchestrator based on screen size)
    m_docks.heatmapDock->setMinimumWidth(800);
    m_docks.heatmapDock->setMinimumHeight(600);
    m_docks.secDock->setMinimumWidth(300);
    m_docks.labDock->setMinimumWidth(320);
    m_docks.labDock->setMinimumHeight(240);
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

