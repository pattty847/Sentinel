#pragma once

#include "BookAnalyticsTypes.hpp"
#include <optional>

namespace sentinel::book::detail {

struct WindowCheck {
    UpdateStatus status = UpdateStatus::InvalidInput;
    std::int64_t firstTick = 0;
    bool newSeries = false;
};

// Integer price keys avoid decimal-tick equality and row-index matching errors.
struct WindowState {
    bool active = false;
    std::uint64_t symbol = 0;
    double tick = 0;
    std::int64_t firstTick = 0;
    std::size_t rows = 0;
    Milliseconds timestamp = 0;

    WindowCheck inspect(const Snapshot& snapshot, std::size_t capacity) const;
    void accept(const Snapshot& snapshot, const WindowCheck& check);
    std::optional<std::size_t> rowAt(std::int64_t priceTick) const;
};

// Raw trade prices use the containing [tick, tick+1) bucket, like the live book.
std::optional<std::int64_t> priceTick(double price, double tick, bool requireAligned);
std::size_t checkedCapacity(std::size_t capacity);
Milliseconds checkedInterval(Milliseconds interval);
double checkedThreshold(double threshold);

} // namespace sentinel::book::detail
