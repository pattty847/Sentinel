#include "LevelTracker.hpp"

#include <algorithm>
#include <limits>

namespace sentinel::book {

LevelTracker::LevelTracker(TrackerConfig config)
    : config_(config), states_(detail::checkedCapacity(config.maxRows)),
      scratch_(config.maxRows), sizes_(config.maxRows)
{
    detail::checkedThreshold(config.minimumSize);
    detail::checkedInterval(config.flickerWindowMs);
    sorted_.reserve(config.maxRows);
}

void LevelTracker::reset()
{
    window_ = {};
    medians_ = {};
}

UpdateStatus LevelTracker::update(const Snapshot& s)
{
    const auto check = window_.inspect(s, config_.maxRows);
    if (check.status == UpdateStatus::Reset) {
        reset();
        return check.status;
    }
    if (check.status != UpdateStatus::Accepted)
        return check.status;
    if (check.newSeries)
        reset();
    trackRows(s, check.firstTick);
    computeMedians(s);
    std::copy(s.rows.begin(), s.rows.end(), sizes_.begin());
    window_.accept(s, check);
    return UpdateStatus::Accepted;
}

void LevelTracker::trackRows(const Snapshot& s, std::int64_t firstTick)
{
    for (std::size_t row = 0; row < s.rows.size(); ++row) {
        const auto previous = window_.rowAt(firstTick + static_cast<std::int64_t>(row));
        scratch_[row] = previous ? states_[*previous] : RowState{};
        for (const auto side : sides)
            observe(scratch_[row][sideIndex(side)], s.rows[row].size(side), s.timestampMs);
    }
    states_.swap(scratch_);
}

void LevelTracker::observe(State& state, double size, Milliseconds now)
{
    if (!validSize(size)) {
        state = {};
        return;
    }
    const bool present = size > 0 && size >= config_.minimumSize;
    if (present && !state.present) {
        if (state.vanished && now - state.disappeared <= config_.flickerWindowMs
            && state.flickers < std::numeric_limits<std::uint64_t>::max())
            ++state.flickers;
        state.since = now;
        state.vanished = false;
    } else if (!present && state.present) {
        state.disappeared = now;
        state.vanished = true;
    }
    state.present = present;
}

void LevelTracker::computeMedians(const Snapshot& s)
{
    for (const auto side : sides) {
        sorted_.clear();
        for (const auto& row : s.rows) {
            const double size = row.size(side);
            if (validSize(size) && size > 0)
                sorted_.push_back(size);
        }
        std::sort(sorted_.begin(), sorted_.end());
        double value = 0;
        if (!sorted_.empty()) {
            const auto mid = sorted_.size() / 2;
            value = sorted_[mid];
            if (sorted_.size() % 2 == 0)
                value = sorted_[mid - 1] + (value - sorted_[mid - 1]) / 2;
        }
        medians_[sideIndex(side)] = value;
    }
}

LevelInfo LevelTracker::level(std::size_t row, Side side) const
{
    if (!window_.active || row >= window_.rows)
        return {};
    const auto& state = states_[row][sideIndex(side)];
    return {state.present, state.present ? window_.timestamp - state.since : 0, state.flickers};
}

bool LevelTracker::isLarge(std::size_t row, Side side, double multiple) const
{
    if (!window_.active || row >= window_.rows || !std::isfinite(multiple) || multiple <= 0)
        return false;
    const double size = sizes_[row].size(side);
    const double reference = median(side);
    return validSize(size) && size > 0 && reference > 0 && size / reference >= multiple;
}

} // namespace sentinel::book
