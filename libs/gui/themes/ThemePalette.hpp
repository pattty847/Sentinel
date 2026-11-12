#pragma once
#include <QColor>

namespace theme {
inline QColor positive() { return QColor(46, 189, 133); }
inline QColor negative() { return QColor(246, 70, 93); }
inline QColor chartBackground() { return QColor::fromRgb(0xff0f1114); }
// Canvas and separator roles shared by QML chrome and chart overlays.
inline QColor bg0() { return chartBackground(); }
inline QColor line() { return QColor::fromRgb(0xff2b3138); }
} // namespace theme
