#pragma once
// Temporary one-second FHE1 replay only. One bounded flight, newest missing
// seconds first, no seal probes or minute-history machinery. Times are injected.
#include "heatmap/ChartSurface.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <cstdint>

namespace sentinel::flow {
class FlowSecondHistory {
public:
    struct Request { uint64_t id; int64_t fromMs, toMs; };
    static constexpr int64_t TimeoutMs = 30000;
    void reset() { flight_.reset(); timedOut_ = false; due_.clear(); from_ = to_ = 0; retryAt_ = 0; backoff_ = 100; limit_ = 512; }
    void window(int64_t from, int64_t to, int64_t anchor) {
        if (from < 0 || to <= from || to > INT64_MAX-1000) return;
        to = (to-1)/1000*1000+1000;
        from = std::max<int64_t>(0, std::min(from, anchor >= 0 ? anchor : from)/1000*1000);
        from = std::max(from, to-int64_t(heatmap::kMaxChartSurfaceBars)*1000);
        if (from != from_ || to != to_) retryAt_ = 0;
        from_ = from; to_ = to;
        std::erase_if(due_, [&](const auto& p) { return p.first < from_ || p.first >= to_; });
    }
    std::optional<Request> next(int64_t now) {
        if (flight_ && !timedOut_) {
            if (now < deadline_) return {};
            // Keep the original id during the retry delay: a cold replay that
            // finishes just after its deadline still applies once.
            timedOut_ = true; retryAt_ = now+backoff_; backoff_ = std::min(5000,backoff_*2);
            return {};
        }
        if (now < retryAt_) return {};
        flight_.reset();
        const auto top = std::min(to_, now/1000*1000); // never include the live bucket
        auto end = top;
        while (end > from_ && covered(end-1000, now)) end -= 1000;
        if (end <= from_) return {};
        auto begin = end-1000;
        while (begin > from_ && end-begin < limit_*1000 && !covered(begin-1000, now)) begin -= 1000;
        flight_ = Request{++serial_, begin, end}; timedOut_ = false; deadline_ = now+TimeoutMs;
        return flight_;
    }
    bool matches(uint64_t id) const { return flight_ && flight_->id == id; }
    const std::optional<Request>& flight() const { return flight_; }
    void accepted(int64_t now, bool building) {
        if (!flight_) return;
        for (auto t=std::max(from_,flight_->fromMs); t<std::min(to_,flight_->toMs); t+=1000)
            due_[t] = building ? now+1000 : INT64_MAX;
        flight_.reset(); timedOut_ = false; backoff_ = 100; retryAt_ = 0;
    }
    void failed(int64_t now, bool oversized = false, bool fatal = false) {
        flight_.reset(); timedOut_ = false;
        if (oversized) limit_ = std::max(1, limit_/2);
        retryAt_ = fatal ? INT64_MAX : now+backoff_;
        backoff_ = std::min(5000, backoff_*2);
    }
    int64_t wakeAt(int64_t now) const {
        if (flight_) return timedOut_ ? retryAt_ : deadline_;
        if (retryAt_ > now) return retryAt_;
        int64_t wake = INT64_MAX;
        for (const auto& [time,due] : due_) if (due > now) wake = std::min(wake,due);
        return wake;
    }
    int64_t from() const { return from_; }
    int64_t to() const { return to_; }
private:
    bool covered(int64_t time, int64_t now) const {
        const auto it=due_.find(time); return it!=due_.end() && it->second>now;
    }
    bool timedOut_ = false;
    uint64_t serial_ = 0;
    int64_t from_ = 0, to_ = 0, deadline_ = 0, retryAt_ = 0;
    int backoff_ = 100, limit_ = 512;
    std::optional<Request> flight_;
    std::map<int64_t,int64_t> due_;
};
}
