#pragma once
#include "TimeAxisMapping.hpp"
#include "marketdata/model/TradeData.h"
#include <array>
#include <span>

namespace trade_bubbles {
struct Sample {
    int64_t timeMs = 0;
    double price = 0, size = 0;
    AggressorSide side = AggressorSide::Unknown;
};
// GUI-thread ingestion; updatePaintNode reads while Qt blocks the GUI thread.
// A future paged history adapter can feed the same event-time samples.
class Tape {
public:
    static constexpr size_t Capacity = 100000;
    bool append(Sample s);
    void clear() { next_ = count_ = 0; ++revision_; }
    std::span<const Sample> samples() const { return {rows_.data(), count_}; }
    uint64_t revision() const { return revision_; }
private:
    std::array<Sample, Capacity> rows_{};
    size_t next_ = 0, count_ = 0;
    uint64_t revision_ = 0;
};
struct Bubble { float x = 0, y = 0, radius = 0; AggressorSide side = AggressorSide::Unknown; };
class Layout {
public:
    static constexpr size_t MaxBubbles = 4096;
    static constexpr double MaxRadius = 18;
    // Area proportional to quote notional, 3 px radius at 1,000 quote units.
    static double radius(double notional);
    std::span<const Bubble> build(std::span<const Sample> trades, const TimeAxisMapping& map, double minNotional);
private:
    struct Bin { double x = 0, y = 0, notional = 0; };
    std::array<Bin, MaxBubbles> bins_{};
    std::array<Bubble, MaxBubbles> bubbles_{};
    size_t count_ = 0;
};
} // namespace trade_bubbles
