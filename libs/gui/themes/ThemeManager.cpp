#include "ThemeManager.hpp"
#include "DarkTheme.hpp"
#include <QDebug>

ThemeManager& ThemeManager::instance() {
    static ThemeManager instance;
    return instance;
}

void ThemeManager::registerTheme(std::unique_ptr<ITheme> theme) {
    if (!theme) return;

    QString id = theme->id();
    m_themes[id] = std::move(theme);
}

bool ThemeManager::applyTheme(const QString& themeId, QApplication* app) {
    if (!app) return false;

    auto it = m_themes.find(themeId);
    if (it == m_themes.end()) return false;

    app->setStyleSheet(it->second->stylesheet());
    m_currentTheme = themeId;
    return true;
}

QStringList ThemeManager::availableThemes() const {
    QStringList themes;
    for (const auto& pair : m_themes) {
        themes << pair.first;
    }
    return themes;
}

QString ThemeManager::themeName(const QString& themeId) const {
    auto it = m_themes.find(themeId);
    if (it == m_themes.end()) {
        return QString();
    }
    return it->second->name();
}

void ThemeManager::initializeDefaults() {
    // Register built-in themes
    registerTheme(std::make_unique<DarkTheme>());
    
    // Future themes can be added here:
    // registerTheme(std::make_unique<LightTheme>());
    // registerTheme(std::make_unique<HighContrastTheme>());
    // registerTheme(std::make_unique<BlueTheme>());
}

