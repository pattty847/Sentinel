#include "ThemeBridge.hpp"
#include "ThemePalette.hpp"

void ThemeBridge::applyTheme(const QString& themeId) {
    if (themeId == "dark") {
        applyDark();
    } else {
        applyDark();
    }
    emit themeChanged();
}

void ThemeBridge::applyDark() {
    m_bg = theme::chartBackground();
    m_panel = QColor("#1B1F24");
    m_panelAlt = QColor("#20252B");
    m_border = theme::line();
    m_text = QColor("#D7DDE3");
    m_textDim = QColor("#93A1AF");
    m_accent = QColor("#2BB3FF");
    m_accentSoft = QColor("#1E4B66");
}
