#pragma once
#include <QColor>

namespace theme {
inline QColor positive() { return QColor(46, 189, 133); }
inline QColor negative() { return QColor(246, 70, 93); }
inline QColor chartBackground() { return QColor::fromRgb(0xff0f1114); }
// Canvas and separator roles shared by QML chrome and chart overlays.
inline QColor bg0() { return chartBackground(); }
inline QColor bg1() { return QColor("#1B1F24"); }
inline QColor bg2() { return QColor("#20252B"); }
inline QColor text() { return QColor("#E6EDF3"); }
inline QColor text3() { return QColor("#93A1AF"); }
inline QColor textOff() { return QColor("#6E7580"); }
inline QColor accent() { return QColor("#00AAFF"); }
inline QColor sel() { return QColor("#2A4A6A"); }
inline QColor selLine() { return QColor("#3A6A9A"); }
inline QColor line() { return QColor::fromRgb(0xff2b3138); }
} // namespace theme
