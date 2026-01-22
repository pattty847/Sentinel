/*
Sentinel — GlyphAtlas
Role: Builds a glyph atlas and per-glyph metrics for GPU label rendering.
Threading: build() on GUI thread; image/metrics read on render thread.
*/
#pragma once

#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>

class QFont;

class GlyphAtlas {
public:
    struct Glyph {
        QRectF uv;        // Normalized UV in atlas
        QRectF bounds;    // Bounding rect relative to baseline (font pixels)
        float advance = 0.0f;
    };

    void build(const QFont& font, const QString& charset);
    bool isBuilt() const { return !m_image.isNull(); }
    const QImage& image() const { return m_image; }
    const Glyph& glyph(QChar c) const;
    int fontPx() const { return m_fontPx; }

private:
    QImage m_image;
    QHash<QChar, Glyph> m_glyphs;
    int m_fontPx = 0;
};
