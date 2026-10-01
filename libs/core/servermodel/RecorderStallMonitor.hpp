#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace recording {
// Main-thread check that every recorded (symbol, layer) series keeps committing
// minute columns while the market-data connection is up. A series is due when
// its next column must have committed: one minute after the bucket that follows
// max(last column bucket, connect minute), plus lateness. The connect-minute term
// allows a snapshot that lands just after the boundary and still catches a missing
// first snapshot. It is stalled graceMs after that, and warned once per repeatMs.
class RecorderStallMonitor {
  public:
    static constexpr int64_t kMinute = 60'000;
    struct Series {
        std::string symbol, layer;
        int64_t lastColumnMs = 0; // 0 = no column yet
    };
    struct Stall {
        std::string symbol, layer;
        int64_t lastColumnMs = 0, overdueMs = 0;
    };
    explicit RecorderStallMonitor(int64_t latenessMs, int64_t graceMs = kMinute, int64_t repeatMs = kMinute)
        : lateness_(latenessMs), grace_(graceMs), repeat_(repeatMs) {}
    void setConnected(bool connected, int64_t nowMs) {
        if (connected && !connected_)
            connectMinute_ = floorMinute(nowMs);
        connected_ = connected;
    }
    std::vector<Stall> check(int64_t nowMs, const std::vector<Series> &series) {
        std::vector<Stall> out;
        if (!connected_)
            return out;
        for (const auto &s : series) {
            auto &warned = lastWarnMs_[{s.symbol, s.layer}];
            const int64_t due = std::max(connectMinute_, floorMinute(s.lastColumnMs)) + 2 * kMinute + lateness_;
            if (nowMs - due < grace_) {
                warned = kNever;
                continue;
            }
            if (warned != kNever && nowMs - warned < repeat_)
                continue;
            warned = nowMs;
            out.push_back({s.symbol, s.layer, s.lastColumnMs, nowMs - due});
        }
        return out;
    }

  private:
    static constexpr int64_t kNever = INT64_MIN;
    static int64_t floorMinute(int64_t ms) { return ms / kMinute * kMinute - (ms % kMinute < 0 ? kMinute : 0); }
    int64_t lateness_, grace_, repeat_;
    bool connected_ = false;
    int64_t connectMinute_ = 0;
    std::map<std::pair<std::string, std::string>, int64_t> lastWarnMs_;
};
} // namespace recording
