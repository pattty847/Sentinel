/*
 * Sentinel – VolumeProfileRenderer
 *
 * Render-thread renderer for Mode A (Session Volume Profile).
 * Draws a right-wall-anchored horizontal histogram using QSG geometry nodes.
 *
 * Visual layout (drawRect is the full chart area):
 *
 *   |←──── chart area ───────────────────────────────────→|←VP width→|
 *   |                                                       |███████POC|  VAH
 *   |                                                       |████      |
 *   |                                                       |██████████|  ← POC (bright)
 *   |                                                       |██████    |
 *   |                                                       |███       |  VAL
 *
 * When overlay=true the histogram is drawn on top of the primary series
 * (transparent background); when overlay=false a margin is reserved so the
 * histogram does not occlude candles.
 *
 * Nodes owned (all appended as children of parentNode):
 *   - m_vaNode     : semi-transparent VA band rectangle
 *   - m_barsNode   : per-bin volume bars (QSGGeometryNode, vertex-color)
 *   - m_pocNode    : POC horizontal line
 *
 * Geometry is retained until bins, mapping or style changes.
 */
#pragma once

#include "VolumeProfileState.hpp"

#include <QColor>
#include <QRectF>
#include <array>
#include <mutex>
#include <vector>

class QQuickWindow;
class QSGNode;
class QSGGeometryNode;

class VolumeProfileRenderer {
public:
    /// Thread-safe enqueue (called from GUI thread).
    void enqueue(std::vector<float> bins, VolumeProfileState::Snapshot snap);
    /// Swap new pending data into retained render-thread storage.
    /// With no update, the caller's bins and snapshot remain unchanged.
    /// Returns true if new data was available.
    bool drainPending(std::vector<float>& bins, VolumeProfileState::Snapshot& snap);
    /// Clear all pending data (called from GUI thread on clearData).
    void clearPending();

    // ── Configuration ──────────────────────────────────────────────────────
    // Width of the histogram as a fraction of the chart width (0–1).
    void setWidthFraction(float frac);
    // When true, histogram overlays candles; when false it uses a right margin.
    void setOverlay(bool overlay);
    // Colours
    void setBarColor(const QColor& color);
    void setVaColor(const QColor& color);
    void setPocColor(const QColor& color);

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void onRootRebuilt();

    // ── Hot path ──────────────────────────────────────────────────────────
    // Called once per frame from the render thread.
    // drawRect    : the full chart drawing rectangle (screen pixels)
    // viewMinPrice: bottom of the visible price range
    // viewMaxPrice: top  of the visible price range
    // bins        : volume per price bin (top→bottom, may be empty = no data)
    //               Direct callers must advance snap.generation when bins change;
    //               drainPending also invalidates on every delivered update/clear.
    // snap        : snapshot from VolumeProfileState (price range, VA, etc.)
    // dpr / snapY : frame surface ratio and whether the camera price axis is whole-pixel
    void render(QSGNode* parentNode,
                bool drawVp,
                const QRectF& drawRect,
                double viewMinPrice,
                double viewMaxPrice,
                const std::vector<float>& bins,
                const VolumeProfileState::Snapshot& snap,
                double dpr, bool snapY);

private:
    friend struct RenderPerformanceTestAccess;
    struct GeometryInputs {
        std::array<double, 4> rect;
        double viewMin, viewMax, dpr, minPrice, tickSize, val, vah, poc;
        uint64_t generation, deliveryRevision;
        size_t binCount;
        int gridHeight;
        bool draw, snapY, validVa;
        bool operator==(const GeometryInputs&) const = default;
    };
    GeometryInputs m_inputs{};
    bool m_inputsValid = false;
    uint64_t m_deliveryRevision = 0; // render thread only
    uint64_t m_geometryRebuilds = 0;
    uint64_t m_geometryAllocations = 0;

    void ensureNodes(QSGNode* parentNode);
    void clearGeometry();
    // Rebuild all three geometry nodes from the current bins + snap.
    void rebuildGeometry(const QRectF& drawRect,
                         double viewMinPrice,
                         double viewMaxPrice,
                         const std::vector<float>& bins,
                         const VolumeProfileState::Snapshot& snap,
                         double dpr, bool snapY);

    // ── Node pointers (owned by the QSG tree) ─────────────────────────────
    QSGGeometryNode* m_vaNode   = nullptr;  // VA band
    QSGGeometryNode* m_barsNode = nullptr;  // histogram bars
    QSGGeometryNode* m_pocNode  = nullptr;  // POC line

    int m_barCapacity = 0; // allocated bins, independent of the active draw count

    // ── Settings ──────────────────────────────────────────────────────────
    float  m_widthFraction = 0.12f;   // 12 % of chart width by default
    bool   m_overlay       = false;
    QColor m_barColor  = QColor(100, 160, 220, 180);
    QColor m_vaColor   = QColor( 60, 200, 100,  60);
    QColor m_pocColor  = QColor(255, 215,   0, 240);

    mutable std::mutex m_pendingMutex;
    std::vector<float> m_pendingBins;
    VolumeProfileState::Snapshot m_pendingSnap;
    bool m_pendingDirty = false;
};
