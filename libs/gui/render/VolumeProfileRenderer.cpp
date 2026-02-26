/*
 * Sentinel – VolumeProfileRenderer (implementation)
 */
#include "VolumeProfileRenderer.hpp"

#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Build a solid-colour axis-aligned rectangle into existing geometry storage
// (expects the geometry to be pre-allocated for 4 vertices / 6 indices).
void fillRect(QSGGeometry* geo, const QRectF& r, const QColor& c) {
    const float x0 = static_cast<float>(r.left());
    const float y0 = static_cast<float>(r.top());
    const float x1 = static_cast<float>(r.right());
    const float y1 = static_cast<float>(r.bottom());
    const quint8 cr = static_cast<quint8>(c.red());
    const quint8 cg = static_cast<quint8>(c.green());
    const quint8 cb = static_cast<quint8>(c.blue());
    const quint8 ca = static_cast<quint8>(c.alpha());

    auto* v = geo->vertexDataAsColoredPoint2D();
    v[0].set(x0, y0, cr, cg, cb, ca);
    v[1].set(x1, y0, cr, cg, cb, ca);
    v[2].set(x0, y1, cr, cg, cb, ca);
    v[3].set(x1, y1, cr, cg, cb, ca);

    auto* idx = geo->indexDataAsUShort();
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 1; idx[4] = 3; idx[5] = 2;
}

