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
// First observation samples immediately. Subsequent samples are >= interval
// apart; missed intervals are NOT filled with fabricated observations. Column 0
// is newest. Sample timestamp is actual observation time. Wrap overwrites oldest.
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

    HistoryConfig config_;
    detail::WindowState window_;
    std::vector<Column> columns_;
    std::vector<LevelSizes> cells_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

} // namespace sentinel::book
