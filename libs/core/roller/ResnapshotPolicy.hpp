#pragma once
#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace sentinel::roller {
// When the serving roller asks capture for a resnapshot (FM-121). Pure and
// clock-injected; ShadowRoller.cpp's Resnapshots feeds it at the live tip. A
// request is due only when all hold:
// - the history book waits for a snapshot (any provenance; no history
//   recorder yet counts as a wait);
// - `grace` has passed since both the wait began and the newest connection
//   change seen in the history feed (TransportUp/Down, a new run or
//   connection id, a journal boundary): a new capture connection normally
//   starts with a snapshot, and a request could break it;
// - a record for the product arrived at the live tip within `liveWindow`: a
//   down or reconnecting product produces none, and capture's engine ignores
//   a resnapshot then;
// - the backoff deadline passed. Every request schedules the next one after
//   the current delay; a forwarded reply doubles the delay up to retryMax.
//   retryMin returns, with no pending deadline, once the book stayed valid
//   for `reset`.
class ResnapshotPolicy {
public:
  using Clock = std::chrono::steady_clock;
  struct Config {
    std::chrono::milliseconds retryMin, retryMax, reset, grace, liveWindow;
  };
  enum class Action { None, Request, Satisfied };
  explicit ResnapshotPolicy(Config c) : cfg(c), delay(c.retryMin) {}
  void connectionChanged(Clock::time_point now) { connectionChangedAt = now; }
  void frame(Clock::time_point now) { lastFrameAt = now; }
  Action poll(Clock::time_point now, bool wait) {
    if (!wait) {
      waitStart.reset();
      if (!validSince)
        validSince = now;
      if (now - *validSince >= cfg.reset) {
        delay = cfg.retryMin;
        nextAt.reset();
      }
      return std::exchange(open, false) ? Action::Satisfied : Action::None;
    }
    validSince.reset();
    open = true;
    if (!waitStart)
      waitStart = now;
    auto since = *waitStart;
    if (connectionChangedAt)
      since = std::max(since, *connectionChangedAt);
    if (now - since < cfg.grace)
      return Action::None;
    if (!lastFrameAt || now - *lastFrameAt > cfg.liveWindow)
      return Action::None;
    if (nextAt && now < *nextAt)
      return Action::None;
    nextAt = now + delay;
    return Action::Request;
  }
  void forwarded() { delay = std::min(cfg.retryMax, delay * 2); }
  // The delay that the last Request scheduled before the next one.
  std::chrono::milliseconds retryDelay() const { return delay; }
  bool waiting() const { return open; }

private:
  Config cfg;
  std::chrono::milliseconds delay;
  std::optional<Clock::time_point> nextAt, validSince, waitStart,
      connectionChangedAt, lastFrameAt;
  bool open = false;
};
} // namespace sentinel::roller
