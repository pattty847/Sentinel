#include "ShortcutBinder.h"
#include "../widgets/HeatmapDock.hpp"
#include "../widgets/MarketDataPanel.hpp"
#include "../widgets/SecFilingDock.hpp"
#include <QShortcut>
#include <QKeySequence>
#include <QMainWindow>
#include <QObject>

ShortcutBinder::ShortcutBinder(QWidget* parent) : m_parent(parent) {
}

void ShortcutBinder::bindShortcuts(const Callbacks& callbacks, const DockWidgets& docks) {
    // Ctrl+S - Save current layout
    QShortcut* saveShortcut = new QShortcut(QKeySequence::Save, m_parent);
    QObject::connect(saveShortcut, &QShortcut::activated, [callbacks]() {
        if (callbacks.saveLayout) {
            callbacks.saveLayout();
        }
    });
    
    // Ctrl+L - Restore layout dialog
    QShortcut* loadShortcut = new QShortcut(QKeySequence("Ctrl+L"), m_parent);
    QObject::connect(loadShortcut, &QShortcut::activated, [callbacks]() {
        if (callbacks.restoreLayout) {
            callbacks.restoreLayout();
        }
    });
    
    // Ctrl+R - Reset to default layout
    QShortcut* resetShortcut = new QShortcut(QKeySequence("Ctrl+R"), m_parent);
    QObject::connect(resetShortcut, &QShortcut::activated, [callbacks]() {
        if (callbacks.resetLayout) {
            callbacks.resetLayout();
        }
    });
    
    // F1-F3 - Toggle docks
    QShortcut* f1Shortcut = new QShortcut(QKeySequence("F1"), m_parent);
    QObject::connect(f1Shortcut, &QShortcut::activated, [docks]() {
        if (docks.heatmapDock) {
            docks.heatmapDock->setVisible(!docks.heatmapDock->isVisible());
        }
    });
    
    QShortcut* f2Shortcut = new QShortcut(QKeySequence("F2"), m_parent);
    QObject::connect(f2Shortcut, &QShortcut::activated, [docks]() {
        if (docks.marketDataDock) {
            docks.marketDataDock->setVisible(!docks.marketDataDock->isVisible());
        }
    });
    
    QShortcut* f3Shortcut = new QShortcut(QKeySequence("F3"), m_parent);
    QObject::connect(f3Shortcut, &QShortcut::activated, [docks]() {
        if (docks.secDock) {
            docks.secDock->setVisible(!docks.secDock->isVisible());
        }
    });
    
    // F11 - Toggle fullscreen mode
    QShortcut* f11Shortcut = new QShortcut(QKeySequence("F11"), m_parent);
    QObject::connect(f11Shortcut, &QShortcut::activated, [this]() {
        QMainWindow* mainWindow = qobject_cast<QMainWindow*>(m_parent);
        if (mainWindow) {
            if (mainWindow->isFullScreen()) {
                mainWindow->showMaximized();
            } else {
                mainWindow->showFullScreen();
            }
        }
    });
}

