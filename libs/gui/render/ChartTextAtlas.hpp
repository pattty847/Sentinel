#pragma once

#include <QString>

#include "MsdfAtlas.hpp"

class ChartTextAtlas {
public:
    struct BuildParams {
        QString fontFamily;
        QString fontPath;
        QString resourceFont;
        QString charset;
        int fontPx = 0;
        float pxRange = 4.0f;
    };

    bool build(const BuildParams& params);
    bool isBuilt() const { return m_atlas.isBuilt(); }
    const QImage& image() const { return m_atlas.image(); }
    const MsdfAtlas::Glyph& glyph(QChar c) const { return m_atlas.glyph(c); }
    int fontPx() const { return m_atlas.fontPx(); }
    float pxRange() const { return m_atlas.pxRange(); }
    int paddingPx() const { return m_atlas.paddingPx(); }
    QString fontFamily() const { return m_atlas.fontFamily(); }
    float ascentPx() const { return m_atlas.ascentPx(); }
    float descentPx() const { return m_atlas.descentPx(); }
    float lineHeightPx() const { return m_atlas.lineHeightPx(); }
    float glyphTopPx() const { return m_atlas.glyphTopPx(); }
    float glyphBottomPx() const { return m_atlas.glyphBottomPx(); }
    const MsdfAtlas& atlas() const { return m_atlas; }

private:
    QString ensureFontFile(const BuildParams& params) const;
    QString runtimeDir() const;

    MsdfAtlas m_atlas;
};
