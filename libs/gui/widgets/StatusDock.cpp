#include "StatusDock.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>

StatusDock::StatusDock(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void StatusDock::setupUi() {
    // StatusDock is now minimal - status info moved to bottom StatusBar
    // Keep this widget as a placeholder for the central area
    // Can be used for future dashboard features if needed
    setStyleSheet("StatusDock { background-color: transparent; }");
    
    // No labels needed - status is in bottom bar
}

