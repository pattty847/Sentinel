#include "DockablePanel.hpp"
#include <QWidget>

DockablePanel::DockablePanel(const QString& id, const QString& title, QWidget* parent)
    : QDockWidget(title, parent)
    , m_panelId(id)
{
    setObjectName(id);  // Persistent identifier for layout restoration
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    
    // Create content widget container
    m_contentWidget = new QWidget(this);
    setWidget(m_contentWidget);
}

