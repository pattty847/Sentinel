#include "ChartTextRenderer.hpp"
#include "SentinelLogging.hpp"

#include <QtGlobal>

#include <algorithm>
#include <array>

namespace {
// Capacity grows at least 1.5x (from 256), never to the exact size: a rising
// glyph count reallocates O(log n) times, not every frame.
void reserveGrowing(std::vector<ChartGlyphInstance>& v, size_t required) {
    if (v.capacity() >= required) return;
    v.reserve(std::max({required, v.capacity() + v.capacity() / 2, size_t(256)}));
}
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
    m_inFrame = false;
    m_debugNode = nullptr;
    for (Bucket& bucket : m_buckets) {
        bucket.node = nullptr;
        bucket.glyphs.clear();
        bucket.used = false;
    }
}

QSGNode* ChartTextRenderer::createRoot() {
    m_root = new QSGNode();
    return m_root;
}

void ChartTextRenderer::beginFrame(QSGNode* parentNode, QQuickWindow* window, const ChartTextAtlas& atlas) {
    m_window = window;
    m_atlas = &atlas;
    m_inFrame = true;
    m_usedGlyphs = 0;
    m_droppedGlyphs = 0;
    m_droppedHighGlyphs = 0;
    m_droppedLowGlyphs = 0;
    m_debugGlyphs.clear();

    if (!m_root) {
        parentNode->appendChildNode(createRoot());
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

    // Glyphs per colour without a per-call container: chart text uses a handful of
    // colours (axis text, light and dark labels). Buckets grow geometrically and
    // keep their capacity across frames (no allocation in steady state).
    std::array<std::pair<QRgb, int>, 8> counts{};
    size_t distinct = 0;
    bool overflow = false;
    for (const ChartGlyphInstance& glyph : glyphs) {
        const QRgb rgba = glyph.color.rgba();
        size_t i = 0;
        while (i < distinct && counts[i].first != rgba) ++i;
        if (i == distinct) {
            if (distinct == counts.size()) { overflow = true; continue; }
            counts[distinct++] = {rgba, 0};
        }
        ++counts[i].second;
    }
    for (size_t i = 0; i < distinct; ++i) {
        Bucket* bucket = findOrCreateBucket(QColor::fromRgba(counts[i].first));
        reserveGrowing(bucket->glyphs, bucket->glyphs.size() + static_cast<size_t>(counts[i].second));
    }
    Q_UNUSED(overflow); // more colours than counted: push_back still grows geometrically

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
    m_inFrame = false; // every exit below ends the frame
    if (!m_root || !m_window || !m_atlas || !m_atlas->isBuilt()) {
        return;
    }
    const float sdfBias = envFloatOrDefault("SENTINEL_CHART_TEXT_SDF_BIAS", 0.0f);
    const float distanceSign = envFloatOrDefault("SENTINEL_CHART_TEXT_DISTANCE_SIGN", -1.0f);
    const float sprFloor = envFloatOrDefault("SENTINEL_CHART_TEXT_SPR_FLOOR", 2.0f);
    static bool shaderParamsLogged = false;
    if (!shaderParamsLogged) {
        shaderParamsLogged = true;
        sLog_Probe("text.shader", "sprFloor=" << sprFloor << " sdfBias=" << sdfBias
                   << " distanceSign=" << distanceSign << " pxRange=" << m_atlas->pxRange()
                   << " fontPx=" << m_atlas->fontPx());
    }

    const ShaderParams params{sdfBias, distanceSign, sprFloor};
    for (Bucket& bucket : m_buckets) {
        if (bucket.used) {
            prepareNode(bucket, params);
        }
        if (bucket.node) {
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

// The bucket's node (made on first use), with this frame's atlas and material.
void ChartTextRenderer::prepareNode(Bucket& bucket, const ShaderParams& params) {
    if (!bucket.node) {
        bucket.node = new ChartTextNode();
        m_root->appendChildNode(bucket.node);
    }
    bucket.node->setAtlas(m_atlas->image(), m_window);
    bucket.node->setPxRange(m_atlas->pxRange());
    bucket.node->setSdfBias(params.sdfBias);
    bucket.node->setDistanceSign(params.distanceSign);
    bucket.node->setSprFloor(params.sprFloor);
    bucket.node->setColor(bucket.color);
}

bool ChartTextRenderer::reserve(const QColor& color, int glyphs) {
    if (!m_inFrame || !m_root || !m_window || !m_atlas || !m_atlas->isBuilt() || glyphs <= 0) {
        return false;
    }
    Bucket* bucket = findOrCreateBucket(color);
    reserveGrowing(bucket->glyphs, static_cast<size_t>(glyphs));
    prepareNode(*bucket, {envFloatOrDefault("SENTINEL_CHART_TEXT_SDF_BIAS", 0.0f),
                          envFloatOrDefault("SENTINEL_CHART_TEXT_DISTANCE_SIGN", -1.0f),
                          envFloatOrDefault("SENTINEL_CHART_TEXT_SPR_FLOOR", 2.0f)});
    bucket->node->reserve(glyphs);
    return true;
}

int ChartTextRenderer::nodeCapacityGlyphs() const {
    int glyphs = 0;
    for (const Bucket& bucket : m_buckets) glyphs += bucket.node ? bucket.node->capacityGlyphs() : 0;
    return glyphs;
}

int ChartTextRenderer::textureCount() const {
    int textures = 0;
    for (const Bucket& bucket : m_buckets) textures += bucket.node && bucket.node->hasTexture() ? 1 : 0;
    return textures;
}

size_t ChartTextRenderer::capacityBytes() const {
    size_t bytes = m_buckets.capacity() * sizeof(Bucket);
    for (const Bucket& bucket : m_buckets) bytes += bucket.glyphs.capacity() * sizeof(ChartGlyphInstance);
    return bytes;
}

ChartTextRenderer::Bucket* ChartTextRenderer::findOrCreateBucket(const QColor& color) {
    for (Bucket& bucket : m_buckets) {
        if (bucket.color == color) {
            return &bucket;
        }
    }
    if (m_buckets.capacity() < 8) m_buckets.reserve(8);
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
