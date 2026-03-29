#pragma once

#include <QObject>
#include <QColor>

class ThemeBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor bg READ bg NOTIFY themeChanged)
    Q_PROPERTY(QColor panel READ panel NOTIFY themeChanged)
    Q_PROPERTY(QColor panelAlt READ panelAlt NOTIFY themeChanged)
    Q_PROPERTY(QColor border READ border NOTIFY themeChanged)
    Q_PROPERTY(QColor text READ text NOTIFY themeChanged)
    Q_PROPERTY(QColor textDim READ textDim NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor accentSoft READ accentSoft NOTIFY themeChanged)

public:
    explicit ThemeBridge(QObject* parent = nullptr) : QObject(parent) {}

    void applyTheme(const QString& themeId);

    QColor bg() const { return m_bg; }
    QColor panel() const { return m_panel; }
    QColor panelAlt() const { return m_panelAlt; }
    QColor border() const { return m_border; }
    QColor text() const { return m_text; }
    QColor textDim() const { return m_textDim; }
    QColor accent() const { return m_accent; }
    QColor accentSoft() const { return m_accentSoft; }

signals:
    void themeChanged();

private:
    void applyDark();

    QColor m_bg;
    QColor m_panel;
    QColor m_panelAlt;
    QColor m_border;
    QColor m_text;
    QColor m_textDim;
    QColor m_accent;
    QColor m_accentSoft;
};
