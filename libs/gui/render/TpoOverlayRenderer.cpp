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
// Collapsed profiles use at most this share of their session's width.
constexpr double kCollapsedSessionFill = 0.96;
// Letter font size relative to the cell; the glyph advance must fit the width.
constexpr double kFontPerCellH = 0.80;
constexpr double kAdvanceFill = 0.78;  // the glyph advance may use this share of the cell width
// Letters fade in between these font sizes (px).
constexpr double kTextHiddenPx = 7.0;
constexpr double kTextFullPx = 10.0;

const QColor kDarkText(18, 22, 28);
// Tag chips: horizontal padding and the gap to the plot's right edge (px).
constexpr double kTagPadPx = 4.0;
constexpr double kTagMarginPx = 4.0;
constexpr int kTagVertices = 5 * 6;

QColor qcolor(const tpo::Rgba& c, double alpha) {
    return QColor(c.r, c.g, c.b, static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0)));
}

QSGGeometryNode* colorNode() {
    auto* node = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(new QSGVertexColorMaterial());
    node->setFlag(QSGNode::OwnsMaterial, true);
    return node;
}

void setTransform(QSGTransformNode* node, double tx, double ty) {
    QMatrix4x4 matrix;
    matrix.translate(static_cast<float>(tx), static_cast<float>(ty));
    node->setMatrix(matrix);
    node->markDirty(QSGNode::DirtyMatrix);
}

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
    m_sessionsRoot = nullptr;
    m_tagChips = nullptr;
    m_tagLight = nullptr;
    m_tagDark = nullptr;
    m_hasTags = false;
    m_clipRect = QRectF();
    for (auto& [start, session] : m_sessions) {
        session.transform = nullptr;
        session.levelTransform = nullptr;
        session.levels = nullptr;
        session.cells = nullptr;
        session.lightText = nullptr;
        session.darkText = nullptr;
        session.cellCapacity = 0;
        session.hasBuilt = false;
    }
}

void TpoOverlayRenderer::dropSessionNodes(Session& session) {
    for (QSGTransformNode* node : {session.transform, session.levelTransform}) {
        if (!node) continue;
        if (QSGNode* parent = node->parent()) {
            parent->removeChildNode(node);
        }
        delete node;  // deletes its geometry and text children (owned by parent)
    }
    session.transform = nullptr;
    session.levelTransform = nullptr;
    session.levels = nullptr;
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
    m_sessionsRoot = new QSGNode();
    m_clip->appendChildNode(m_sessionsRoot);
    m_tagChips = colorNode();
    m_tagChips->geometry()->allocate(kTagVertices);
    m_tagLight = new ChartTextNode();
    m_tagDark = new ChartTextNode();
    m_clip->appendChildNode(m_tagChips);
    m_clip->appendChildNode(m_tagLight);
    m_clip->appendChildNode(m_tagDark);
    m_hasTags = false;
    parentNode->appendChildNode(m_root);
    m_clipRect = QRectF();
}

