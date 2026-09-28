#include "LayoutManager.hpp"
#include <QSettings>
#include <QByteArray>
#include "SentinelLogging.hpp"

void LayoutManager::saveLayout(QMainWindow* window, const QString& layoutName) {
    if (!window) return;
    
    QSettings settings("Sentinel", "SentinelTerminal");
    settings.beginGroup("layouts");
    settings.beginGroup(layoutName);
    
    settings.setValue("version", APP_LAYOUT_VERSION);
    
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
    
    int savedVersion = settings.value("version", 0).toInt();
    if (savedVersion != APP_LAYOUT_VERSION) {
        sLog_Warning("Layout version mismatch, falling back to default layout: layout=" << layoutName
                     << " savedVersion=" << savedVersion << " appVersion=" << APP_LAYOUT_VERSION);
        settings.endGroup();
        settings.beginGroup(layoutName);
        settings.remove(QString());
        settings.endGroup();
        settings.endGroup();
        return false;
    }
    
    QByteArray state = settings.value("state").toByteArray();
    settings.endGroup();
    settings.endGroup();
    
    if (state.isEmpty()) {
        sLog_Warning("Layout restore failed, empty state data: layout=" << layoutName);
        deleteLayout(layoutName);
        return false;
    }
    
    if (!window->restoreState(state)) {
        sLog_Warning("Layout restore failed, restoreState() returned false, falling back to default: layout="
                     << layoutName << " stateBytes=" << state.size());
        deleteLayout(layoutName);
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
    
    deleteLayout(defaultLayoutName());
    
    QMetaObject::invokeMethod(window, "resetLayoutToDefault", Qt::QueuedConnection);
}

