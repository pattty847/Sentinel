#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sentinel::book {

// These bounded analytics objects are not thread-safe. The GUI thread owns them;
// no Qt, callbacks, signals or book-replica locks are used here. Allocate capacity
// at construction, then reuse input/output buffers. Views expire on next update.
using Milliseconds = std::int64_t;
enum class Side : std::uint8_t { Bid, Ask };
enum class Aggressor : std::uint8_t { Buy, Sell, Unknown };
enum class UpdateStatus { Accepted, Reset, NotSampled, InvalidInput, CapacityExceeded };

struct LevelSizes {
    double bid = 0;
    double ask = 0;
    double size(Side side) const { return side == Side::Bid ? bid : ask; }
};

// Rows ascend: price(row) = firstPrice + row * tickSize. firstPrice must be
// tick-aligned. symbolId is a caller-owned stable identity, changed on a symbol
// switch (or feed epoch). timestampMs is nonnegative monotonic receive time, not
// exchange UTC, and must strictly increase within a symbol/tick series. Rejected
// updates preserve persistent state; callers must handle status, not discard it.
// An empty rows span is withdrawal, even if other fields are unset. It resets all
// state. Nonempty all-zero rows are real observations, not withdrawal.
// Invalid individual sizes are unknown observations, never interpreted as zero.
struct Snapshot {
    std::uint64_t symbolId = 0;
    double tickSize = 0;
    double firstPrice = 0;
    std::span<const LevelSizes> rows;
    Milliseconds timestampMs = 0;
};

struct Trade {
    double price = 0;
    double size = 0;
    Aggressor aggressor = Aggressor::Unknown;
    Milliseconds timestampMs = 0;
};

inline bool validSize(double size) { return std::isfinite(size) && size >= 0; }
inline constexpr std::array<Side, 2> sides{Side::Bid, Side::Ask};
inline constexpr std::size_t sideIndex(Side side) { return static_cast<std::size_t>(side); }

} // namespace sentinel::book
