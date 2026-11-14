#pragma once
#include "chart/feeds/CvdFeed.hpp"
#include <QSGClipNode>
#include <QSGGeometry>
#include <functional>

// Keeps the unmodified price mapping and excludes a docked bottom pane.
// The clip is detached at zero inset, preserving the original scene tree.
class ChartPaneClip : public QSGNode {
public:
    ChartPaneClip() : geometry_(QSGGeometry::defaultAttributes_Point2D(), 4) {
        geometry_.setDrawingMode(QSGGeometry::DrawTriangleStrip);
        clip_.setFlag(OwnedByParent, false);
        clip_.setGeometry(&geometry_);
        clip_.setIsRectangular(true);
    }
    ~ChartPaneClip() override {
        if (clip_.parent()) removeChildNode(&clip_);
    }
    QSGNode* content() { return active_ ? static_cast<QSGNode*>(&clip_) : this; }
    QRectF clipRect() const { return clip_.clipRect(); }
    void sync(const QRectF& surface, double inset) {
        const bool active = inset > 0;
        if (active != active_) {
            QSGNode* from = active ? this : static_cast<QSGNode*>(&clip_);
            QSGNode* to = active ? static_cast<QSGNode*>(&clip_) : this;
            if (!active) removeChildNode(&clip_);
            while (auto* child = from->firstChild()) {
                from->removeChildNode(child); to->appendChildNode(child);
            }
            if (active) appendChildNode(&clip_);
            active_ = active;
        }
        if (!active) return;
        auto rect = surface;
        rect.setHeight(std::max(0.0, surface.height() - inset));
        if (clip_.clipRect() == rect) return;
        clip_.setClipRect(rect);
        QSGGeometry::updateRectGeometry(&geometry_, rect);
        clip_.markDirty(DirtyGeometry);
    }
private:
    QSGGeometry geometry_;
    QSGClipNode clip_;
    bool active_ = false;
};

// Captured pointers/values only; afterSynchronizing runs with the GUI blocked.
struct ChartPaneClipLink {
    const chart::CvdPresentation* presentation = nullptr;
    ChartPaneClip* node = nullptr;
    QRectF surface;
    int64_t timeframeMs = 0;
    std::function<void(double)> syncInset;
    void sync() const {
        if (!node) return;
        const double inset = presentation ? presentation->bottomInset(timeframeMs, surface.height()) : 0;
        if (syncInset) syncInset(inset);
        node->sync(surface, inset);
    }
};
