/*
Sentinel — GlyphAtlas
*/
#include "GlyphAtlas.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>

void GlyphAtlas::build(const QFont& font, const QString& charset) {
    m_glyphs.clear();
    m_image = QImage();
    m_fontPx = font.pixelSize();
    if (charset.isEmpty()) {
        return;
    }

    QFontMetricsF metrics(font);
    float maxAdvance = 0.0f;
    float maxBoundsW = 0.0f;
    float maxBoundsH = 0.0f;
    for (const QChar c : charset) {
        const float advance = metrics.horizontalAdvance(c);
        const QRectF bounds = metrics.boundingRect(QString(c));
        maxAdvance = std::max(maxAdvance, advance);
        maxBoundsW = std::max(maxBoundsW, static_cast<float>(bounds.width()));
        maxBoundsH = std::max(maxBoundsH, static_cast<float>(bounds.height()));
    }

    const int padding = 1;
    const int cellW = static_cast<int>(std::ceil(std::max(maxAdvance, maxBoundsW))) + padding * 2;
    const int cellH = static_cast<int>(std::ceil(std::max(static_cast<float>(metrics.height()),
                                                          maxBoundsH))) + padding * 2;
    if (cellW <= 0 || cellH <= 0) {
        return;
    }

    const int atlasW = cellW * charset.size();
    const int atlasH = cellH;
    QImage atlas(atlasW, atlasH, QImage::Format_ARGB32_Premultiplied);
    atlas.fill(Qt::transparent);

    QPainter painter(&atlas);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::white);
    painter.setFont(font);

    for (int i = 0; i < charset.size(); ++i) {
        const QChar c = charset.at(i);
        const QRectF bounds = metrics.boundingRect(QString(c));
        const int cellX = i * cellW;
        const QPointF baseline(cellX + padding - bounds.left(),
                               padding - bounds.top());
        painter.drawText(baseline, QString(c));

        Glyph glyph;
        glyph.bounds = bounds;
        glyph.advance = metrics.horizontalAdvance(c);
        const QRectF pixelRect(cellX + padding, padding, bounds.width(), bounds.height());
        glyph.uv = QRectF(pixelRect.x() / atlasW,
                          pixelRect.y() / atlasH,
                          pixelRect.width() / atlasW,
                          pixelRect.height() / atlasH);
        m_glyphs.insert(c, glyph);
    }
    painter.end();

    m_image = atlas;
}

const GlyphAtlas::Glyph& GlyphAtlas::glyph(QChar c) const {
    static const Glyph empty;
    auto it = m_glyphs.constFind(c);
    if (it == m_glyphs.constEnd()) {
        return empty;
    }
    return it.value();
}
