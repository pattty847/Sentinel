/*
Sentinel — LabTextItem
*/
#include "LabTextItem.hpp"

#include <QQuickWindow>
#include <QtMath>
#include <algorithm>
#include <limits>

LabTextItem::LabTextItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void LabTextItem::setText(const QString& text) {
    if (m_text == text) {
        return;
    }
    m_text = text;
    markGeometryDirty();
    emit textChanged();
}

void LabTextItem::setScale(float scale) {
    if (qFuzzyCompare(m_scale, scale)) {
        return;
    }
    m_scale = scale;
    markGeometryDirty();
    emit scaleChanged();
}

void LabTextItem::setColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    update();
    emit colorChanged();
}

void LabTextItem::setFontFamily(const QString& fontFamily) {
    if (m_fontFamily == fontFamily) {
        return;
    }
    m_fontFamily = fontFamily;
    markAtlasDirty();
    emit fontFamilyChanged();
}

void LabTextItem::setFontPixelSize(int fontPx) {
    if (m_fontPx == fontPx) {
        return;
    }
    m_fontPx = fontPx;
    markAtlasDirty();
    emit fontPixelSizeChanged();
}

void LabTextItem::setPixelRange(float pxRange) {
    if (qFuzzyCompare(m_pxRange, pxRange)) {
        return;
    }
    m_pxRange = pxRange;
    markAtlasDirty();
    emit pixelRangeChanged();
}

void LabTextItem::setCharset(const QString& charset) {
    if (m_charset == charset) {
        return;
    }
    m_charset = charset;
    markAtlasDirty();
    emit charsetChanged();
}

void LabTextItem::updatePolish() {
    if (m_atlasDirty) {
        rebuildAtlas();
    }
}

void LabTextItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        markGeometryDirty();
    }
}

QSGNode* LabTextItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<MsdfGlyphNode*>(oldNode);
    if (!node) {
        node = new MsdfGlyphNode();
    }

    if (!m_atlas.isBuilt()) {
        return node;
    }

    if (m_geometryDirty) {
        rebuildGeometry();
        node->ensureCapacity(static_cast<int>(m_quads.size()));
        node->updateGeometry(m_quads);
    }

    node->setAtlas(m_atlas.image(), window());
    node->setColor(m_color);
    node->setPxRange(m_atlas.pxRange());

    return node;
}

void LabTextItem::markAtlasDirty() {
    m_atlasDirty = true;
    polish();
    update();
}

void LabTextItem::markGeometryDirty() {
    m_geometryDirty = true;
    update();
}

void LabTextItem::rebuildAtlas() {
    MsdfAtlas::BuildParams params;
    params.fontFamily = m_fontFamily;
    params.fontPx = m_fontPx;
    params.pxRange = m_pxRange;
    params.charset = m_charset;
    if (m_atlas.build(params)) {
        m_atlasDirty = false;
        m_geometryDirty = true;
    }
}

void LabTextItem::rebuildGeometry() {
    m_geometryDirty = false;
    m_quads.clear();

    if (!m_atlas.isBuilt() || m_text.isEmpty()) {
        return;
    }

    const float scale = m_scale;
    const float padding = static_cast<float>(m_atlas.paddingPx());

    float penX = 0.0f;
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    for (const QChar c : m_text) {
        const auto& glyph = m_atlas.glyph(c);
        if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
            continue;
        }
        minX = std::min(minX, penX + static_cast<float>(glyph.bounds.left()) - padding);
        maxX = std::max(maxX, penX + static_cast<float>(glyph.bounds.right()) + padding);
        minY = std::min(minY, static_cast<float>(glyph.bounds.top()) - padding);
        maxY = std::max(maxY, static_cast<float>(glyph.bounds.bottom()) + padding);
        penX += glyph.advance;
    }

    if (!std::isfinite(minX) || !std::isfinite(maxX)) {
        return;
    }

    const float centerX = static_cast<float>(width()) * 0.5f;
    const float centerY = static_cast<float>(height()) * 0.5f;
    const float originX = centerX - (minX + maxX) * 0.5f * scale;
    const float originY = centerY - (minY + maxY) * 0.5f * scale;

    penX = 0.0f;
    for (const QChar c : m_text) {
        const auto& glyph = m_atlas.glyph(c);
        if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
            penX += glyph.advance;
            continue;
        }

        const float x0 = originX + (penX + glyph.bounds.left() - padding) * scale;
        const float y0 = originY + (glyph.bounds.top() - padding) * scale;
        const float x1 = originX + (penX + glyph.bounds.right() + padding) * scale;
        const float y1 = originY + (glyph.bounds.bottom() + padding) * scale;

        const float u0 = static_cast<float>(glyph.uv.left());
        const float v0 = static_cast<float>(glyph.uv.top());
        const float u1 = static_cast<float>(glyph.uv.right());
        const float v1 = static_cast<float>(glyph.uv.bottom());

        MsdfGlyphNode::GlyphQuad quad;
        quad.pos[0] = QVector2D(x0, y0);
        quad.pos[1] = QVector2D(x0, y1);
        quad.pos[2] = QVector2D(x1, y0);
        quad.pos[3] = QVector2D(x1, y0);
        quad.pos[4] = QVector2D(x0, y1);
        quad.pos[5] = QVector2D(x1, y1);

        quad.uv[0] = QVector2D(u0, v0);
        quad.uv[1] = QVector2D(u0, v1);
        quad.uv[2] = QVector2D(u1, v0);
        quad.uv[3] = QVector2D(u1, v0);
        quad.uv[4] = QVector2D(u0, v1);
        quad.uv[5] = QVector2D(u1, v1);

        m_quads.push_back(quad);
        penX += glyph.advance;
    }
}
