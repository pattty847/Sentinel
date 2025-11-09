#include "LayoutManager.hpp"
#include <QSettings>
#include <QByteArray>
#include <QDebug>

void LayoutManager::saveLayout(QMainWindow* window, const QString& layoutName) {
    if (!window) return;
    
    QSettings settings("Sentinel", "SentinelTerminal");
    settings.beginGroup("layouts");
    settings.beginGroup(layoutName);
    
    // Save version for compatibility checking
    settings.setValue("version", APP_LAYOUT_VERSION);
    
    // Save window state
    QByteArray state = window->saveState();
    settings.setValue("state", state);
    
    settings.endGroup();
    settings.endGroup();
    settings.sync();
}

bool LayoutManager::restoreLayout(QMainWindow* window, const QString& layoutName) {
    if (!window) return false;
    
    QSettings settings("Sentinel", "SentinelTerminal");
    settings.beginGroup("layouts");
    settings.beginGroup(layoutName);
    
    // Check version compatibility
    int savedVersion = settings.value("version", 0).toInt();
    if (savedVersion != APP_LAYOUT_VERSION) {
        qWarning() << "Layout version mismatch:" << savedVersion << "vs" << APP_LAYOUT_VERSION
                   << "- falling back to default layout";
        settings.endGroup();
        settings.endGroup();
        return false;
    }
    
    QByteArray state = settings.value("state").toByteArray();
    settings.endGroup();
    settings.endGroup();
    
    if (state.isEmpty()) {
        qWarning() << "Layout restore failed: empty state data";
        return false;
    }
    
    if (!window->restoreState(state)) {
        qWarning() << "Layout restore failed: restoreState() returned false - falling back to default layout";
        return false;
    }
    
    return true;
}

QStringList LayoutManager::availableLayouts() {
    QSettings settings("Sentinel", "SentinelTerminal");
    settings.beginGroup("layouts");
    QStringList layouts = settings.childGroups();
    settings.endGroup();
    return layouts;
}

void LayoutManager::deleteLayout(const QString& layoutName) {
    QSettings settings("Sentinel", "SentinelTerminal");
    settings.beginGroup("layouts");
    settings.remove(layoutName);
    settings.endGroup();
    settings.sync();
}

void LayoutManager::resetToDefault(QMainWindow* window) {
    if (!window) return;
    
    // Clear saved default layout
    deleteLayout(defaultLayoutName());
    
    // Call resetLayoutToDefault() slot on MainWindowGPU
    // This uses Qt's signal/slot mechanism to invoke the method
    QMetaObject::invokeMethod(window, "resetLayoutToDefault", Qt::QueuedConnection);
}

