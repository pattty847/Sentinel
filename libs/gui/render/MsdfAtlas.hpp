/*
Sentinel — MsdfAtlas
Role: Builds MSDF glyph atlas + metrics with optional on-disk cache.
Threading: build() on GUI thread; image/metrics read on render thread.
*/
#pragma once

#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>

class MsdfAtlas {
public:
    struct Glyph {
        QRectF uv;        // Normalized UV in atlas (includes padding)
        QRectF bounds;    // Bounding rect relative to baseline (font pixels)
        float advance = 0.0f;
    };

    struct BuildParams {
        QString fontFamily;
        QString fontPath;
        QString charset;
        int fontPx = 0;
        float pxRange = 4.0f;
    };

    bool build(const BuildParams& params);
    bool isBuilt() const { return !m_image.isNull(); }
    const QImage& image() const { return m_image; }
    const Glyph& glyph(QChar c) const;
    int fontPx() const { return m_fontPx; }
    float pxRange() const { return m_pxRange; }
    int paddingPx() const { return m_paddingPx; }
    QString fontFamily() const { return m_fontFamily; }
    float ascentPx() const { return m_ascentPx; }
    float descentPx() const { return m_descentPx; }
    float lineHeightPx() const { return m_lineHeightPx; }
    float glyphTopPx() const { return m_glyphTopPx; }
    float glyphBottomPx() const { return m_glyphBottomPx; }

private:
    QString resolveFontPath(const BuildParams& params) const;
    QString cacheDirPath() const;
    QString cacheKey(const QString& fontPath, int fontPx, const QString& charset, float pxRange) const;
    bool loadFromCache(const QString& cacheBasePath, const QString& key);
    void saveCache(const QString& cacheBasePath, const QString& key) const;

    QImage m_image;
    QHash<QChar, Glyph> m_glyphs;
    QString m_fontFamily;
    int m_fontPx = 0;
    float m_pxRange = 4.0f;
    int m_paddingPx = 0;
    float m_ascentPx = 0.0f;
    float m_descentPx = 0.0f;
    float m_lineHeightPx = 0.0f;
    float m_glyphTopPx = 0.0f;
    float m_glyphBottomPx = 0.0f;
};
