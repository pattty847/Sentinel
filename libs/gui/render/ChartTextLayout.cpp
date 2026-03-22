#include "ChartTextLayout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct RunMetrics {
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float penAdvance = 0.0f;
    bool valid = false;
};

RunMetrics measureRun(const ChartTextAtlas& atlas, const ChartTextRun& run) {
    RunMetrics metrics;
    const float padding = static_cast<float>(atlas.paddingPx());
    float penX = 0.0f;
    for (const QChar c : run.text) {
        const auto& glyph = atlas.glyph(c);
        if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
            penX += glyph.advance;
            continue;
        }
        metrics.minX = std::min(metrics.minX, penX + static_cast<float>(glyph.bounds.left()) - padding);
        metrics.maxX = std::max(metrics.maxX, penX + static_cast<float>(glyph.bounds.right()) + padding);
        metrics.penAdvance = penX + glyph.advance;
        penX += glyph.advance;
    }

    if (run.useStableMetrics) {
        metrics.minY = atlas.glyphTopPx() - padding;
        metrics.maxY = atlas.glyphBottomPx() + padding;
    } else {
        for (const QChar c : run.text) {
            const auto& glyph = atlas.glyph(c);
            if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
                continue;
            }
            metrics.minY = std::min(metrics.minY, static_cast<float>(glyph.bounds.top()) - padding);
            metrics.maxY = std::max(metrics.maxY, static_cast<float>(glyph.bounds.bottom()) + padding);
        }
    }

    metrics.valid = std::isfinite(metrics.minX) && std::isfinite(metrics.maxX) &&
                    std::isfinite(metrics.minY) && std::isfinite(metrics.maxY);
    return metrics;
}
} // namespace

bool ChartTextLayout::measureRunRect(const ChartTextAtlas& atlas,
                                     const ChartTextRun& run,
                                     QRectF& outRect) {
    outRect = QRectF();
    if (!atlas.isBuilt() || run.text.isEmpty() || run.scale <= 0.0f) {
        return false;
    }

    const RunMetrics runMetrics = measureRun(atlas, run);
    if (!runMetrics.valid) {
        return false;
    }

    float originX = static_cast<float>(run.anchor.x());
    if (run.hAlign == ChartTextRun::HorizontalAlign::Center) {
        originX -= (runMetrics.minX + runMetrics.maxX) * 0.5f * run.scale;
    } else if (run.hAlign == ChartTextRun::HorizontalAlign::Right) {
        originX -= runMetrics.maxX * run.scale;
    } else {
        originX -= runMetrics.minX * run.scale;
    }

    float originY = static_cast<float>(run.anchor.y());
    if (run.vAlign == ChartTextRun::VerticalAlign::Center) {
        originY -= (runMetrics.minY + runMetrics.maxY) * 0.5f * run.scale;
    } else if (run.vAlign == ChartTextRun::VerticalAlign::Bottom) {
        originY -= runMetrics.maxY * run.scale;
    } else {
        originY -= runMetrics.minY * run.scale;
    }

    if (run.pixelSnap) {
        originX = std::round(originX);
        originY = std::round(originY);
    }

    outRect = QRectF(originX + runMetrics.minX * run.scale,
                     originY + runMetrics.minY * run.scale,
                     (runMetrics.maxX - runMetrics.minX) * run.scale,
                     (runMetrics.maxY - runMetrics.minY) * run.scale);
    return true;
}

void ChartTextLayout::appendRun(const ChartTextAtlas& atlas,
                                const ChartTextRun& run,
                                std::vector<ChartGlyphInstance>& out) {
    if (!atlas.isBuilt() || run.text.isEmpty() || run.scale <= 0.0f) {
        return;
    }

    QRectF runRect;
    if (!measureRunRect(atlas, run, runRect)) {
        return;
    }

    const RunMetrics runMetrics = measureRun(atlas, run);
    const float originX = static_cast<float>(runRect.x() - runMetrics.minX * run.scale);
    const float originY = static_cast<float>(runRect.y() - runMetrics.minY * run.scale);

    float penX = 0.0f;
    bool firstGlyph = true;
    for (const QChar c : run.text) {
        const auto& glyph = atlas.glyph(c);
        if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
            penX += glyph.advance;
            continue;
        }

        ChartGlyphInstance instance;
        instance.rect = QRectF(originX + (penX + glyph.bounds.left()) * run.scale,
                               originY + glyph.bounds.top() * run.scale,
                               glyph.bounds.width() * run.scale,
                               glyph.bounds.height() * run.scale);
        if (run.pixelSnap) {
            // Snap only X edges per-glyph. Y snapping per-glyph causes zigzag
            // because different characters have different bounds.top() which
            // round to different pixel rows. The origin Y is already snapped;
            // MSDF handles consistent sub-pixel Y positioning gracefully.
            const float snappedLeft = std::round(instance.rect.left());
            const float snappedRight = std::round(instance.rect.right());
            instance.rect = QRectF(snappedLeft, instance.rect.top(),
                                   snappedRight - snappedLeft,
                                   instance.rect.height());
        }
        instance.uv = glyph.uv;
        instance.color = run.color;
        instance.debugBaselineY = originY;
        if (firstGlyph) {
            instance.debugAnchor = run.anchor;
            instance.debugShowAnchor = true;
            firstGlyph = false;
        }
        out.push_back(instance);
        penX += glyph.advance;
    }
}

bool ChartTextLayout::buildDebugSample(const ChartTextAtlas& atlas,
                                       const ChartTextRun& run,
                                       ChartGlyphInstance& out,
                                       float& outRenderPx) {
    out = ChartGlyphInstance{};
    outRenderPx = static_cast<float>(atlas.fontPx()) * run.scale;
    std::vector<ChartGlyphInstance> glyphs;
    glyphs.reserve(run.text.size());
    appendRun(atlas, run, glyphs);
    if (glyphs.empty()) {
        return false;
    }
    out = glyphs.front();
    return true;
}
