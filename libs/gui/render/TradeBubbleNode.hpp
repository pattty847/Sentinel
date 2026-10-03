#pragma once
#include "TradeBubbleData.hpp"
#include <QSGGeometryNode>
#include <QColor>

// Render-thread owned. All CPU arrays and QSG vertices allocated once at creation.
class TradeBubbleNode final : public QSGGeometryNode {
public:
    TradeBubbleNode();
    int usedVertexCount() const { return usedVertexCount_; }
    void sync(const trade_bubbles::Tape& tape, const TimeAxisMapping& mapping,
              bool enabled, double minNotional, QColor buy, QColor sell);
private:
    trade_bubbles::Layout layout_;
    QSGGeometry geometry_;
    uint64_t revision_ = ~uint64_t(0);
    TimeAxisMapping mapping_;
    double minNotional_ = -1;
    bool enabled_ = false;
    int usedVertexCount_ = 0;
    QColor buy_, sell_;
};
