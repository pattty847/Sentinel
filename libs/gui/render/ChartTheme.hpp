#pragma once
// ChartTheme: the theme palette as a QML singleton (Sentinel.Charts). QML chrome that must
// match the widget side reads its colours here instead of copying hex literals; the values
// come from ThemePalette.hpp, so there is one place to change them.

#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

#include "../themes/ThemePalette.hpp"

class ChartTheme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QColor tooltipText READ tooltipText CONSTANT)
    Q_PROPERTY(QColor tooltipBackground READ tooltipBackground CONSTANT)
    Q_PROPERTY(QColor tooltipBorder READ tooltipBorder CONSTANT)

public:
    explicit ChartTheme(QObject* parent = nullptr) : QObject(parent) {}

    QColor tooltipText() const { return theme::text2(); }
    QColor tooltipBackground() const { return theme::bg2(); }
    QColor tooltipBorder() const { return theme::line(); }
};
