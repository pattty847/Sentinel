// Sentinel - TimeAuthority for frame cadence and presentation time snapshots.
#pragma once

#include <QtGlobal>
#include <atomic>
#include <cstdint>

class TimeAuthority {
public:
    struct Snapshot {
        int64_t activeTimeframeMs = 0;
        int64_t lastEventTimeMs = 0;
        int64_t nowEventMs = 0;
        int64_t nowPresentationMs = 0;
        bool hasEvent = false;
    };

    void setActiveTimeframeMs(int64_t timeframeMs);
    int64_t activeTimeframeMs() const;

    void observeEventTime(int64_t eventTimeMs, qint64 steadyNowMs);
    Snapshot snapshot(qint64 steadyNowMs) const;

private:
    std::atomic<int64_t> m_activeTimeframeMs{0};
    std::atomic<int64_t> m_lastEventTimeMs{0};
    std::atomic<int64_t> m_wallTimeAtLastEventMs{0};
    std::atomic<bool> m_hasEvent{false};
};
