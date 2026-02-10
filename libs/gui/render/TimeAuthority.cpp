#include "TimeAuthority.hpp"

#include <algorithm>

void TimeAuthority::setActiveTimeframeMs(int64_t timeframeMs) {
    if (timeframeMs <= 0) {
        return;
    }
    m_activeTimeframeMs.store(timeframeMs, std::memory_order_release);
}

int64_t TimeAuthority::activeTimeframeMs() const {
    return m_activeTimeframeMs.load(std::memory_order_acquire);
}

void TimeAuthority::observeEventTime(int64_t eventTimeMs, qint64 steadyNowMs) {
    if (eventTimeMs <= 0) {
        return;
    }
    m_lastEventTimeMs.store(eventTimeMs, std::memory_order_release);
    m_wallTimeAtLastEventMs.store(static_cast<int64_t>(std::max<qint64>(0, steadyNowMs)),
                                  std::memory_order_release);
    m_hasEvent.store(true, std::memory_order_release);
}

TimeAuthority::Snapshot TimeAuthority::snapshot(qint64 steadyNowMs) const {
    Snapshot out;
    out.activeTimeframeMs = activeTimeframeMs();
    out.hasEvent = m_hasEvent.load(std::memory_order_acquire);
    if (!out.hasEvent) {
        out.nowPresentationMs = static_cast<int64_t>(std::max<qint64>(0, steadyNowMs));
        return out;
    }

    out.lastEventTimeMs = m_lastEventTimeMs.load(std::memory_order_acquire);
    out.nowEventMs = out.lastEventTimeMs;
    const int64_t wallAtEvent = m_wallTimeAtLastEventMs.load(std::memory_order_acquire);
    const int64_t steadyNowSafe = static_cast<int64_t>(std::max<qint64>(0, steadyNowMs));
    const int64_t delta = std::max<int64_t>(0, steadyNowSafe - wallAtEvent);
    out.nowPresentationMs = out.lastEventTimeMs + delta;
    return out;
}
