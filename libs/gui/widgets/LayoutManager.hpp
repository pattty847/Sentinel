#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>

/**
 * Manages save/restore of dock widget layouts.
 * Uses QSettings with versioning for safe restoration across app versions.
 */
class LayoutManager {
public:
    static constexpr int APP_LAYOUT_VERSION = 1;  // Increment on breaking changes
    
    /**
     * Save the current window state as a named layout.
     */
    static void saveLayout(QMainWindow* window, const QString& layoutName);
    
    /**
     * Restore a named layout. Returns false if restore failed.
     * Falls back to default layout on failure.
     */
    static bool restoreLayout(QMainWindow* window, const QString& layoutName);
    
    /**
     * Get list of all saved layout names.
     */
    static QStringList availableLayouts();
    
    /**
     * Delete a saved layout.
     */
    static void deleteLayout(const QString& layoutName);
    
    /**
     * Reset window to default layout (clears saved state).
     */
    static void resetToDefault(QMainWindow* window);
    
    /**
     * Get the default layout name.
     */
    static QString defaultLayoutName() { return QString("default"); }
};

