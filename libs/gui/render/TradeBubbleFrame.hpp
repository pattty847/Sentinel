#pragma once
#include "TradeBubbleData.hpp"
#include <QColor>

namespace trade_bubbles {
// UGR publishes this value snapshot on the render thread. The sibling bubble
// item consumes it after ALL items synchronize, independent of QML sync order.
// Tape ingestion is on the GUI thread, which is still blocked at that point.
struct RenderFrame {
    explicit RenderFrame(std::shared_ptr<Tape> source) : tape(std::move(source)) {}
    std::shared_ptr<Tape> tape;
    TimeAxisMapping mapping;
    bool enabled = false;
    double minNotional = 0;
    QColor buy, sell;
};
}
