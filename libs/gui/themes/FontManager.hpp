#pragma once

#include <QStringList>
#include <QApplication>

class FontManager {
public:
    static FontManager& instance();

    void initialize(QApplication* app);
    QStringList availableFonts() const;
    QString currentFontFamily() const { return m_currentFontFamily; }
    bool applyFontFamily(const QString& family, QApplication* app);

private:
    FontManager() = default;
    QStringList loadResourceFonts();
    QStringList preferredSystemFonts() const;

    QStringList m_resourceFonts;
    QString m_currentFontFamily;
    bool m_initialized = false;
};
