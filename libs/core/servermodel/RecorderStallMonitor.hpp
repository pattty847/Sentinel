#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recording {
// Main-thread check that every recorded (symbol, layer) series keeps committing
// minute columns while that symbol's own market-data connection is up. A series is due when
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
    void setConnected(const std::string& symbol, bool connected, int64_t nowMs) {
        auto& state = connections_[symbol];
        if (connected && !state.up) state.minute = floorMinute(nowMs);
        state.up = connected;
    }
    std::vector<Stall> check(int64_t nowMs, const std::vector<Series> &series) {
        std::vector<Stall> out;
        for (const auto &s : series) {
            const auto connection = connections_.find(s.symbol);
            if (connection == connections_.end() || !connection->second.up) continue;
            auto &warned = lastWarnMs_[{s.symbol, s.layer}];
            const int64_t due = dueMs(connection->second.minute, s.lastColumnMs);
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

    // How far past due the symbol's series' next column is (>= 0), or nullopt
    // while that symbol's own connection is down (or never reported). Stalled
    // (and warned by check) once this reaches graceMs.
    std::optional<int64_t> overdueMs(const std::string &symbol, int64_t nowMs, int64_t lastColumnMs) const {
        const auto connection = connections_.find(symbol);
        if (connection == connections_.end() || !connection->second.up)
            return std::nullopt;
        return std::max<int64_t>(0, nowMs - dueMs(connection->second.minute, lastColumnMs));
    }

  private:
    int64_t dueMs(int64_t connectMinute, int64_t lastColumnMs) const {
        return std::max(connectMinute, floorMinute(lastColumnMs)) + 2 * kMinute + lateness_;
    }
    static constexpr int64_t kNever = INT64_MIN;
    static int64_t floorMinute(int64_t ms) { return ms / kMinute * kMinute - (ms % kMinute < 0 ? kMinute : 0); }
    int64_t lateness_, grace_, repeat_;
    struct Connection { bool up = false; int64_t minute = 0; };
    std::map<std::string, Connection> connections_;
    std::map<std::pair<std::string, std::string>, int64_t> lastWarnMs_;
};
} // namespace recording
