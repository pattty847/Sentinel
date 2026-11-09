#pragma once
#include "TimeAxisMapping.hpp"
#include "chart/core/BubblePlacement.hpp"
#include "marketdata/model/TradeData.h"
#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>

namespace trade_bubbles {
struct Sample {
    int64_t timeMs = 0;
    double price = 0, size = 0;
    AggressorSide side = AggressorSide::Unknown;
    std::string tradeId;
    int64_t exchangeNs = 0;
    uint64_t sequence = 0; // Assigned by Tape, never reused (including across clear).
};
// Two spans expose a wrapped, event-time-ordered ring without copying it.
struct Samples {
    std::span<const Sample> first, second;
    size_t size() const { return first.size() + second.size(); }
    bool empty() const { return size() == 0; }
    const Sample& operator[](size_t i) const { return i < first.size() ? first[i] : second[i-first.size()]; }
    const Sample& front() const { return (*this)[0]; }
    const Sample& back() const { return (*this)[size()-1]; }
};
struct WindowKey {
    uint64_t first = 0, last = 0, predecessor = 0;
    size_t count = 0;
    bool operator==(const WindowKey&) const = default;
};
struct Window {
    Samples rows;
    size_t comparisons = 0;
    uint64_t predecessor = 0;
    double previousPrice = 0;
    WindowKey key() const { return rows.empty() ? WindowKey{} : WindowKey{rows.front().sequence, rows.back().sequence, predecessor, rows.size()}; }
};
// GUI-thread ingestion; updatePaintNode reads while Qt blocks the GUI thread.
// A future paged history adapter can feed the same event-time samples and IDs.
class Tape {
public:
    static constexpr size_t Capacity = 100000;
    Tape() { ids_.reserve(Capacity); }
    bool append(Sample s);
    void clear();
    Samples samples() const { return slice(0, count_); }
    Window visible(double startMs, double endMs) const;
    const Sample* storage() const { return rows_.data(); }
    uint64_t revision() const { return revision_; }
private:
    Sample& at(size_t i) { return rows_[(head_ + i) % Capacity]; }
    const Sample& at(size_t i) const { return rows_[(head_ + i) % Capacity]; }
    Samples slice(size_t begin, size_t end) const;
    std::array<Sample, Capacity> rows_{};
    // IDs allocate only during ingestion, never during render; bounded by the ring.
    std::unordered_set<std::string> ids_;
    size_t head_ = 0, count_ = 0;
    uint64_t revision_ = 0;
};
// Converts tape storage to the Qt-free policy without copying strings or rows.
struct PolicyRows {
    Samples rows;
    size_t size() const { return rows.size(); }
    chart::bubbles::Execution operator[](size_t i) const {
        const auto& t = rows[i];
        return {double(t.timeMs), t.price, t.size, int(t.side), t.tradeId, t.exchangeNs};
    }
};
struct Bubble {
    float x = 0, y = 0, radius = 0;
    AggressorSide side = AggressorSide::Unknown;
    float minY = 0, maxY = 0, opacity = 1;
    double notional = 0;
};
class Layout {
public:
    static constexpr size_t MaxBubbles = 4096;
    static constexpr double MaxRadius = chart::bubbles::MaxRadius;
    Layout();
    static double radius(double notional, double reference) { return chart::bubbles::radius(notional, reference); }
    std::span<const Bubble> build(Samples trades, const TimeAxisMapping& map, const chart::bubbles::Options& options, double previousPrice = 0);
    size_t scannedRows() const { return scannedRows_; }
    size_t hashProbes() const { return hashProbes_; }
    size_t coarseCellVisits() const { return coarseCellVisits_; }
    size_t fineCellCount() const { return fineCellCount_; }
    double binSizePx() const { return binSize_; } // 0 means individual executions.
private:
    struct Point {
        double x = 0, y = 0, notional = 0;
        double minY = 0, maxY = 0, opacity = 1;
        bool aggregate = false;
        AggressorSide side = AggressorSide::Unknown;
        uint32_t cellX = 0, cellY = 0;
    };
    static constexpr size_t PointCapacity = chart::bubbles::BubblePlacement::MaxMarks;
    static constexpr size_t FineSlots = 1u << 18; // <=150k marks, load <0.58
    chart::bubbles::BubblePlacement policy_;
public:
    static constexpr size_t scratchBytes() { return PointCapacity * sizeof(Point) +
        FineSlots * sizeof(uint32_t) + MaxBubbles * sizeof(Point) +
        MaxBubbles * sizeof(Bubble) + MaxBubbles * 2 * sizeof(uint16_t) +
        chart::bubbles::BubblePlacement::storageBytes(); }
private:
    std::unique_ptr<Point[]> points_; // raw projected rows, compacted in place to 6 px cells
    std::unique_ptr<uint32_t[]> fineSlots_;
    std::array<uint16_t, MaxBubbles * 2> coarseSlots_{};
    std::array<Point, MaxBubbles> bins_{};
    std::array<Bubble, MaxBubbles> bubbles_{};
    size_t count_ = 0, scannedRows_ = 0;
    double binSize_ = 0;
    size_t hashProbes_ = 0, coarseCellVisits_ = 0, fineCellCount_ = 0;
};
} // namespace trade_bubbles
