#include "WindowState.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace sentinel::book::detail {

std::optional<std::int64_t> priceTick(double price, double tick, bool requireAligned)
{
    if (!validSize(price) || !std::isfinite(tick) || tick <= 0)
        return std::nullopt;
    const double scaled = price / tick;
    // Keep exact integer additions and distinguish adjacent rows in binary64.
    if (!std::isfinite(scaled) || scaled >= 0x1p50)
        return std::nullopt;
    const double nearest = std::round(scaled);
    const double tolerance = std::min(0.125, std::max(1e-8,
        4 * std::numeric_limits<double>::epsilon() * scaled));
    const bool aligned = std::abs(scaled - nearest) <= tolerance;
    if (requireAligned && !aligned)
        return std::nullopt;
    return static_cast<std::int64_t>(aligned ? nearest : std::floor(scaled));
}

WindowCheck WindowState::inspect(const Snapshot& s, std::size_t capacity) const
{
    if (s.rows.empty())
        return {UpdateStatus::Reset};
    if (s.rows.size() > capacity)
        return {UpdateStatus::CapacityExceeded};
    const auto first = priceTick(s.firstPrice, s.tickSize, true);
    if (!first || s.timestampMs < 0)
        return {};
    const double last = s.firstPrice + static_cast<double>(s.rows.size() - 1) * s.tickSize;
    if (!priceTick(last, s.tickSize, true))
        return {};
    const bool changed = !active || symbol != s.symbolId || tick != s.tickSize;
    if (!changed && s.timestampMs <= timestamp)
        return {};
    return {UpdateStatus::Accepted, *first, changed};
}

void WindowState::accept(const Snapshot& s, const WindowCheck& check)
{
    active = true;
    symbol = s.symbolId;
    tick = s.tickSize;
    firstTick = check.firstTick;
    rows = s.rows.size();
    timestamp = s.timestampMs;
}

std::optional<std::size_t> WindowState::rowAt(std::int64_t price) const
{
    if (!active || price < firstTick || price - firstTick >= static_cast<std::int64_t>(rows))
        return std::nullopt;
    return static_cast<std::size_t>(price - firstTick);
}

std::size_t checkedCapacity(std::size_t capacity)
{
    if (capacity == 0 || capacity > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("book analytics capacity must be in [1, INT_MAX]");
    return capacity;
}

Milliseconds checkedInterval(Milliseconds interval)
{
    if (interval <= 0)
        throw std::invalid_argument("book analytics interval must be positive");
    return interval;
}

double checkedThreshold(double threshold)
{
    if (!validSize(threshold))
        throw std::invalid_argument("book analytics threshold must be finite and nonnegative");
    return threshold;
}

} // namespace sentinel::book::detail
