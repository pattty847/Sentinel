#pragma once
#include "TradeOverlayMapping.hpp"
#include "ChartRaster.hpp"

#include "ChartTextPrimitives.hpp"
#include "TpoProfileModel.hpp"

#include <QByteArray>
#include <QRectF>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

class ChartTextAtlas;
class ChartTextNode;
class QQuickWindow;
class QSGClipNode;
class QSGGeometryNode;
class QSGNode;
class QSGOpacityNode;
class QSGTransformNode;
class TpoRootNode;

/*
 * TpoOverlayRenderer
 *
 * Market profile (TPO) overlay. Draws one profile per retained session as
 * coloured letter cells (vertex-colour quads) with MSDF letters on top.
 *
 *   Split:     each period's letters stay in its own time column
 *              [sessionStart + p*period, +period), aligned with candles.
 *   Collapsed: each row's letters pack left-to-right from the session start
 *              (classic market profile). The profile sticks to the left edge of
 *              the plot while its session is still on screen.
 *
 * Every look comes from tpo::StyleModel (chart/core/TpoStyle.hpp): cell fill,
 * letters, the value-area rule, and the POC/VAH/VAL/IBH/IBL lines. Level lines
 * sit under the cells and reach across the profile or the whole session; the
 * newest session's levels also get tags at the plot's right edge.
 *
 * Geometry lives in per-session layout space relative to the session anchor
 * (session start, top of the trade grid). Panning only moves the session's
 * QSGTransformNode; cells and glyphs are rebuilt only when that session's data,
 * the row grouping, the cell size (zoom), the layout or the theme change.
 *
 * Threading: enqueue()/setStyle() on the GUI thread; everything else on the
 * render thread inside updatePaintNode.
 */
class TpoOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;              // period index within the session
        int gridWidth = 0;      // periods in the session
        int gridHeight = 0;     // trade grid rows
        QByteArray letters;     // one byte per row, '\0' = not visited
        TradeOverlayGrid grid;  // startMs/endMs = session bounds
        int64_t periodMs = 0;
    };

    struct Style {
        tpo::Layout layout = tpo::Layout::Collapsed;
        tpo::StyleModel look = tpo::presetStyle(tpo::Preset::Letters);
        double rowPx = 13.0;    // minimum display row height before rows merge
        int maxSessions = 5;
    };

    TpoOverlayRenderer();
    ~TpoOverlayRenderer();

    /// Thread-safe enqueue (GUI thread).
    void enqueue(PendingUpload upload);
    /// Thread-safe style change (GUI thread). Applied on the next render.
    void setStyle(const Style& style);
    Style style() const;

    void onRootRebuilt();

    // World -> screen uses the published frame camera; DPR and per-axis snapping also
    // participate in the cached layout. No integer time-bound conversion.
    void render(QQuickWindow* window,
                QSGNode* parentNode,
                bool drawTpo,
                const ChartTextAtlas& atlas,
                bool atlasReady,
                const chart_raster::RasterCamera& camera, double dpr, bool snapX, bool snapY,
                const QRectF& surfaceBounds);

    // Test access (render thread state, read after render()).
    struct TagView {
        int count = 0;
        std::array<tpo::Level, 5> levels{};
        std::array<double, 5> centreY{};  // chip centres, item coordinates
        int64_t sessionStartMs = 0;
    };
    const TagView& tagsForTest() const { return m_tagView; }
    struct SessionNodes {
        const QSGTransformNode* cells = nullptr;
        const QSGGeometryNode* cellGeometry = nullptr;
        const QSGTransformNode* levels = nullptr;
        const QSGGeometryNode* levelGeometry = nullptr;
    };
    SessionNodes sessionNodesForTest(int64_t sessionStartMs) const;

