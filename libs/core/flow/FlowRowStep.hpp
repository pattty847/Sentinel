#pragma once
// Power-of-two row steps preserve exact aggregation and bound offsets to u16.
#include "servermodel/RecordingCodec.hpp"
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <limits>
namespace sentinel::flow::chunks {
// Section 4 row step: a bar spans at most 65,535 rows; steps are 2^0..2^48.
constexpr int64_t MaxOffset = 65'534;
constexpr unsigned MaxStepExponent = 48;
inline bool validStep(uint64_t step) {
    return step && !(step & (step - 1)) && step <= (uint64_t(1) << MaxStepExponent);
}
// A FlowBar tick must represent a positive integer in its price scale. At large
// power-of-two steps, multiplication by the scale can lose a few low bits; use
// its double spacing while still refusing a value halfway between grid units.
inline int64_t tickUnits(double tick, int64_t priceScale) {
    const auto units = recording::priceUnits(tick, double(priceScale));
    const double scaled = tick * double(priceScale);
    const double ulp = std::nextafter(scaled, std::numeric_limits<double>::infinity()) - scaled;
    const double tolerance = std::min(0.499, std::max(1e-6, 2 * ulp));
    if (!units || *units <= 0 || priceScale <= 0 || std::abs(double(*units) - scaled) > tolerance)
        throw std::invalid_argument("unrepresentable flow row tick");
    return *units;
}
// Row of a price at a step: floorDiv(native row, step).
inline int64_t rowAt(int64_t priceUnits, int64_t rowTickUnits, uint64_t step) {
    return recording::floorDiv(recording::floorDiv(priceUnits, rowTickUnits), int64_t(step));
}

// The smallest step, at least minStep, at which a bar spans at most 65,535 rows.
inline uint64_t stepFor(int64_t lowUnits, int64_t highUnits, int64_t rowTickUnits, uint64_t minStep = 1) {
    if (!validStep(minStep) || lowUnits <= 0 || highUnits < lowUnits || rowTickUnits <= 0)
        throw std::invalid_argument("flow chunk step input");
    for (uint64_t s = minStep; s <= (uint64_t(1) << MaxStepExponent); s *= 2)
        if (rowAt(highUnits, rowTickUnits, s) - rowAt(lowUnits, rowTickUnits, s) <= MaxOffset) return s;
    throw std::logic_error("flow chunk no step fits"); // unreachable: 2^48 fits any i64 range
}

}
