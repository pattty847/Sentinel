#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chart {
enum class CvdReset { None, UtcDay, TpoSession };
struct CvdPoint {
    int64_t timeMs = 0, resetMs = 0;
    double value = 0;
};
struct CvdScale {
    double min = 0, max = 0;
    double fraction(double value) const { return (value - min) / (max - min); }
};
// Feed all loaded bars in time order; retain only the visible window. Replaying
// the complete loaded series makes history paging and forming replacements exact.
class CvdSeries {
public:
    static constexpr int kCapacity = 8192;
    std::array<CvdPoint, kCapacity> points{};
    int count = 0;
    static int64_t resetStart(int64_t time, CvdReset rule, int64_t tpoStart) {
        if (rule == CvdReset::None) return 0;
        if (rule == CvdReset::TpoSession) return tpoStart;
        constexpr int64_t day = 86400000;
        const auto remainder = time % day;
        return time - remainder - (remainder < 0 ? day : 0);
    }
    void clear() { count = 0; started_ = false; total_ = prefix_ = 0; }
    bool push(int64_t time, double delta, int64_t reset, bool visible) {
        if (!std::isfinite(delta) || (started_ && time < lastTime_)) return false;
        const bool replace = started_ && time == lastTime_;
        if (!replace) prefix_ = !started_ || reset != lastReset_ ? 0 : total_;
        const double next = prefix_ + delta;
        if (!std::isfinite(next)) return false;
        total_ = next; lastTime_ = time; lastReset_ = reset; started_ = true;
        if (count && points[size_t(count - 1)].timeMs == time) --count;
        if (visible) {
            if (count == kCapacity) return false;
            points[size_t(count++)] = {time, reset, total_};
        }
        return true;
    }
    CvdScale scale(double from = -std::numeric_limits<double>::infinity(),
                   double to = std::numeric_limits<double>::infinity(), int64_t duration = 0) const {
        CvdScale out;
        for (int i = 0; i < count; ++i) {
            const auto& point = points[size_t(i)];
            if (double(point.timeMs) >= to || double(point.timeMs) + duration <= from) continue;
            out.min = std::min(out.min, point.value);
            out.max = std::max(out.max, point.value);
        }
        if (out.min == out.max) { out.min = -1; out.max = 1; }
        return out;
    }
private:
    bool started_ = false;
    int64_t lastTime_ = 0, lastReset_ = 0;
    double total_ = 0, prefix_ = 0;
};
} // namespace chart
