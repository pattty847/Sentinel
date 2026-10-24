#include "BookHistoryRing.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace sentinel::book {
namespace {
std::size_t cellCapacity(const HistoryConfig& config)
{
    const auto rows = detail::checkedCapacity(config.maxRows);
    const auto columns = detail::checkedCapacity(config.columns);
    if (rows > std::vector<LevelSizes>().max_size() / columns)
        throw std::invalid_argument("book history capacity overflows storage");
    return rows * columns;
}

double knownSize(double value)
{
    return validSize(value) ? value : std::numeric_limits<double>::quiet_NaN();
}
} // namespace

BookHistoryRing::BookHistoryRing(HistoryConfig config)
    : config_(config), columns_(detail::checkedCapacity(config.columns)), cells_(cellCapacity(config))
{
    detail::checkedInterval(config.sampleIntervalMs);
}

void BookHistoryRing::reset()
{
    window_ = {};
    next_ = count_ = 0;
}

UpdateStatus BookHistoryRing::update(const Snapshot& s)
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
    window_.accept(s, check);
    if (count_ > 0 && s.timestampMs - columns_[index(0)].timestamp < config_.sampleIntervalMs)
        return UpdateStatus::NotSampled;
    sample(s);
    return UpdateStatus::Accepted;
}

void BookHistoryRing::sample(const Snapshot& s)
{
    columns_[next_] = {window_.firstTick, s.rows.size(), s.timestampMs};
    for (std::size_t row = 0; row < s.rows.size(); ++row)
        cells_[next_ * config_.maxRows + row] = {knownSize(s.rows[row].bid), knownSize(s.rows[row].ask)};
    next_ = (next_ + 1) % columns_.size();
    count_ = std::min(count_ + 1, columns_.size());
}

std::size_t BookHistoryRing::index(std::size_t columnsAgo) const
{
    return (next_ + columns_.size() - 1 - columnsAgo) % columns_.size();
}

std::optional<Milliseconds> BookHistoryRing::timestamp(std::size_t columnsAgo) const
{
    if (columnsAgo >= count_)
        return std::nullopt;
    return columns_[index(columnsAgo)].timestamp;
}

std::optional<LevelSizes> BookHistoryRing::cell(std::size_t columnsAgo, std::size_t visibleRow) const
{
    if (columnsAgo >= count_ || visibleRow >= window_.rows)
        return std::nullopt;
    const auto slot = index(columnsAgo);
    const auto& column = columns_[slot];
    const auto row = window_.firstTick + static_cast<std::int64_t>(visibleRow) - column.firstTick;
    if (row < 0 || row >= static_cast<std::int64_t>(column.rows))
        return std::nullopt;
    return cells_[slot * config_.maxRows + static_cast<std::size_t>(row)];
}

} // namespace sentinel::book