void TpoOverlayRenderer::ensureSessionNodes(Session& session) {
    if (session.transform) {
        return;
    }
    session.levelTransform = new QSGTransformNode();
    session.levels = colorNode();
    session.levelTransform->appendChildNode(session.levels);
    m_sessionsRoot->appendChildNode(session.levelTransform);
    session.transform = new QSGTransformNode();
    session.cells = colorNode();
    session.lightText = new ChartTextNode();
    session.darkText = new ChartTextNode();
    session.transform->appendChildNode(session.cells);
    session.transform->appendChildNode(session.lightText);
    session.transform->appendChildNode(session.darkText);
    m_sessionsRoot->appendChildNode(session.transform);
    session.cellCapacity = 0;
    session.hasBuilt = false;
    session.lastTx = std::numeric_limits<double>::quiet_NaN();
    session.lastTy = std::numeric_limits<double>::quiet_NaN();
    session.lastLevelTx = std::numeric_limits<double>::quiet_NaN();
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
    const tpo::StyleModel& look = key.look;
    const bool split = key.layout == tpo::Layout::Split;
    const double cellW = key.cellW;
    const double cellH = key.cellH;
    const float gap = static_cast<float>(look.cells.width);
    const float gapX = cellW >= 6.0 ? gap : 0.0f;
    const float gapY = cellH >= 6.0 ? gap : 0.0f;
    const auto edge = [&key](double logical, bool snap) {
        return snap ? std::round(logical * key.dpr) / key.dpr : logical;
    };
    const float minCell = static_cast<float>(1.0 / key.dpr);
    const int group = profile.group();

    int lastPeriod = -1;
    for (int p = session.periods - 1; p >= 0; --p) {
        if (!session.rowsByPeriod[static_cast<size_t>(p)].empty()) { lastPeriod = p; break; }
    }

    const int quads = look.cells.visible ? profile.totalTpos() : 0;
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

    double widest = 0.0;
    for (int row = 0; row < profile.rows(); ++row) {
        if (profile.count(row) == 0) {
            continue;
        }
        const double top = rowTop(row);
        const float y0 = static_cast<float>(edge(top, key.snapY));
        const float y1 = static_cast<float>(edge(top + cellH - gapY, key.snapY));
        const float cy = static_cast<float>(top + cellH * 0.5);
        const bool inVa = va.contains(row);
        const bool poc = row == va.poc;
        int k = 0;
        profile.forEachPeriod(row, [&](int period) {
            const double left = split ? period * periodPx : k * cellW;
            const float x0 = static_cast<float>(edge(left, key.snapX));
            const float x1 = static_cast<float>(edge(left + cellW - gapX, key.snapX));
            const tpo::Rgba color = tpo::cellColor(look, period, session.periods, inVa, poc);
            if (look.cells.visible && written + 6 <= session.cellCapacity) {
                writeQuad(v + written, x0, y0, std::max(x0 + minCell, x1), std::max(y0 + minCell, y1), color);
                written += 6;
            }
            if (key.text) {
                const bool dark = look.cells.visible && tpo::prefersDarkText(color);
                appendGlyph(dark ? session.darkGlyphs : session.lightGlyphs, atlas, tpo::letterForPeriod(period),
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
    rebuildLevels(session, key, topAbs, basePx);

    session.lightText->updateGeometry(session.lightGlyphs);
    session.darkText->updateGeometry(session.darkGlyphs);

    const double fade = std::clamp((textFontPx - kTextHiddenPx) / (kTextFullPx - kTextHiddenPx), 0.0, 1.0);
    session.textAlpha = key.text ? static_cast<float>(fade * look.letters.opacity) : 0.0f;
    session.lightText->setColor(qcolor(look.letters.color, session.textAlpha));
    QColor dark = kDarkText;
    dark.setAlphaF(session.textAlpha);
    session.darkText->setColor(dark);

    sLog_Probe("tpo.build", "session=" << session.startMs << " periods=" << session.periods
               << " layout=" << tpo::layoutName(key.layout) << " theme=" << tpo::themeName(look.theme)
               << " group=" << group << " rows=" << profile.rows() << " tpos=" << profile.totalTpos()
               << " maxCount=" << profile.maxCount() << " poc=" << va.poc
               << " va=[" << va.low << ".." << va.high << "]"
               << " ib=[" << profile.initialBalance().lowAbs << ".." << profile.initialBalance().highAbs << "]"
               << " cell=" << cellW << "x" << cellH << " font=" << textFontPx
               << " glyphs=" << (session.lightGlyphs.size() + session.darkGlyphs.size()));
}

void TpoOverlayRenderer::rebuildLevels(Session& session, const LayoutKey& key, int64_t topAbs, double basePx) {
    const tpo::StyleModel& look = key.look;
    const tpo::ProfileRows& profile = session.profile;
    const tpo::ValueArea& va = profile.valueArea();
    const tpo::LevelLines lines = tpo::levelLines(profile, look);
    const double reach = look.reach == tpo::LineReach::Session ? key.sessionPx : session.profileWidthPx;
    const auto snap = [&key](double logical) { return std::round(logical * key.dpr) / key.dpr; };
    const float x1 = static_cast<float>(key.snapX ? snap(reach) : reach);
    const double minPx = 1.0 / key.dpr;

    QSGGeometry* geometry = session.levels->geometry();
    if (geometry->vertexCount() != 6 * 6) geometry->allocate(6 * 6);
    auto* v = geometry->vertexDataAsColoredPoint2D();
    std::memset(static_cast<void*>(v), 0, sizeof(QSGGeometry::ColoredPoint2D) * 36);
    int written = 0;
    const double cellH = basePx * profile.group();
    if (look.valueArea.visible && va.valid() && reach > 0.0) {
        const double top = static_cast<double>(topAbs - (profile.minGroup() + va.high + 1) * profile.group()) * basePx;
        const double bottom = top + (va.high - va.low + 1) * cellH;
        writeQuad(v + written, 0.0f, static_cast<float>(snap(top)), x1, static_cast<float>(snap(bottom)),
                  tpo::elementColor(look.valueArea));
        written += 6;
    }
    for (int i = 0; i < lines.count && reach > 0.0; ++i) {
        const auto& line = lines.lines[static_cast<size_t>(i)];
        const tpo::Element& element = tpo::levelElement(look, line.level);
        const double width = std::max(element.width, minPx);
        const double y = (static_cast<double>(topAbs) - line.ticks) * basePx;
        const double y0 = key.snapY ? snap(y - width * 0.5) : y - width * 0.5;
        writeQuad(v + written, 0.0f, static_cast<float>(y0), x1, static_cast<float>(y0 + width),
                  tpo::elementColor(element));
        written += 6;
    }
    session.levels->markDirty(QSGNode::DirtyGeometry);
}

TpoOverlayRenderer::TagKey TpoOverlayRenderer::tagKey(const Session* tagged, const TimeAxisMapping& mapping,
                                                     bool metrics, const QRectF& bounds) const {
    TagKey key;
    key.look = m_style.look;
    key.bounds = bounds;
    if (!tagged || !metrics || !m_style.look.tags.visible) return key;
    const tpo::LevelLines lines = tpo::levelLines(tagged->profile, m_style.look);
    key.sessionStartMs = tagged->startMs;
    key.count = lines.count;
    for (int i = 0; i < lines.count; ++i) {
        const auto& line = lines.lines[static_cast<size_t>(i)];
        key.levels[static_cast<size_t>(i)] = line.level;
        key.ys[static_cast<size_t>(i)] = bounds.top() + mapping.priceToScreenY(line.ticks * m_grid.tick);
    }
    return key;
}

void TpoOverlayRenderer::updateTags(const ChartTextAtlas& atlas, const TagKey& key) {
    if (m_hasTags && key == m_tagKey) return;
    m_tagKey = key;
    m_hasTags = true;

    const tpo::StyleModel& look = key.look;
    const QRectF& bounds = key.bounds;
    const double fontPx = std::max(1.0, look.tags.width);
    const float scale = static_cast<float>(fontPx / std::max(1, atlas.fontPx()));
    const double advance = atlas.glyph(QChar('A')).advance * scale;
    const double chipH = std::round(fontPx * 1.35);
    const double chipW = std::round(3 * advance + 2 * kTagPadPx);
    const double left = std::round(bounds.right() - kTagMarginPx - chipW);
    const unsigned kept = tpo::placeTags(key.ys, key.count, chipH, bounds.top(), bounds.bottom());

    auto* v = m_tagChips->geometry()->vertexDataAsColoredPoint2D();
    std::memset(static_cast<void*>(v), 0, sizeof(QSGGeometry::ColoredPoint2D) * kTagVertices);
    m_tagLightGlyphs.clear();
    m_tagDarkGlyphs.clear();
    m_tagView = {};
    m_tagView.sessionStartMs = key.sessionStartMs;
    int written = 0;
    for (int i = 0; i < key.count; ++i) {
        if (!(kept & (1u << i))) continue;
        const tpo::Level level = key.levels[static_cast<size_t>(i)];
        const double top = std::round(key.ys[static_cast<size_t>(i)] - chipH * 0.5);
        tpo::Element chipStyle = tpo::levelElement(look, level);
        chipStyle.opacity = look.tags.opacity;
        const tpo::Rgba chip = tpo::elementColor(chipStyle);
        writeQuad(v + written, static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(left + chipW), static_cast<float>(top + chipH), chip);
        written += 6;
        auto& out = tpo::prefersDarkText(chip) ? m_tagDarkGlyphs : m_tagLightGlyphs;
        const char* name = tpo::levelName(level);
        for (int c = 0; c < 3; ++c) {
            appendGlyph(out, atlas, name[c], static_cast<float>(left + kTagPadPx + (c + 0.5) * advance),
                        static_cast<float>(top + chipH * 0.5), scale);
        }
        m_tagView.levels[static_cast<size_t>(m_tagView.count)] = level;
        m_tagView.centreY[static_cast<size_t>(m_tagView.count++)] = top + chipH * 0.5;
    }
    m_tagChips->markDirty(QSGNode::DirtyGeometry);
    m_tagLight->updateGeometry(m_tagLightGlyphs);
    m_tagDark->updateGeometry(m_tagDarkGlyphs);
    m_tagLight->setColor(qcolor(look.tags.color, 1.0));
    m_tagDark->setColor(kDarkText);
}

TpoOverlayRenderer::SessionNodes TpoOverlayRenderer::sessionNodesForTest(int64_t sessionStartMs) const {
    const auto it = m_sessions.find(sessionStartMs);
    if (it == m_sessions.end()) return {};
    const Session& s = it->second;
    return {s.transform, s.cells, s.levelTransform, s.levels};
}

void TpoOverlayRenderer::ensureProfile(Session& session, int64_t topAbs, int group) {
    if (session.profileVersion == session.dataVersion && session.profileGroup == group) return;
    session.profile.build(session.rowsByPeriod, topAbs, group, tpo::initialBalancePeriods(session.periodMs));
    session.profileVersion = session.dataVersion;
    session.profileGroup = group;
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
            ensureProfile(session, topAbs, group);
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
                                const chart_raster::RasterCamera& camera, double dpr, bool snapX, bool snapY,
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
    const auto edge = [scale](double logical, bool snap) {
        return snap ? std::round(logical * scale) / scale : logical;
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
    if (textAvailable) {
        for (ChartTextNode* node : {m_tagLight, m_tagDark}) {
            node->setSharedAtlasTexture(m_root->texture);
            node->setPxRange(atlas.pxRange());
        }
    }

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
        ensureProfile(session, topAbs, group);
        const double sessionPx = static_cast<double>(session.endMs - session.startMs) * pxPerMs;
        collapsedCellW = std::min(collapsedCellW,
                                  sessionPx * kCollapsedSessionFill / std::max(1, session.profile.maxCount()));
    }

    const Session* tagged = nullptr;
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
        if (onScreen) tagged = &session;

        const double periodPx = static_cast<double>(session.periodMs) * pxPerMs;
        const double cellW = std::max(style.layout == tpo::Layout::Collapsed ? collapsedCellW : periodPx, 1.0);
        const double fontPx = std::min({style.look.letters.width, cellH * kFontPerCellH, cellW * kAdvanceFill / advanceRatio});
        const double sessionPx = static_cast<double>(session.endMs - session.startMs) * pxPerMs;

        LayoutKey key;
        key.dataVersion = session.profileVersion;
        key.group = group;
        // Quantized sizes leave cells behind the continuous camera during a glide.
        // Exact sizes are stable at rest and rebuild only as the drawn scale changes.
        key.cellW = cellW;
        key.cellH = cellH;
        key.dpr = scale;
        key.snapX = snapX;
        key.snapY = snapY;
        key.layout = style.layout;
        key.look = style.look;
        key.sessionPx = style.look.reach == tpo::LineReach::Session ? sessionPx : 0.0;
        key.text = textAvailable && style.look.letters.visible && fontPx >= kTextHiddenPx;
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
        const double tx = edge(anchorX, snapX);
        const double ty = edge(anchorY, snapY);
        const double levelTx = style.look.reach == tpo::LineReach::Session ? edge(startX, snapX) : tx;
        if (tx != session.lastTx || ty != session.lastTy) {
            setTransform(session.transform, tx, ty);
            session.lastTx = tx;
        }
        if (levelTx != session.lastLevelTx || ty != session.lastTy) {
            setTransform(session.levelTransform, levelTx, ty);
            session.lastLevelTx = levelTx;
        }
        session.lastTy = ty;
    }
    updateTags(atlas, tagKey(tagged, mapping, atlasReady && atlas.isBuilt() && atlas.fontPx() > 0, surfaceBounds));

    sLog_Probe("tpo.frame", "sessions=" << m_sessions.size() << " layout=" << tpo::layoutName(style.layout)
               << " basePx=" << basePx << " group=" << group << " cellH=" << cellH
               << " view=[" << viewStartMs << ".." << viewEndMs << "]");
}