private:
    struct LayoutKey {
        uint64_t dataVersion = 0;
        int group = 0;
        double cellW = 0.0;
        double cellH = 0.0;
        double dpr = 1.0;
        bool snapX = true, snapY = true;
        tpo::Layout layout = tpo::Layout::Collapsed;
        tpo::StyleModel look;
        double sessionPx = 0.0;  // session width; only when level lines reach the session
        bool text = false;
        bool operator==(const LayoutKey&) const = default;
    };

    struct Session {
        int64_t startMs = 0;
        int64_t endMs = 0;
        int64_t periodMs = 0;
        int periods = 0;
        std::vector<std::vector<int>> rowsByPeriod;
        uint64_t dataVersion = 1;

        tpo::ProfileRows profile;
        uint64_t profileVersion = 0;
        int profileGroup = 0;

        QSGTransformNode* transform = nullptr;
        QSGGeometryNode* cells = nullptr;
        // Level lines and the value-area band, under the cells. Anchored at the
        // profile (Profile reach) or the session start (Session reach).
        QSGTransformNode* levelTransform = nullptr;
        QSGGeometryNode* levels = nullptr;
        ChartTextNode* lightText = nullptr;
        ChartTextNode* darkText = nullptr;
        int cellCapacity = 0;
        LayoutKey built;
        bool hasBuilt = false;
        double profileWidthPx = 0.0;
        double lastTx = 0.0, lastTy = 0.0, lastLevelTx = 0.0;
        float textAlpha = 0.0f;
        std::vector<ChartGlyphInstance> lightGlyphs;
        std::vector<ChartGlyphInstance> darkGlyphs;
    };

    struct GroupKey {
        int64_t basePx = -1;
        int64_t pxPerHour = -1;
        uint64_t dataVersions = 0;
        tpo::Layout layout = tpo::Layout::Collapsed;
        int64_t rowPx = 0;
        size_t sessions = 0;
        bool operator==(const GroupKey&) const = default;
    };

    struct TagKey {
        int count = 0;
        std::array<tpo::Level, 5> levels{};
        std::array<double, 5> ys{};  // line screen y, item coordinates
        int64_t sessionStartMs = 0;
        tpo::StyleModel look;
        QRectF bounds;
        bool operator==(const TagKey&) const = default;
    };

    void ensureProfile(Session& session, int64_t topAbs, int group);
    void rebuildLevels(Session& session, const LayoutKey& key, int64_t topAbs, double basePx);
    TagKey tagKey(const Session* tagged, const TimeAxisMapping& mapping, bool metrics, const QRectF& bounds) const;
    void updateTags(const ChartTextAtlas& atlas, const TagKey& key);
    int chooseRowGroup(double basePx, double pxPerMs, int64_t topAbs, const Style& style);
    void applyUploads(std::vector<PendingUpload>& uploads, int maxSessions);
    void clearSessions();
    void dropSessionNodes(Session& session);
    void ensureRoot(QSGNode* parentNode);
    void ensureSessionNodes(Session& session);
    void rebuildSession(Session& session, const LayoutKey& key, const Style& style,
                        const ChartTextAtlas& atlas, double basePx, double periodPx,
                        int64_t topAbs, float textFontPx);
    void appendGlyph(std::vector<ChartGlyphInstance>& out, const ChartTextAtlas& atlas,
                     char letter, float cx, float cy, float scale) const;

    // GUI -> render handoff.
    mutable std::mutex m_pendingMutex;
    std::vector<PendingUpload> m_pendingUploads;
    Style m_pendingStyle;

    // Render-thread state.
    std::vector<PendingUpload> m_drain;
    Style m_style;
    TradeOverlayGrid m_grid;
    int m_gridRows = 0;
    std::map<int64_t, Session> m_sessions;
    TpoRootNode* m_root = nullptr;
    QSGOpacityNode* m_opacity = nullptr;
    QSGClipNode* m_clip = nullptr;
    QSGNode* m_sessionsRoot = nullptr;
    QSGGeometryNode* m_tagChips = nullptr;
    ChartTextNode* m_tagLight = nullptr;
    ChartTextNode* m_tagDark = nullptr;
    std::vector<ChartGlyphInstance> m_tagLightGlyphs, m_tagDarkGlyphs;
    TagKey m_tagKey;
    TagView m_tagView;
    bool m_hasTags = false;
    QRectF m_clipRect;
    GroupKey m_groupKey;
    int m_group = 1;
};
