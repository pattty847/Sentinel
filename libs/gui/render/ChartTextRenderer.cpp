#include "ChartTextRenderer.hpp"

#include <QtGlobal>

namespace {
float envFloatOrDefault(const char* name, float fallback) {
    const QByteArray value = qgetenv(name);
    if (value.isEmpty()) {
        return fallback;
    }
    bool ok = false;
    const float parsed = value.toFloat(&ok);
    return ok ? parsed : fallback;
}
} // namespace

void ChartTextRenderer::onRootRebuilt() {
    m_root = nullptr;
    m_debugNode = nullptr;
    for (Bucket& bucket : m_buckets) {
        bucket.node = nullptr;
        bucket.glyphs.clear();
        bucket.used = false;
    }
}

void ChartTextRenderer::beginFrame(QSGNode* parentNode, QQuickWindow* window, const ChartTextAtlas& atlas) {
    m_window = window;
    m_atlas = &atlas;
    m_usedGlyphs = 0;
    m_droppedGlyphs = 0;
    m_droppedHighGlyphs = 0;
    m_droppedLowGlyphs = 0;
    m_debugGlyphs.clear();

    if (!m_root) {
        m_root = new QSGNode();
        parentNode->appendChildNode(m_root);
    }

    for (Bucket& bucket : m_buckets) {
        bucket.glyphs.clear();
        bucket.used = false;
    }
}

void ChartTextRenderer::submitRun(const ChartTextRun& run, Priority priority) {
    if (!m_atlas || !m_atlas->isBuilt()) {
        return;
    }
    std::vector<ChartGlyphInstance> glyphs;
    glyphs.reserve(run.text.size());
    ChartTextLayout::appendRun(*m_atlas, run, glyphs);
    submitGlyphs(glyphs, priority);
}

void ChartTextRenderer::submitGlyphs(const std::vector<ChartGlyphInstance>& glyphs, Priority priority) {
    if (glyphs.empty()) {
        return;
    }
    if (!canAccept(static_cast<int>(glyphs.size()), priority)) {
        if (priority == Priority::High) {
            m_droppedHighGlyphs += static_cast<int>(glyphs.size());
        } else {
            m_droppedLowGlyphs += static_cast<int>(glyphs.size());
        }
        m_droppedGlyphs += static_cast<int>(glyphs.size());
        if (priority == Priority::Low) {
            return;
        }
    }

    QHash<QRgb, int> pendingCounts;
    for (const ChartGlyphInstance& glyph : glyphs) {
        pendingCounts[glyph.color.rgba()] += 1;
    }
    for (auto it = pendingCounts.constBegin(); it != pendingCounts.constEnd(); ++it) {
        Bucket* bucket = findOrCreateBucket(QColor::fromRgba(it.key()));
        const size_t required = bucket->glyphs.size() + static_cast<size_t>(it.value());
        if (bucket->glyphs.capacity() < required) {
            bucket->glyphs.reserve(required);
        }
    }

    for (const ChartGlyphInstance& glyph : glyphs) {
        Bucket* bucket = findOrCreateBucket(glyph.color);
        bucket->glyphs.push_back(glyph);
        bucket->used = true;
        ++m_usedGlyphs;
    }
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG_OVERLAY")) {
        m_debugGlyphs.insert(m_debugGlyphs.end(), glyphs.begin(), glyphs.end());
    }
}

void ChartTextRenderer::endFrame() {
    if (!m_root || !m_window || !m_atlas || !m_atlas->isBuilt()) {
        return;
    }
    const float sdfBias = envFloatOrDefault("SENTINEL_CHART_TEXT_SDF_BIAS", 0.0f);
    const float distanceSign = envFloatOrDefault("SENTINEL_CHART_TEXT_DISTANCE_SIGN", -1.0f);
    const float sprFloor = envFloatOrDefault("SENTINEL_CHART_TEXT_SPR_FLOOR", 2.0f);
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG")) {
        static int sprLogCount = 0;
        if (++sprLogCount <= 3) {
            qDebug("ChartTextRenderer::endFrame: sprFloor=%.2f sdfBias=%.2f distanceSign=%.1f pxRange=%.1f",
                   sprFloor, sdfBias, distanceSign, m_atlas->pxRange());
        }
    }

    for (Bucket& bucket : m_buckets) {
        if (bucket.used && !bucket.node) {
            bucket.node = new ChartTextNode();
            m_root->appendChildNode(bucket.node);
        }
        if (!bucket.node) {
            continue;
        }
        if (bucket.used) {
            bucket.node->setAtlas(m_atlas->image(), m_window);
            bucket.node->setPxRange(m_atlas->pxRange());
            bucket.node->setSdfBias(sdfBias);
            bucket.node->setDistanceSign(distanceSign);
            bucket.node->setSprFloor(sprFloor);
            bucket.node->setColor(bucket.color);
            bucket.node->updateGeometry(bucket.glyphs);
        } else {
            bucket.node->updateGeometry(bucket.glyphs);
        }
    }

    if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG_OVERLAY")) {
        if (!m_debugNode) {
            m_debugNode = new ChartTextDebugNode();
            m_root->appendChildNode(m_debugNode);
        }
        m_debugNode->updateGeometry(m_debugGlyphs);
    } else if (m_debugNode) {
        m_debugNode->updateGeometry({});
    }
}

ChartTextRenderer::Bucket* ChartTextRenderer::findOrCreateBucket(const QColor& color) {
    for (Bucket& bucket : m_buckets) {
        if (bucket.color == color) {
            return &bucket;
        }
    }
    m_buckets.push_back(Bucket{color, {}, nullptr, false});
    return &m_buckets.back();
}

bool ChartTextRenderer::canAccept(int glyphCount, Priority priority) {
    if (glyphCount <= 0) {
        return true;
    }
    if (priority == Priority::High) {
        return true;
    }
    return (m_usedGlyphs + glyphCount) <= m_maxGlyphs;
}
