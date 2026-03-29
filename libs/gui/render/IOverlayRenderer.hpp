/// Minimal lifecycle interface for chart overlay renderers.
///
/// Captures the uniform parts of overlay management (root-rebuilt notification,
/// z-ordering) without imposing a virtual render() method.  Each overlay's
/// render call has materially different data dependencies, so concrete dispatch
/// stays in UnifiedGridRenderer::renderOverlays().
#pragma once

class IOverlayRenderer {
public:
    virtual ~IOverlayRenderer() = default;

    /// Called when the QSG root node is recreated and all child node pointers
    /// must be considered stale.
    virtual void onRootRebuilt() = 0;

    /// Determines paint order: lower values paint first (behind higher values).
    virtual int zOrder() const = 0;
};
