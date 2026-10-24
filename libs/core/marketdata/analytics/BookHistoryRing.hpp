#pragma once

#include "WindowState.hpp"
#include <vector>

namespace sentinel::book {

struct HistoryConfig {
    std::size_t maxRows = 256;
    std::size_t columns = 240;
    Milliseconds sampleIntervalMs = 250;
};

// Preallocated columns x maxRows storage. Each sampled column retains its own
// price origin/row count. cell() aligns older columns to the latest accepted
// visible window, even when that update was not due for sampling. Out-of-window
// cells are nullopt, never fabricated zero liquidity. Invalid individual sizes
// are stored as NaN (unknown); validSize() distinguishes them from real zero.
// The first observation anchors a fixed interval grid and samples immediately.
// Each later update samples the current grid slot once, without shifting future
// due times. Skipped slots occupy missing columns: timestamp() and cell() return
// nullopt for them. size() includes missing slots. Column 0 is newest; timestamps
// are actual observation times, not nominal slot times. Wrap overwrites oldest.
// Catch-up work is bounded by column capacity, even after arbitrarily long gaps.
class BookHistoryRing {
public:
    explicit BookHistoryRing(HistoryConfig config = {});
    UpdateStatus update(const Snapshot& snapshot);
    void reset();
    std::size_t size() const { return count_; }
    std::optional<Milliseconds> timestamp(std::size_t columnsAgo) const;
    std::optional<LevelSizes> cell(std::size_t columnsAgo, std::size_t visibleRow) const;

private:
    struct Column {
        std::int64_t firstTick = 0;
        std::size_t rows = 0;
        Milliseconds timestamp = 0;
    };
    std::size_t index(std::size_t columnsAgo) const;
    void sample(const Snapshot& snapshot);
    bool advanceGrid(Milliseconds now);
    void advanceColumn();

    HistoryConfig config_;
    detail::WindowState window_;
    std::vector<Column> columns_;
    std::vector<LevelSizes> cells_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    Milliseconds slotTime_ = 0;
};

} // namespace sentinel::book
