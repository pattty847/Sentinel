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
    QSGGeometry* geometry() { return &geometry_; }
    const QSGGeometry* geometry() const { return &geometry_; }
    int usedVertexCount() const { return usedVertexCount_; }
    uint64_t rebuildCount() const { return rebuildCount_; }
    size_t lastScanRows() const { return lastScanRows_; }
    size_t rangeComparisons() const { return rangeComparisons_; }
    const QMatrix4x4& translation() const { return transform_.matrix(); }
    void sync(const trade_bubbles::Tape& tape, const TimeAxisMapping& mapping,
              bool enabled, double minNotional, QColor buy, QColor sell);
private:
    trade_bubbles::Layout layout_;
    QSGGeometry geometry_, clipGeometry_;
    QSGClipNode clip_;
    QSGTransformNode transform_;
    QSGGeometryNode mesh_;
    trade_bubbles::WindowKey windowKey_;
    uint64_t rebuildCount_ = 0;
    size_t lastScanRows_ = 0, rangeComparisons_ = 0;
    TimeAxisMapping mapping_;
    double minNotional_ = -1;
    bool enabled_ = false;
    int usedVertexCount_ = 0;
    QColor buy_, sell_;
};
