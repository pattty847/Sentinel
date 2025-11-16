#pragma once

#include "ITheme.hpp"
#include <QApplication>
#include <QString>
#include <QStringList>
#include <memory>
#include <map>

/**
 * Manages application themes.
 * Coordinates theme registration, application, and switching.
 */
class ThemeManager {
public:
    /**
     * Get the singleton instance.
     */
    static ThemeManager& instance();
    
    /**
     * Register a theme with the manager.
     */
    void registerTheme(std::unique_ptr<ITheme> theme);
    
    /**
     * Apply a theme by ID to the application.
     */
    bool applyTheme(const QString& themeId, QApplication* app);
    
    /**
     * Get list of available theme IDs.
     */
    QStringList availableThemes() const;
    
    /**
     * Get theme name by ID.
     */
    QString themeName(const QString& themeId) const;
    
    /**
     * Get the current active theme ID.
     */
    QString currentTheme() const { return m_currentTheme; }
    
    /**
     * Initialize default themes.
     * Call this after QApplication is created.
     */
    void initializeDefaults();

private:
    ThemeManager() = default;
    ~ThemeManager() = default;
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;
    
    std::map<QString, std::unique_ptr<ITheme>> m_themes;
    QString m_currentTheme;
};

