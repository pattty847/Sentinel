#pragma once

#include "WindowState.hpp"
#include <optional>
#include <vector>

namespace sentinel::book {

enum class EventKind : std::uint8_t { Added, Pulled, Eaten };
struct BookEvent {
    std::size_t row = 0; // Row in the snapshot that produced the event.
    double price = 0;
    Side side = Side::Bid;
    EventKind kind = EventKind::Added;
    double size = 0;
    Milliseconds timestampMs = 0;
};
struct SideTotals {
    double added = 0;
    double pulled = 0;
    double eaten = 0;
    double largestPull = 0;
};
struct RollingTotals {
    std::array<SideTotals, 2> side{};
    std::optional<BookEvent> largestPull;
};
struct DiffConfig {
    std::size_t maxRows = 256;
    std::size_t maxSteps = 2048; // Number of nonempty event steps retained.
    Milliseconds windowMs = 10000;
    double minimumSize = 0;
};

// Snapshot/trade matching is a heuristic; exact order requires the raw journal.
// Only overlapping prices are diffed. Entering rows establish a baseline; leaving
// rows are unobserved, not pulled. Sides are independent: a bid->ask crossing is
// a bid decrease (eat/pull) plus an ask add. Buy aggression eats asks, sell eats
// bids; unknown aggression is ignored. Supply each trade batch once, on the next
// snapshot, in the same monotonic clock, in (previous timestamp, current timestamp].
// outOfIntervalTradesThisUpdate/Total count trades rejected by that time filter
// (independent of size/price validity), to reveal feeder clock mismatches. Counts
// include accepted comparison updates only, saturate at UINT64_MAX, and reset on
// reset/symbol/tick/withdrawal. The per-update count is zero on a rejected update
// or fresh baseline (which has no trade interval). Other ignored trades are not
// included in these timestamp diagnostics.
// A refill without a net drop is not eaten. Threshold applies to each classified
// component after splitting. Invalid sizes break that side's comparison baseline.
// Totals cover (now-windowMs, now], advanced on accepted snapshots. Max pull ties
// keep the oldest event. Numeric sums saturate at the largest finite double.
// On capacity overflow the update is rejected atomically; callers
// must provision enough capacity (or explicitly reset), never silently lose data.
class BookDiffClassifier {
public:
    explicit BookDiffClassifier(DiffConfig config = {});
    UpdateStatus update(const Snapshot& snapshot, std::span<const Trade> trades = {});
    void reset();
    std::span<const BookEvent> events() const { return events_; }
    const RollingTotals& totals() const { return totals_; }
    std::uint64_t outOfIntervalTradesThisUpdate() const { return rejectedThisUpdate_; }
    std::uint64_t outOfIntervalTradesTotal() const { return rejectedTotal_; }

private:
    struct Step {
        Milliseconds timestamp = 0;
        RollingTotals totals;
    };
    std::uint64_t matchTrades(const Snapshot& snapshot, std::span<const Trade> trades);
    void classify(const Snapshot& snapshot, std::int64_t firstTick);
    void classifySide(const Snapshot& snapshot, std::size_t row, std::size_t previous, Side side);
    void emit(const Snapshot& snapshot, std::size_t row, Side side, EventKind kind, double size);
    void expire(Milliseconds now);
    void refreshTotals();
    Step summarize(Milliseconds now) const;

    DiffConfig config_;
    detail::WindowState window_;
    std::vector<LevelSizes> previous_;
    std::vector<LevelSizes> traded_;
    std::vector<BookEvent> events_;
    std::vector<Step> steps_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    RollingTotals totals_;
    std::uint64_t rejectedThisUpdate_ = 0;
    std::uint64_t rejectedTotal_ = 0;
};

} // namespace sentinel::book
