#pragma once

#include <QDockWidget>
#include <QString>

/**
 * Base class for all dockable panels in Sentinel.
 * Provides consistent behavior, menu integration, and symbol change propagation.
 */
class DockablePanel : public QDockWidget {
    Q_OBJECT

public:
    explicit DockablePanel(const QString& id, const QString& title, QWidget* parent = nullptr);
    virtual ~DockablePanel() = default;
    
    QString panelId() const { return m_panelId; }
    
    /**
     * Pure virtual method for subclasses to build their UI.
     * Called after the dock widget is constructed.
     */
    virtual void buildUi() = 0;
    
    /**
     * Virtual hook for symbol change propagation.
     * Override in subclasses that need to respond to symbol changes.
     */
    virtual void onSymbolChanged(const QString& symbol) {}

protected:
    QString m_panelId;
    QWidget* m_contentWidget = nullptr;
};

