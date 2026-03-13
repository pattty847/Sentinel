#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <limits>
#include <vector>

struct ChartGlyphInstance {
    QRectF rect;
    QRectF uv;
    QColor color;
    QPointF debugAnchor;
    float debugBaselineY = std::numeric_limits<float>::quiet_NaN();
    bool debugShowAnchor = false;
};

struct ChartTextRun {
    enum class HorizontalAlign {
        Left,
        Center,
        Right,
    };

    enum class VerticalAlign {
        Top,
        Center,
        Bottom,
    };

    QString text;
    QPointF anchor;
    QColor color = Qt::white;
    float scale = 1.0f;
    HorizontalAlign hAlign = HorizontalAlign::Center;
    VerticalAlign vAlign = VerticalAlign::Center;
    bool useStableMetrics = false;
    bool pixelSnap = false;
};

struct ChartTextBucket {
    QColor color;
    std::vector<ChartGlyphInstance> glyphs;
};
