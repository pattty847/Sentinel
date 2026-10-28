#include "LevelTracker.hpp"

#include <algorithm>
#include <limits>

namespace sentinel::book {

double positiveMedian(std::span<double> values)
{
    const auto end = std::partition(values.begin(), values.end(),
                                    [](double v) { return std::isfinite(v) && v > 0; });
    const auto count = static_cast<std::size_t>(end - values.begin());
    if (count == 0)
        return 0;
    std::sort(values.begin(), end);
    const auto mid = count / 2;
    double value = values[mid];
    if (count % 2 == 0)
        value = values[mid - 1] + (value - values[mid - 1]) / 2;
    return value;
}

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

UpdateStatus LevelTracker::update(const Snapshot& s) { return update(s, nullptr); }

UpdateStatus LevelTracker::update(const Snapshot& s, const LargeReference& reference)
{
    return update(s, &reference);
}

UpdateStatus LevelTracker::update(const Snapshot& s, const LargeReference* reference)
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
    if (reference)
        medians_ = reference->medians;
    else
        computeMedians(s);
    trackLarge(s, reference ? reference->multiple : 4);
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
            && state.flickers < std::numeric_limits<std::uint64_t>::max()) {
            ++state.flickers;
            state.lastFlicker = now;
        }
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
        for (const auto& row : s.rows)
            sorted_.push_back(row.size(side));
        medians_[sideIndex(side)] = positiveMedian(sorted_);
    }
}

void LevelTracker::trackLarge(const Snapshot& s, double multiple)
{
    for (std::size_t row = 0; row < s.rows.size(); ++row)
        for (const auto side : sides)
            observeLarge(states_[row][sideIndex(side)], largeSize(s.rows[row].size(side), side, multiple),
                         s.timestampMs);
}

void LevelTracker::observeLarge(State& state, bool large, Milliseconds now) const
{
    if (large && !state.large) {
        if (!state.largeLapsed || now - state.largeEnded > config_.flickerWindowMs)
            state.largeSince = now;
        state.largeLapsed = false;
    } else if (!large && state.large) {
        state.largeEnded = now;
        state.largeLapsed = true;
    }
    state.large = large;
}

bool LevelTracker::largeSize(double size, Side side, double multiple) const
{
    const double reference = median(side);
    return std::isfinite(multiple) && multiple > 0 && validSize(size) && size > 0 && std::isfinite(reference)
        && reference > 0 && size / reference >= multiple;
}

LevelInfo LevelTracker::level(std::size_t row, Side side) const
{
    if (!window_.active || row >= window_.rows)
        return {};
    const auto& state = states_[row][sideIndex(side)];
    return {state.present, state.present ? window_.timestamp - state.since : 0, state.flickers,
            state.flickers > 0 ? window_.timestamp - state.lastFlicker : -1, state.large,
            state.large ? window_.timestamp - state.largeSince : 0};
}

bool LevelTracker::isLarge(std::size_t row, Side side, double multiple) const
{
    return window_.active && row < window_.rows && largeSize(sizes_[row].size(side), side, multiple);
}

} // namespace sentinel::book
