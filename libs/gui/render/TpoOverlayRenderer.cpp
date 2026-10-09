/*
 * Sentinel – TpoOverlayRenderer
 *
 * Market profile overlay: coloured letter cells + MSDF letters per session.
 * See the header for the layout modes and the rebuild contract.
 */
#include "TpoOverlayRenderer.hpp"

#include "ChartTextAtlas.hpp"
#include "ChartTextNode.hpp"
#include "SentinelLogging.hpp"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// Owns the shared MSDF atlas texture for every TPO text node; deleted with the tree.
class TpoRootNode final : public QSGNode {
public:
    ~TpoRootNode() override { delete texture; }
    QSGTexture* texture = nullptr;
};

namespace {
// Collapsed cells are at most this wide relative to their height, and never
// wider than kMaxCollapsedCellW: a zoomed-in price axis must not balloon the profile.
constexpr double kCollapsedAspect = 0.85;
constexpr double kMaxCollapsedCellW = 16.0;
// Display rows are never shorter than this.
constexpr double kMinRowPx = 3.0;
// Letters stay a small monospace, however tall the rows get.
constexpr double kMaxFontPx = 13.0;
// Collapsed profiles use at most this share of their session's width.
constexpr double kCollapsedSessionFill = 0.96;
// Letter font size relative to the cell; the glyph advance must fit the width.
constexpr double kFontPerCellH = 0.80;
constexpr double kAdvanceFill = 0.78;  // the glyph advance may use this share of the cell width
// Letters fade in between these font sizes (px).
constexpr double kTextHiddenPx = 7.0;
constexpr double kTextFullPx = 10.0;

const QColor kLightText(240, 243, 247);
const QColor kDarkText(18, 22, 28);

int64_t quantize(double value) {
    return std::llround(value * 256.0);
}

void setVertex(QSGGeometry::ColoredPoint2D& v, float x, float y, const tpo::Rgba& c) {
    // QSGVertexColorMaterial expects premultiplied colour.
    const int a = c.a;
    v.set(x, y,
          static_cast<uchar>(c.r * a / 255),
          static_cast<uchar>(c.g * a / 255),
          static_cast<uchar>(c.b * a / 255),
          static_cast<uchar>(a));
}

void writeQuad(QSGGeometry::ColoredPoint2D* v, float x0, float y0, float x1, float y1,
               const tpo::Rgba& c) {
    setVertex(v[0], x0, y0, c);
    setVertex(v[1], x1, y0, c);
    setVertex(v[2], x0, y1, c);
    setVertex(v[3], x0, y1, c);
    setVertex(v[4], x1, y0, c);
    setVertex(v[5], x1, y1, c);
}
} // namespace

TpoOverlayRenderer::TpoOverlayRenderer() = default;
TpoOverlayRenderer::~TpoOverlayRenderer() = default;

// ─────────────────────────────────────────────────────────────────────────────
//  GUI thread
// ─────────────────────────────────────────────────────────────────────────────

void TpoOverlayRenderer::enqueue(PendingUpload upload) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUploads.push_back(std::move(upload));
}

void TpoOverlayRenderer::setStyle(const Style& style) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingStyle = style;
    m_pendingStyle.rowPx = std::clamp(style.rowPx, 4.0, 64.0);
    m_pendingStyle.maxSessions = std::clamp(style.maxSessions, 1, 8);
}

