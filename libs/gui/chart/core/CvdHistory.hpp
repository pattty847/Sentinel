#pragma once
#include "CvdSeries.hpp"
#include "../../../core/servermodel/SessionManager.hpp"

namespace chart {
inline int64_t cvdResetBoundary(int64_t time, CvdReset reset, int sessionType) {
    const auto session = SessionManager::sessionContaining(time,
        static_cast<SessionManager::SessionType>(std::clamp(sessionType, 0, 6)));
    return CvdSeries::resetStart(time, reset, session.startMs);
}
// An optional earlier retention/request anchor. The source still applies its
// ordinary viewport margin and existing 8192-bucket cap; off/visible leaves it alone.
inline int64_t cvdHistoryAnchor(int64_t visibleStart, int64_t timeframe, int stripPx,
                               CvdReset reset, int sessionType) {
    if (stripPx <= 0 || reset == CvdReset::Visible || visibleStart < 0 || timeframe <= 0) return -1;
    const auto tf = std::max<int64_t>(1000, timeframe);
    return std::max<int64_t>(0, cvdResetBoundary(visibleStart / tf * tf, reset, sessionType));
}
} // namespace chart
