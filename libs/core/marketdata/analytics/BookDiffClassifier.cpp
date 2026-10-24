#include "BookDiffClassifier.hpp"

#include <algorithm>
#include <limits>

namespace sentinel::book {
namespace {
void addFinite(double& total, double amount)
{
    total = std::min(total, std::numeric_limits<double>::max() - amount) + amount;
}

void includePull(RollingTotals& totals, const BookEvent& event)
{
    if (!totals.largestPull || event.size > totals.largestPull->size)
        totals.largestPull = event;
    auto& side = totals.side[sideIndex(event.side)];
    side.largestPull = std::max(side.largestPull, event.size);
}
} // namespace

BookDiffClassifier::BookDiffClassifier(DiffConfig config)
    : config_(config), previous_(detail::checkedCapacity(config.maxRows)),
      traded_(config.maxRows), steps_(detail::checkedCapacity(config.maxSteps))
{
    detail::checkedInterval(config.windowMs);
    detail::checkedThreshold(config.minimumSize);
    events_.reserve(config.maxRows * 4); // Up to eat + pull per side per row.
}

void BookDiffClassifier::reset()
{
    window_ = {};
    head_ = count_ = 0;
    events_.clear();
    totals_ = {};
}

UpdateStatus BookDiffClassifier::update(const Snapshot& s, std::span<const Trade> trades)
{
    events_.clear();
    const auto check = window_.inspect(s, config_.maxRows);
    if (check.status == UpdateStatus::Reset) {
        reset();
        return check.status;
    }
    if (check.status != UpdateStatus::Accepted)
        return check.status;
    if (check.newSeries)
        reset();
    if (window_.active) {
        matchTrades(s, trades);
        classify(s, check.firstTick);
    }
    // Check space before changing any persistent state, including expiration.
    if (!events_.empty() && count_ == steps_.size()
        && s.timestampMs - steps_[head_].timestamp < config_.windowMs) {
        events_.clear();
        return UpdateStatus::CapacityExceeded;
    }
    expire(s.timestampMs);
    if (!events_.empty()) {
        steps_[(head_ + count_) % steps_.size()] = summarize(s.timestampMs);
        ++count_;
    }
    refreshTotals();
    std::copy(s.rows.begin(), s.rows.end(), previous_.begin());
    window_.accept(s, check);
    return UpdateStatus::Accepted;
}

void BookDiffClassifier::matchTrades(const Snapshot& s, std::span<const Trade> trades)
{
    std::fill(traded_.begin(), traded_.end(), LevelSizes{});
    for (const auto& trade : trades) {
        if (!validSize(trade.size) || trade.timestampMs <= window_.timestamp
            || trade.timestampMs > s.timestampMs)
            continue;
        const auto key = detail::priceTick(trade.price, s.tickSize, false);
        const auto row = key ? window_.rowAt(*key) : std::nullopt;
        if (!row)
            continue;
        if (trade.aggressor == Aggressor::Buy)
            addFinite(traded_[*row].ask, trade.size);
        else if (trade.aggressor == Aggressor::Sell)
            addFinite(traded_[*row].bid, trade.size);
    }
}

void BookDiffClassifier::classify(const Snapshot& s, std::int64_t firstTick)
{
    for (std::size_t row = 0; row < s.rows.size(); ++row) {
        const auto previous = window_.rowAt(firstTick + static_cast<std::int64_t>(row));
        if (!previous)
            continue;
        for (const auto side : sides)
            classifySide(s, row, *previous, side);
    }
}

void BookDiffClassifier::classifySide(const Snapshot& s, std::size_t row,
                                    std::size_t previous, Side side)
{
    const double before = previous_[previous].size(side);
    const double after = s.rows[row].size(side);
    if (!validSize(before) || !validSize(after))
        return;
    if (after > before) {
        emit(s, row, side, EventKind::Added, after - before);
        return;
    }
    const double drop = before - after;
    const double eaten = std::min(drop, traded_[previous].size(side));
    emit(s, row, side, EventKind::Eaten, eaten);
    emit(s, row, side, EventKind::Pulled, drop - eaten);
}

void BookDiffClassifier::emit(const Snapshot& s, std::size_t row, Side side,
                             EventKind kind, double size)
{
    if (size > 0 && size >= config_.minimumSize)
        events_.push_back({row, s.firstPrice + row * s.tickSize, side, kind, size, s.timestampMs});
}

void BookDiffClassifier::expire(Milliseconds now)
{
    while (count_ > 0 && now - steps_[head_].timestamp >= config_.windowMs) {
        head_ = (head_ + 1) % steps_.size();
        --count_;
    }
}

BookDiffClassifier::Step BookDiffClassifier::summarize(Milliseconds now) const
{
    Step step{now, {}};
    for (const auto& event : events_) {
        auto& side = step.totals.side[sideIndex(event.side)];
        switch (event.kind) {
        case EventKind::Added: addFinite(side.added, event.size); break;
        case EventKind::Eaten: addFinite(side.eaten, event.size); break;
        case EventKind::Pulled:
            addFinite(side.pulled, event.size);
            includePull(step.totals, event);
            break;
        }
    }
    return step;
}

void BookDiffClassifier::refreshTotals()
{
    totals_ = {};
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& step = steps_[(head_ + i) % steps_.size()].totals;
        for (const auto side : sides) {
            auto& target = totals_.side[sideIndex(side)];
            const auto& source = step.side[sideIndex(side)];
            addFinite(target.added, source.added);
            addFinite(target.pulled, source.pulled);
            addFinite(target.eaten, source.eaten);
            target.largestPull = std::max(target.largestPull, source.largestPull);
        }
        if (step.largestPull)
            includePull(totals_, *step.largestPull);
    }
}

} // namespace sentinel::book
