#pragma once
#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace sentinel::roller {
// When the serving roller asks capture for a resnapshot (FM-121). Pure and
// clock-injected; ShadowRoller.cpp's Resnapshots polls it at the live tip with
// the history recorder's state. No requests while the journal transport is
// down: capture is reconnecting and its new connection starts with a snapshot.
// - Recorder: the recorder invalidated the book itself (or imported it
//   invalid); no snapshot comes unless asked. Due at once.
// - Upstream: a transport/journal-boundary invalidation, or no recorder yet
//   (no usable snapshot since the replay start). A reconnect normally brings a
//   snapshot, so it is due only after upstreamGrace with the transport up
//   (restarted on every transport change). Journal-local loss (a damaged block)
//   and a one-sided first snapshot end here.
// Every request schedules the next one after the current delay; a forwarded
// reply doubles the delay up to retryMax. retryMin returns, with no pending
// deadline, once the book stayed valid for `reset`.
class ResnapshotPolicy {
public:
  using Clock = std::chrono::steady_clock;
  struct Config {
    std::chrono::milliseconds retryMin, retryMax, reset, upstreamGrace;
  };
  enum class Wait { None, Recorder, Upstream };
  enum class Action { None, Request, Satisfied };
  explicit ResnapshotPolicy(Config c) : cfg(c), delay(c.retryMin) {}
  Action poll(Clock::time_point now, Wait wait, bool transportUp) {
    if (wait == Wait::None) {
      upstreamSince.reset();
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
    if (!transportUp) {
      upstreamSince.reset();
      return Action::None;
    }
    if (wait == Wait::Upstream) {
      if (!upstreamSince)
        upstreamSince = now;
      if (now - *upstreamSince < cfg.upstreamGrace)
        return Action::None;
    }
    if (nextAt && now < *nextAt)
      return Action::None;
    nextAt = now + delay;
    return Action::Request;
  }
  void transportChanged() { upstreamSince.reset(); }
  void forwarded() { delay = std::min(cfg.retryMax, delay * 2); }
  // The delay that the last Request scheduled before the next one.
  std::chrono::milliseconds retryDelay() const { return delay; }
  bool waiting() const { return open; }

private:
  Config cfg;
  std::chrono::milliseconds delay;
  std::optional<Clock::time_point> nextAt, validSince, upstreamSince;
  bool open = false;
};
} // namespace sentinel::roller