TpoOverlayRenderer::Style TpoOverlayRenderer::style() const {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pendingStyle;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render thread
// ─────────────────────────────────────────────────────────────────────────────

void TpoOverlayRenderer::onRootRebuilt() {
    // The old scene graph (and every node below m_root) was deleted with the root.
    m_root = nullptr;
    m_opacity = nullptr;
    m_clip = nullptr;
    m_clipRect = QRectF();
    for (auto& [start, session] : m_sessions) {
        session.transform = nullptr;
        session.cells = nullptr;
        session.lightText = nullptr;
        session.darkText = nullptr;
        session.cellCapacity = 0;
        session.hasBuilt = false;
    }
}

void TpoOverlayRenderer::dropSessionNodes(Session& session) {
    if (session.transform) {
        if (QSGNode* parent = session.transform->parent()) {
            parent->removeChildNode(session.transform);
        }
        delete session.transform;  // deletes cells and text nodes (owned by parent)
    }
    session.transform = nullptr;
    session.cells = nullptr;
    session.lightText = nullptr;
    session.darkText = nullptr;
    session.cellCapacity = 0;
    session.hasBuilt = false;
}

void TpoOverlayRenderer::clearSessions() {
    for (auto& [start, session] : m_sessions) {
        dropSessionNodes(session);
    }
    m_sessions.clear();
}

void TpoOverlayRenderer::applyUploads(std::vector<PendingUpload>& uploads, int maxSessions) {
    for (auto& upload : uploads) {
        const auto& g = upload.grid;
        if (upload.gridWidth <= 0 || upload.gridHeight <= 0 || upload.x < 0 ||
            upload.x >= upload.gridWidth || upload.letters.size() != upload.gridHeight ||
            g.endMs <= g.startMs || !std::isfinite(g.maxPrice) || !std::isfinite(g.tick) ||
            g.tick <= 0.0 || g.maxPrice <= 0.0) {
            sLog_RenderN(1000, "TPO upload dropped: x=" << upload.x << " periods=" << upload.gridWidth
                         << " rows=" << upload.gridHeight << " letters=" << upload.letters.size());
            continue;
        }
        if (g.generation != m_grid.generation || g.maxPrice != m_grid.maxPrice ||
            g.tick != m_grid.tick || upload.gridHeight != m_gridRows) {
            if (!m_sessions.empty()) {
                sLog_Render("TPO grid changed: generation " << m_grid.generation << "->" << g.generation
                            << " maxPrice=" << g.maxPrice << " tick=" << g.tick
                            << " rows=" << upload.gridHeight);
            }
            clearSessions();
            m_grid = g;
            m_gridRows = upload.gridHeight;
        }
        auto it = m_sessions.find(g.startMs);
        if (it == m_sessions.end() && !m_sessions.empty()) {
            // A different session type or bracket: sessions overlap or periods differ.
            bool foreign = false;
            for (const auto& [start, other] : m_sessions) {
                if ((upload.periodMs > 0 && other.periodMs != upload.periodMs) ||
                    (g.startMs < other.endMs && other.startMs < g.endMs)) {
                    foreign = true;
                    break;
                }
            }
            if (foreign) {
                sLog_Render("TPO session layout changed: clearing " << m_sessions.size()
                            << " sessions for start=" << g.startMs << " end=" << g.endMs
                            << " periodMs=" << upload.periodMs);
                clearSessions();
            }
        }
        if (it == m_sessions.end()) {
            if (static_cast<int>(m_sessions.size()) >= maxSessions &&
                g.startMs < m_sessions.begin()->first) {
                continue;  // older than every retained session
            }
            Session session;
            session.startMs = g.startMs;
            session.endMs = g.endMs;
            it = m_sessions.emplace(g.startMs, std::move(session)).first;
        }
        Session& session = it->second;
        if (session.periods != upload.gridWidth) {
            session.periods = upload.gridWidth;
            session.rowsByPeriod.resize(static_cast<size_t>(upload.gridWidth));
        }
        session.periodMs = upload.periodMs > 0
            ? upload.periodMs
            : (session.endMs - session.startMs) / std::max(1, session.periods);
        auto& rows = session.rowsByPeriod[static_cast<size_t>(upload.x)];
        rows.clear();
        const char* letters = upload.letters.constData();
        for (int row = 0; row < upload.gridHeight; ++row) {
            if (letters[row] != '\0') {
                rows.push_back(row);
            }
        }
        ++session.dataVersion;
    }
    while (static_cast<int>(m_sessions.size()) > maxSessions) {
        dropSessionNodes(m_sessions.begin()->second);
        m_sessions.erase(m_sessions.begin());
    }
}

void TpoOverlayRenderer::ensureRoot(QSGNode* parentNode) {
    if (m_root) {
        return;
    }
    m_root = new TpoRootNode();
    m_opacity = new QSGOpacityNode();
    m_clip = new QSGClipNode();
    m_clip->setIsRectangular(true);
    auto* clipGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
    clipGeometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    m_clip->setGeometry(clipGeometry);
    m_clip->setFlag(QSGNode::OwnsGeometry, true);
    m_root->appendChildNode(m_opacity);
    m_opacity->appendChildNode(m_clip);
    parentNode->appendChildNode(m_root);
    m_clipRect = QRectF();
}

void TpoOverlayRenderer::ensureSessionNodes(Session& session) {
    if (session.transform) {
        return;
    }
    session.transform = new QSGTransformNode();
    session.cells = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    session.cells->setGeometry(geometry);
    session.cells->setFlag(QSGNode::OwnsGeometry, true);
    session.cells->setMaterial(new QSGVertexColorMaterial());
    session.cells->setFlag(QSGNode::OwnsMaterial, true);
    session.lightText = new ChartTextNode();
    session.darkText = new ChartTextNode();
    session.transform->appendChildNode(session.cells);
    session.transform->appendChildNode(session.lightText);
    session.transform->appendChildNode(session.darkText);
    m_clip->appendChildNode(session.transform);
    session.cellCapacity = 0;
    session.hasBuilt = false;
    session.lastTx = std::numeric_limits<double>::quiet_NaN();
    session.lastTy = std::numeric_limits<double>::quiet_NaN();
}

void TpoOverlayRenderer::appendGlyph(std::vector<ChartGlyphInstance>& out,
                                     const ChartTextAtlas& atlas,
                                     char letter, float cx, float cy, float scale) const {
    const auto& glyph = atlas.glyph(QChar(letter));
    if (glyph.advance <= 0.0f || glyph.uv.isNull()) {
        return;
    }
    // Centre the monospace advance horizontally and the cap height vertically,
    // so upper- and lower-case letters share one baseline per row.
    const auto& cap = atlas.glyph(QChar('A'));
    const float capMid = static_cast<float>(cap.bounds.top() + cap.bounds.bottom()) * 0.5f;
    const float penX = std::round(cx - glyph.advance * scale * 0.5f);
    const float baseline = std::round(cy - capMid * scale);
    ChartGlyphInstance instance;
    instance.rect = QRectF(penX + glyph.bounds.left() * scale,
                           baseline + glyph.bounds.top() * scale,
                           glyph.bounds.width() * scale,
                           glyph.bounds.height() * scale);
    instance.uv = glyph.uv;
    out.push_back(instance);
}

void TpoOverlayRenderer::rebuildSession(Session& session, const LayoutKey& key, const Style& style,
                                        const ChartTextAtlas& atlas, double basePx, double periodPx,
                                        int64_t topAbs, float textFontPx) {
    const tpo::ProfileRows& profile = session.profile;
    const tpo::ValueArea& va = profile.valueArea();
    const bool split = key.layout == tpo::Layout::Split;
    const double cellW = key.cellW;
    const double cellH = key.cellH;
    const float gapX = cellW >= 6.0 ? 1.0f : 0.0f;
    const float gapY = cellH >= 6.0 ? 1.0f : 0.0f;
    const auto edge = [&key](double logical) {
        return key.free ? logical : std::round(logical * key.dpr) / key.dpr;
    };
    const float minCell = static_cast<float>(key.free ? 1.0 : std::ceil(key.dpr) / key.dpr);
    const int group = profile.group();

    int lastPeriod = -1;
    for (int p = session.periods - 1; p >= 0; --p) {
        if (!session.rowsByPeriod[static_cast<size_t>(p)].empty()) { lastPeriod = p; break; }
    }

    const bool pocBar = split && va.valid() && lastPeriod >= 0;
    const int quads = profile.totalTpos() + (pocBar ? 1 : 0);
    QSGGeometry* geometry = session.cells->geometry();
    const int needed = quads * 6;
    if (needed > session.cellCapacity) {
        session.cellCapacity = std::max(needed + needed / 4, 600);
        geometry->allocate(session.cellCapacity);
    }
    auto* v = geometry->vertexDataAsColoredPoint2D();
    int written = 0;

    const float fontScale = textFontPx / static_cast<float>(std::max(1, atlas.fontPx()));
    session.lightGlyphs.clear();
    session.darkGlyphs.clear();

    auto rowTop = [&](int row) {
        const int64_t g = profile.minGroup() + row;
        return static_cast<double>(topAbs - (g + 1) * group) * basePx;
    };

    if (pocBar) {
        const double top = rowTop(va.poc);
        writeQuad(v + written, 0.0f, static_cast<float>(edge(top)),
                  static_cast<float>(edge((lastPeriod + 1) * periodPx)),
                  static_cast<float>(edge(top + cellH)), tpo::pocBarColor(style.theme));
        written += 6;
    }

    double widest = 0.0;
    for (int row = 0; row < profile.rows(); ++row) {
        if (profile.count(row) == 0) {
            continue;
        }
        const double top = rowTop(row);
        const float y0 = static_cast<float>(edge(top));
        const float y1 = static_cast<float>(edge(top + cellH - gapY));
        const float cy = static_cast<float>(top + cellH * 0.5);
        const bool inVa = va.contains(row);
        const bool poc = row == va.poc;
        int k = 0;
        profile.forEachPeriod(row, [&](int period) {
            const double left = split ? period * periodPx : k * cellW;
            const float x0 = static_cast<float>(edge(left));
            const float x1 = static_cast<float>(edge(left + cellW - gapX));
            const tpo::Rgba color = tpo::cellColor(style.theme, period, session.periods, inVa, poc);
            if (written + 6 <= session.cellCapacity) {
                writeQuad(v + written, x0, y0, std::max(x0 + minCell, x1), std::max(y0 + minCell, y1), color);
                written += 6;
            }
            if (key.text) {
                auto& out = tpo::prefersDarkText(color) ? session.darkGlyphs : session.lightGlyphs;
                appendGlyph(out, atlas, tpo::letterForPeriod(period),
                            static_cast<float>(left + cellW * 0.5), cy, fontScale);
            }
            ++k;
        });
        widest = std::max(widest, k * cellW);
    }
    if (written < session.cellCapacity) {
        std::memset(static_cast<void*>(v + written), 0,
                    sizeof(QSGGeometry::ColoredPoint2D) * static_cast<size_t>(session.cellCapacity - written));
    }
    session.cells->markDirty(QSGNode::DirtyGeometry);
    session.profileWidthPx = split ? (lastPeriod + 1) * periodPx : widest;

    session.lightText->updateGeometry(session.lightGlyphs);
    session.darkText->updateGeometry(session.darkGlyphs);

    const double fade = std::clamp((textFontPx - kTextHiddenPx) / (kTextFullPx - kTextHiddenPx), 0.0, 1.0);
    session.textAlpha = key.text ? static_cast<float>(fade) : 0.0f;
    QColor light = kLightText;
    light.setAlphaF(session.textAlpha);
    QColor dark = kDarkText;
    dark.setAlphaF(session.textAlpha);
    session.lightText->setColor(light);
    session.darkText->setColor(dark);

    sLog_Probe("tpo.build", "session=" << session.startMs << " periods=" << session.periods
               << " layout=" << tpo::layoutName(key.layout) << " theme=" << tpo::themeName(key.theme)
               << " group=" << group << " rows=" << profile.rows() << " tpos=" << profile.totalTpos()
               << " maxCount=" << profile.maxCount() << " poc=" << va.poc
               << " va=[" << va.low << ".." << va.high << "]"
               << " cell=" << cellW << "x" << cellH << " font=" << textFontPx
               << " glyphs=" << (session.lightGlyphs.size() + session.darkGlyphs.size()));
}

int TpoOverlayRenderer::chooseRowGroup(double basePx, double pxPerMs, int64_t topAbs,
                                       const Style& style) {
    // Rows grow (1-2-2.5-5 ticks) until they reach style.rowPx, unless the cells
    // are already width-bound: collapsed cells by session width / widest row,
    // split cells by the period width. Coarser rows would then only make taller,
    // narrower cells, so the profile keeps its finer rows instead.
    int group = tpo::rowGroupFor(basePx, kMinRowPx);
    for (int guard = 0; guard < 64; ++guard) {
        const double cellH = basePx * group;
        if (cellH >= style.rowPx) {
            return group;
        }
        double widthCap = kMaxCollapsedCellW;
        for (auto& [start, session] : m_sessions) {
            if (session.periods <= 0) continue;
            if (style.layout == tpo::Layout::Split) {
                widthCap = std::min(widthCap, static_cast<double>(session.periodMs) * pxPerMs);
                continue;
            }
            if (session.profileVersion != session.dataVersion || session.profileGroup != group) {
                session.profile.build(session.rowsByPeriod, topAbs, group);
                session.profileVersion = session.dataVersion;
                session.profileGroup = group;
            }
            const double sessionPx = static_cast<double>(session.endMs - session.startMs) * pxPerMs;
            widthCap = std::min(widthCap, sessionPx * kCollapsedSessionFill /
                                              std::max(1, session.profile.maxCount()));
        }
        if (widthCap < cellH * kCollapsedAspect) {
            return group;
        }
        group = tpo::nextRowGroup(group);
    }
    return group;
}

void TpoOverlayRenderer::render(QQuickWindow* window,
                                QSGNode* parentNode,
                                bool drawTpo,
                                const ChartTextAtlas& atlas,
                                bool atlasReady,
                                const chart_raster::RasterCamera& camera, double dpr,
                                const QRectF& surfaceBounds) {
    if (!window || !parentNode) {
        return;
    }
    Style style;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_drain.swap(m_pendingUploads);
        style = m_pendingStyle;
    }
    m_style = style;
    if (!m_drain.empty()) {
        applyUploads(m_drain, style.maxSessions);
        m_drain.clear();
    } else if (static_cast<int>(m_sessions.size()) > style.maxSessions) {
        applyUploads(m_drain, style.maxSessions);
    }

    // Created on the first frame so the TPO subtree keeps its place below chart text.
    ensureRoot(parentNode);

    const auto mapping = chart_raster::toMapping(camera);
    const double viewStartMs = mapping.viewStartMs, viewEndMs = mapping.viewEndMs;
    const double scale = std::isfinite(dpr) && dpr > 0 ? dpr : 1.0;
    const bool free = camera.free;
    const auto edge = [scale, free](double logical) {
        return free ? logical : std::round(logical * scale) / scale;
    };
    const bool viewValid = mapping.valid && viewEndMs > viewStartMs &&
        !surfaceBounds.isEmpty() && m_grid.tick > 0.0;
    const bool visible = drawTpo && viewValid && !m_sessions.empty();
    const double opacity = visible ? 1.0 : 0.0;
    if (m_opacity->opacity() != opacity) {
        m_opacity->setOpacity(opacity);  // 0 blocks the whole subtree
    }
    if (!visible) {
        return;
    }

    if (surfaceBounds != m_clipRect) {
        m_clipRect = surfaceBounds;
        m_clip->setClipRect(surfaceBounds);
        QSGGeometry::updateRectGeometry(m_clip->geometry(), surfaceBounds);
        m_clip->markDirty(QSGNode::DirtyGeometry);
    }

    if (atlasReady && !m_root->texture && atlas.isBuilt()) {
        QSGTexture* texture = window->createTextureFromImage(atlas.image());
        if (texture && texture->isAtlasTexture()) {
            if (QSGTexture* own = texture->removedFromAtlas(); own && own != texture) {
                delete texture;
                texture = own;
            }
        }
        if (texture) {
            texture->setFiltering(QSGTexture::Linear);
            texture->setMipmapFiltering(QSGTexture::None);
            texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
            texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
        }
        m_root->texture = texture;
    }
    const bool textAvailable = atlasReady && m_root->texture != nullptr;

    const double pxPerMs = camera.pxPerMs() / scale;
    const double pxPerPrice = camera.pxPerPrice() / scale;
    const double basePx = m_grid.tick * pxPerPrice;
    const int64_t topAbs = std::llround(m_grid.maxPrice / m_grid.tick);
    uint64_t dataVersions = 0;
    for (const auto& [start, session] : m_sessions) dataVersions += session.dataVersion;
    const GroupKey groupKey{quantize(basePx), quantize(pxPerMs * 3'600'000.0), dataVersions,
                            style.layout, quantize(style.rowPx), m_sessions.size()};
    if (!(groupKey == m_groupKey)) {
        m_group = chooseRowGroup(basePx, pxPerMs, topAbs, style);
        m_groupKey = groupKey;
    }
    const int group = m_group;
    const double cellH = basePx * group;
    const double anchorY = surfaceBounds.top() + mapping.priceToScreenY(static_cast<double>(topAbs) * m_grid.tick);
    const auto& capGlyph = atlas.glyph(QChar('A'));
    const double advanceRatio = (atlasReady && atlas.fontPx() > 0 && capGlyph.advance > 0.0f)
        ? capGlyph.advance / atlas.fontPx() : 0.6;

    // Profiles depend on data and row grouping only. Collapsed cells share one
    // width across sessions, so every profile reads on the same scale; it depends
    // on zoom and the widest row of any retained session, never on panning.
    double collapsedCellW = std::min(cellH * kCollapsedAspect, kMaxCollapsedCellW);
    for (auto& [start, session] : m_sessions) {
        if (session.periods <= 0) {
            continue;
        }
        if (session.profileVersion != session.dataVersion || session.profileGroup != group) {
            session.profile.build(session.rowsByPeriod, topAbs, group);
            session.profileVersion = session.dataVersion;
            session.profileGroup = group;
        }
        const double sessionPx = static_cast<double>(session.endMs - session.startMs) * pxPerMs;
        collapsedCellW = std::min(collapsedCellW,
                                  sessionPx * kCollapsedSessionFill / std::max(1, session.profile.maxCount()));
    }

    for (auto& [start, session] : m_sessions) {
        ensureSessionNodes(session);
        if (session.periods <= 0) {
            continue;
        }
        if (textAvailable) {
            session.lightText->setSharedAtlasTexture(m_root->texture);
            session.darkText->setSharedAtlasTexture(m_root->texture);
            session.lightText->setPxRange(atlas.pxRange());
            session.darkText->setPxRange(atlas.pxRange());
        }
        const double startX = surfaceBounds.left() + mapping.timeToScreenX(static_cast<double>(session.startMs));
        const double endX = surfaceBounds.left() + mapping.timeToScreenX(static_cast<double>(session.endMs));
        const bool onScreen = endX > surfaceBounds.left() && startX < surfaceBounds.right();

        const double periodPx = static_cast<double>(session.periodMs) * pxPerMs;
        const double cellW = std::max(style.layout == tpo::Layout::Collapsed ? collapsedCellW : periodPx, 1.0);
        const double fontPx = std::min({kMaxFontPx, cellH * kFontPerCellH, cellW * kAdvanceFill / advanceRatio});

        LayoutKey key;
        key.dataVersion = session.profileVersion;
        key.group = group;
        // Quantized sizes leave cells behind the continuous camera during a glide.
        // Exact sizes are stable at rest and rebuild only as the drawn scale changes.
        key.cellW = cellW;
        key.cellH = cellH;
        key.dpr = scale;
        key.free = free;
        key.layout = style.layout;
        key.theme = style.theme;
        key.text = textAvailable && fontPx >= kTextHiddenPx;
        if (onScreen && (!session.hasBuilt || !(key == session.built))) {
            rebuildSession(session, key, style, atlas, basePx, periodPx, topAbs,
                           static_cast<float>(fontPx));
            session.built = key;
            session.hasBuilt = true;
        }

        double anchorX = startX;
        if (style.layout == tpo::Layout::Collapsed && startX < surfaceBounds.left()) {
            // Keep the profile readable while its session is on screen.
            anchorX = std::max(startX, std::min(surfaceBounds.left(), endX - session.profileWidthPx));
        }
        const double tx = edge(anchorX);
        const double ty = edge(anchorY);
        if (tx != session.lastTx || ty != session.lastTy) {
            QMatrix4x4 matrix;
            matrix.translate(static_cast<float>(tx), static_cast<float>(ty));
            session.transform->setMatrix(matrix);
            session.transform->markDirty(QSGNode::DirtyMatrix);
            session.lastTx = tx;
            session.lastTy = ty;
        }
    }

    sLog_Probe("tpo.frame", "sessions=" << m_sessions.size() << " layout=" << tpo::layoutName(style.layout)
               << " basePx=" << basePx << " group=" << group << " cellH=" << cellH
               << " view=[" << viewStartMs << ".." << viewEndMs << "]");
}
