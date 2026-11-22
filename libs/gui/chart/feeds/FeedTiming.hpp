#pragma once
#include "SentinelLogging.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace chart {
// Measurement only (flow.timing probe). Times one GUI-thread snapshot build and
// reports it only when the build's input signature differs from the last report,
// so a stationary view logs once. Inert when the probe is off.
class FeedTiming {
public:
    FeedTiming() : on_(sentinel::logging::probeEnabled("flow.timing")) {}
    bool on() const { return on_; }
    void start() { if (on_) timer_.start(); }
    // Build milliseconds when the signature changed since the last report, else negative.
    double finish(uint64_t signature) {
        if (!on_ || !timer_.isValid()) return -1;
        const double ms = double(timer_.nsecsElapsed()) / 1e6;
        timer_.invalidate();
        if (reported_ && signature == last_) return -1;
        reported_ = true;
        last_ = signature;
        return ms;
    }
    static uint64_t mix(uint64_t seed, int64_t value) {
        return (seed ^ uint64_t(value)) * 0x100000001b3ULL; // FNV-1a step
    }
    // The bucket index of a time, so a pan inside one bucket keeps the signature.
    static int64_t bucket(double ms, int64_t tf) {
        if (!std::isfinite(ms) || tf <= 0) return 0;
        return int64_t(std::clamp(std::floor(ms / double(tf)), -9e15, 9e15));
    }
private:
    bool on_ = false, reported_ = false;
    QElapsedTimer timer_;
    uint64_t last_ = 0;
};
} // namespace chart
