#include "FontManager.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QFontDatabase>
#include <QSettings>
#include <QFont>
#include <QFontInfo>

FontManager& FontManager::instance() {
    static FontManager instance;
    return instance;
}

void FontManager::initialize(QApplication* app) {
    if (m_initialized || !app) return;
    m_resourceFonts = loadResourceFonts();

    QSettings settings("Sentinel", "SentinelGUI");
    const QString saved = settings.value("ui/fontFamily").toString();
    if (!saved.isEmpty()) {
        applyFontFamily(saved, app);
    } else {
        const QStringList preferred = preferredSystemFonts();
        const QString fallback = !preferred.isEmpty() ? preferred.front()
                                                      : (m_resourceFonts.isEmpty() ? QString() : m_resourceFonts.front());
        if (!fallback.isEmpty()) {
            applyFontFamily(fallback, app);
        }
    }
    m_initialized = true;
}

QStringList FontManager::availableFonts() const {
    QStringList fonts;
    const QStringList preferred = preferredSystemFonts();
    for (const auto& name : preferred) {
        if (!fonts.contains(name)) fonts << name;
    }
    for (const auto& name : m_resourceFonts) {
        if (!fonts.contains(name)) fonts << name;
    }
    return fonts;
}

bool FontManager::applyFontFamily(const QString& family, QApplication* app) {
    if (!app || family.isEmpty()) return false;
    QFont font(family, 10);
    app->setFont(font);
    app->setStyleSheet(app->styleSheet());  // Force style refresh for existing widgets
    m_currentFontFamily = QFontInfo(font).family();

    sLog_App("Font applied: " << m_currentFontFamily);

    QSettings settings("Sentinel", "SentinelGUI");
    settings.setValue("ui/fontFamily", m_currentFontFamily);
    return true;
}

QStringList FontManager::loadResourceFonts() {
    QStringList families;
    const QStringList fontFiles = {
        ":/fonts/RobotoMono/RobotoMono-Regular.ttf",
        ":/fonts/RobotoMono/RobotoMono-Medium.ttf",
        ":/fonts/RobotoMono/RobotoMono-Bold.ttf",
        ":/fonts/RobotoMono/RobotoMono-Light.ttf",
        ":/fonts/RobotoMono/RobotoMono-SemiBold.ttf",
        ":/fonts/RobotoMono/RobotoMono-ExtraLight.ttf",
        ":/fonts/RobotoMono/RobotoMono-Thin.ttf",
        ":/fonts/RobotoMono/RobotoMono-Italic.ttf",
        ":/fonts/RobotoMono/RobotoMono-BoldItalic.ttf",
        ":/fonts/RobotoMono/RobotoMono-LightItalic.ttf",
        ":/fonts/RobotoMono/RobotoMono-MediumItalic.ttf",
        ":/fonts/RobotoMono/RobotoMono-SemiBoldItalic.ttf",
        ":/fonts/RobotoMono/RobotoMono-ExtraLightItalic.ttf",
        ":/fonts/RobotoMono/RobotoMono-ThinItalic.ttf"
    };

    for (const auto& path : fontFiles) {
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) continue;
        const QStringList loaded = QFontDatabase::applicationFontFamilies(id);
        for (const auto& family : loaded) {
            if (!families.contains(family)) {
                families << family;
            }
        }
    }
    return families;
}

QStringList FontManager::preferredSystemFonts() const {
    const QStringList preferred = {"Inter", "IBM Plex Sans", "Noto Sans", "Ubuntu", "DejaVu Sans", "Liberation Sans"};
    QStringList available;
    QFontDatabase db;
    for (const auto& name : preferred) {
        if (db.families().contains(name) && !available.contains(name)) {
            available << name;
        }
        if (available.size() >= 3) break;
    }
    return available;
}