// Price → screen Y within drawRect for a given visible price range.
// viewMaxPrice maps to drawRect.top(); viewMinPrice to drawRect.bottom().
inline float priceToY(double price, double viewMinPrice, double viewMaxPrice,
                      const QRectF& drawRect) {
    if (viewMaxPrice <= viewMinPrice) {
        return static_cast<float>(drawRect.center().y());
    }
    const double frac = (viewMaxPrice - price) / (viewMaxPrice - viewMinPrice);
    return static_cast<float>(drawRect.top() + frac * drawRect.height());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────

void VolumeProfileRenderer::setWidthFraction(float frac) {
    m_widthFraction = std::clamp(frac, 0.02f, 0.50f);
}

void VolumeProfileRenderer::setOverlay(bool overlay) {
    m_overlay = overlay;
}

void VolumeProfileRenderer::setBarColor(const QColor& color) {
    m_barColor = color;
}

void VolumeProfileRenderer::setVaColor(const QColor& color) {
    m_vaColor = color;
}

void VolumeProfileRenderer::setPocColor(const QColor& color) {
    m_pocColor = color;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void VolumeProfileRenderer::onRootRebuilt() {
    m_vaNode   = nullptr;
    m_barsNode = nullptr;
    m_pocNode  = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hot path
// ─────────────────────────────────────────────────────────────────────────────

void VolumeProfileRenderer::render(QSGNode* parentNode,
                                   bool drawVp,
                                   const QRectF& drawRect,
                                   double viewMinPrice,
                                   double viewMaxPrice,
                                   const std::vector<float>& bins,
                                   const VolumeProfileState::Snapshot& snap) {
    if (!parentNode) {
        return;
    }

    if (!drawVp || bins.empty() || snap.gridHeight <= 0 || snap.tickSize <= 0.0 ||
        drawRect.isEmpty() || viewMaxPrice <= viewMinPrice) {
        clearGeometry();
        return;
    }

    ensureNodes(parentNode);
    rebuildGeometry(drawRect, viewMinPrice, viewMaxPrice, bins, snap);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void VolumeProfileRenderer::ensureNodes(QSGNode* parentNode) {
    // VA band
    if (!m_vaNode) {
        m_vaNode = new QSGGeometryNode();
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 4, 6);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        geo->setIndexDataPattern(QSGGeometry::StaticPattern);
        m_vaNode->setGeometry(geo);
        m_vaNode->setFlag(QSGNode::OwnsGeometry);
        auto* mat = new QSGVertexColorMaterial();
        m_vaNode->setMaterial(mat);
        m_vaNode->setFlag(QSGNode::OwnsMaterial);
        parentNode->appendChildNode(m_vaNode);
    }
    // Bars node
    if (!m_barsNode) {
        // Pre-allocate for max 4096 bins (each bin = 2 triangles = 4 verts + 6 idx)
        constexpr int kMaxBins = 4096;
        m_barsNode = new QSGGeometryNode();
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                    kMaxBins * 4, kMaxBins * 6);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        geo->setVertexDataPattern(QSGGeometry::DynamicPattern);
        geo->setIndexDataPattern(QSGGeometry::DynamicPattern);
        m_barsNode->setGeometry(geo);
        m_barsNode->setFlag(QSGNode::OwnsGeometry);
        auto* mat = new QSGVertexColorMaterial();
        m_barsNode->setMaterial(mat);
        m_barsNode->setFlag(QSGNode::OwnsMaterial);
        parentNode->appendChildNode(m_barsNode);
    }
    // POC line node
    if (!m_pocNode) {
        m_pocNode = new QSGGeometryNode();
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 4, 6);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_pocNode->setGeometry(geo);
        m_pocNode->setFlag(QSGNode::OwnsGeometry);
        auto* mat = new QSGVertexColorMaterial();
        m_pocNode->setMaterial(mat);
        m_pocNode->setFlag(QSGNode::OwnsMaterial);
        parentNode->appendChildNode(m_pocNode);
    }
}

void VolumeProfileRenderer::clearGeometry() {
    // Zero out vertex counts so nothing is drawn; nodes stay in tree for reuse.
    if (m_vaNode   && m_vaNode->geometry())   m_vaNode->geometry()->allocate(0, 0);
    if (m_barsNode && m_barsNode->geometry()) m_barsNode->geometry()->allocate(0, 0);
    if (m_pocNode  && m_pocNode->geometry())  m_pocNode->geometry()->allocate(0, 0);
    if (m_vaNode)   m_vaNode->markDirty(QSGNode::DirtyGeometry);
    if (m_barsNode) m_barsNode->markDirty(QSGNode::DirtyGeometry);
    if (m_pocNode)  m_pocNode->markDirty(QSGNode::DirtyGeometry);
}

void VolumeProfileRenderer::rebuildGeometry(const QRectF& drawRect,
                                             double viewMinPrice,
                                             double viewMaxPrice,
                                             const std::vector<float>& bins,
                                             const VolumeProfileState::Snapshot& snap) {
    const int n = static_cast<int>(bins.size());
    if (n == 0) return;

    // ── Histogram area ─────────────────────────────────────────────────────
    // The VP is pinned to the right edge.
    const float vpWidth  = static_cast<float>(drawRect.width()) * m_widthFraction;
    const float vpLeft   = static_cast<float>(drawRect.right()) - vpWidth;
    const float vpRight  = static_cast<float>(drawRect.right());

    // Find max bin for normalisation (avoid zero-division).
    float maxVol = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (bins[static_cast<size_t>(i)] > maxVol) maxVol = bins[static_cast<size_t>(i)];
    }
    if (maxVol <= 0.0f) {
        clearGeometry();
        return;
    }

    // ── 1. VA band ─────────────────────────────────────────────────────────
    if (snap.va.valid && m_vaNode) {
        const float yVah = priceToY(snap.va.vahPrice, viewMinPrice, viewMaxPrice, drawRect);
        const float yVal = priceToY(snap.va.valPrice, viewMinPrice, viewMaxPrice, drawRect);
        const QRectF vaRect(static_cast<double>(vpLeft),
                            static_cast<double>(std::min(yVah, yVal)),
                            static_cast<double>(vpWidth),
                            static_cast<double>(std::abs(yVal - yVah)));
        auto* geo = m_vaNode->geometry();
        if (geo->vertexCount() < 4 || geo->indexCount() < 6) {
            geo->allocate(4, 6);
        }
        fillRect(geo, vaRect, m_vaColor);
        m_vaNode->markDirty(QSGNode::DirtyGeometry);
    }

    // ── 2. Volume bars ─────────────────────────────────────────────────────
    if (m_barsNode) {
        // Only render bins that fall within the visible price range.
        // bin[0] = top of grid (highest price), bin[n-1] = bottom (lowest price).
        // Centre price of bin i: snap.maxPrice - (i + 0.5) * snap.tickSize
        const double gridMaxPrice = snap.minPrice + static_cast<double>(n) * snap.tickSize;

        // Count visible bins first to size the geometry.
        int visCount = 0;
        for (int i = 0; i < n; ++i) {
            const double binTop    = gridMaxPrice - static_cast<double>(i)       * snap.tickSize;
            const double binBottom = gridMaxPrice - static_cast<double>(i + 1)   * snap.tickSize;
            if (binTop < viewMinPrice || binBottom > viewMaxPrice) continue;
            ++visCount;
        }

        auto* geo = m_barsNode->geometry();
        if (geo->vertexCount() < visCount * 4 || geo->indexCount() < visCount * 6) {
            geo->allocate(visCount * 4, visCount * 6);
        }
        auto* vdata = geo->vertexDataAsColoredPoint2D();
        auto* idata = geo->indexDataAsUShort();
        int vi = 0;
        int ii = 0;

        const quint8 cr = static_cast<quint8>(m_barColor.red());
        const quint8 cg = static_cast<quint8>(m_barColor.green());
        const quint8 cb = static_cast<quint8>(m_barColor.blue());

        for (int i = 0; i < n && vi / 4 < visCount; ++i) {
            const float vol = bins[static_cast<size_t>(i)];
            const double binTop    = gridMaxPrice - static_cast<double>(i)     * snap.tickSize;
            const double binBottom = gridMaxPrice - static_cast<double>(i + 1) * snap.tickSize;
            if (binTop < viewMinPrice || binBottom > viewMaxPrice) continue;

            const float barFrac  = vol / maxVol;
            const float barWidth = vpWidth * barFrac;
            // Bars extend leftward from vpRight.
            const float x0 = vpRight - barWidth;
            const float x1 = vpRight;
            const float y0 = priceToY(binTop,    viewMinPrice, viewMaxPrice, drawRect);
            const float y1 = priceToY(binBottom, viewMinPrice, viewMaxPrice, drawRect);
            // Intensity: brighter for higher volume (keep hue, scale alpha).
            const quint8 ca = static_cast<quint8>(
                std::clamp(static_cast<int>(80 + 175 * barFrac), 80, 255));

            const quint16 vi0 = static_cast<quint16>(vi);
            vdata[vi  ].set(x0, y0, cr, cg, cb, ca);
            vdata[vi+1].set(x1, y0, cr, cg, cb, ca);
            vdata[vi+2].set(x0, y1, cr, cg, cb, ca);
            vdata[vi+3].set(x1, y1, cr, cg, cb, ca);
            idata[ii  ] = vi0;     idata[ii+1] = vi0+1; idata[ii+2] = vi0+2;
            idata[ii+3] = vi0+1;   idata[ii+4] = vi0+3; idata[ii+5] = vi0+2;
            vi += 4;
            ii += 6;
        }
        // Zero out any leftover allocated geometry from prior frames.
        for (int i = vi; i < geo->vertexCount(); ++i) {
            vdata[i].set(0, 0, 0, 0, 0, 0);
        }
        m_barsNode->markDirty(QSGNode::DirtyGeometry);
    }

    // ── 3. POC line ────────────────────────────────────────────────────────
    if (snap.va.valid && m_pocNode) {
        const float yPoc = priceToY(snap.va.pocPrice, viewMinPrice, viewMaxPrice, drawRect);
        constexpr float kPocLineHalfH = 1.5f;
        const QRectF pocRect(static_cast<double>(vpLeft),
                             static_cast<double>(yPoc - kPocLineHalfH),
                             static_cast<double>(vpWidth),
                             static_cast<double>(kPocLineHalfH * 2.0f));
        auto* geo = m_pocNode->geometry();
        if (geo->vertexCount() < 4 || geo->indexCount() < 6) {
            geo->allocate(4, 6);
        }
        fillRect(geo, pocRect, m_pocColor);
        m_pocNode->markDirty(QSGNode::DirtyGeometry);
    }
}
