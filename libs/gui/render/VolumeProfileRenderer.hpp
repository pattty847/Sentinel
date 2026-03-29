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
 * All geometry is rebuilt each frame (cost: O(gridHeight) geometry update).
 * Grid heights are typically 2048; at 110 fps this is well within budget.
 */
#pragma once

#include "IOverlayRenderer.hpp"
#include "VolumeProfileState.hpp"

#include <QColor>
#include <QRectF>
#include <mutex>
#include <vector>

class QQuickWindow;
class QSGNode;
class QSGGeometryNode;

class VolumeProfileRenderer : public IOverlayRenderer {
public:
    /// Thread-safe enqueue (called from GUI thread).
    void enqueue(std::vector<float> bins, VolumeProfileState::Snapshot snap);
    /// Drain pending data into local copies (called from render thread).
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
    void onRootRebuilt() override;
    int zOrder() const override { return 3; }

    // ── Hot path ──────────────────────────────────────────────────────────
    // Called once per frame from the render thread.
    // drawRect    : the full chart drawing rectangle (screen pixels)
    // viewMinPrice: bottom of the visible price range
    // viewMaxPrice: top  of the visible price range
    // bins        : volume per price bin (top→bottom, may be empty = no data)
    // snap        : snapshot from VolumeProfileState (price range, VA, etc.)
    void render(QSGNode* parentNode,
                bool drawVp,
                const QRectF& drawRect,
                double viewMinPrice,
                double viewMaxPrice,
                const std::vector<float>& bins,
                const VolumeProfileState::Snapshot& snap);

private:
    void ensureNodes(QSGNode* parentNode);
    void clearGeometry();
    // Rebuild all three geometry nodes from the current bins + snap.
    void rebuildGeometry(const QRectF& drawRect,
                         double viewMinPrice,
                         double viewMaxPrice,
                         const std::vector<float>& bins,
                         const VolumeProfileState::Snapshot& snap);

    // ── Node pointers (owned by the QSG tree) ─────────────────────────────
    QSGGeometryNode* m_vaNode   = nullptr;  // VA band
    QSGGeometryNode* m_barsNode = nullptr;  // histogram bars
    QSGGeometryNode* m_pocNode  = nullptr;  // POC line

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
