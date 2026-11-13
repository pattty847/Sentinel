#pragma once
#include "TradeBubbleData.hpp"
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QColor>

// Render-thread owned. All CPU arrays and QSG vertices allocated once at creation.
// A fixed clip contains a translating mesh: follow-live pan needs no vertex rewrite.
class TradeBubbleNode final : public QSGNode {
public:
    TradeBubbleNode();
    ~TradeBubbleNode() override;
    QSGGeometry* geometry() { return &geometry_; }
    const QSGGeometry* geometry() const { return &geometry_; }
    void clear();
    void setBottomInset(double inset) { bottomInset_ = inset; }
    QRectF clipRect() const { return clip_.clipRect(); }
    int usedVertexCount() const { return usedVertexCount_; }
    uint64_t rebuildCount() const { return rebuildCount_; }
    uint64_t dirtyMarks() const { return dirtyMarks_; } // scene graph changes this node made
    size_t lastScanRows() const { return lastScanRows_; }
    size_t rangeComparisons() const { return rangeComparisons_; }
    const QMatrix4x4& translation() const { return transform_.matrix(); }
    void sync(const trade_bubbles::Tape& tape, const TimeAxisMapping& mapping,
              bool enabled, const chart::bubbles::Options& options, const trade_bubbles::StyleModel& style = {});
private:
    trade_bubbles::Layout layout_;
    QSGGeometry geometry_, clipGeometry_;
    QSGClipNode clip_;
    QSGTransformNode transform_;
    QSGGeometryNode mesh_;
    trade_bubbles::WindowKey windowKey_;
    const trade_bubbles::Tape* tape_ = nullptr;
    uint64_t rebuildCount_ = 0, dirtyMarks_ = 0;
    size_t lastScanRows_ = 0, rangeComparisons_ = 0;
    TimeAxisMapping mapping_;
    chart::bubbles::Options options_;
    bool enabled_ = false;
    double bottomInset_ = 0;
    int usedVertexCount_ = 0;
    trade_bubbles::StyleModel style_;
};
