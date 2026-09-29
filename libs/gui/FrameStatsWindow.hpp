#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fixed storage for render-thread frame durations. Callers provide monotonic timestamps.
class FrameStatsWindow {
public:
    static constexpr std::size_t Capacity = 512;
    static constexpr int64_t WindowMs = 1000;

    struct Summary {
        int samples = 0;
        double p50Ms = 0.0;
        double p95Ms = 0.0;
        double renderRateHz = 0.0;
    };

    void clear() { m_count = 0; m_next = 0; }

    void add(int64_t timestampMs, double durationMs) {
        if (!std::isfinite(durationMs) || durationMs < 0.0) return;
        m_samples[m_next] = {timestampMs, durationMs};
        m_next = (m_next + 1) % Capacity;
        if (m_count < Capacity) ++m_count;
    }

    Summary summarize(int64_t nowMs) const {
        std::array<double, Capacity> durations{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < m_count; ++i) {
            const auto& sample = m_samples[i];
            if (sample.timestampMs <= nowMs && nowMs - sample.timestampMs < WindowMs)
                durations[count++] = sample.durationMs;
        }
        if (!count) return {};
        std::sort(durations.begin(), durations.begin() + count);
        // Nearest-rank percentiles; the window contains at most one second of frames.
        const auto p50Rank = (count + 1) / 2 - 1;
        const auto p95Rank = (95 * count + 99) / 100 - 1;
        return {static_cast<int>(count), durations[p50Rank], durations[p95Rank],
                static_cast<double>(count)};
    }

private:
    struct Sample { int64_t timestampMs = 0; double durationMs = 0.0; };
    std::array<Sample, Capacity> m_samples{};
    std::size_t m_count = 0;
    std::size_t m_next = 0;
};
