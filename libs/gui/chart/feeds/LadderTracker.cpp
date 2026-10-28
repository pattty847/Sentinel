#include "chart/feeds/LadderTracker.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <limits>
#include <span>

namespace chart {
namespace {
using sentinel::book::Side;
using sentinel::book::UpdateStatus;

uint32_t saturatedMs(int64_t ms) {
    return uint32_t(std::clamp<int64_t>(ms, 0, std::numeric_limits<uint32_t>::max()));
}
uint16_t saturatedCount(uint64_t n) {
    return uint16_t(std::min<uint64_t>(n, std::numeric_limits<uint16_t>::max()));
}
bool hot(const sentinel::book::LevelInfo& info) {
    return info.flickers > 0 && info.sinceLastFlickerMs >= 0 && info.sinceLastFlickerMs <= kFlickerHotMs;
}
} // namespace

void WallCandidates::begin(double minPrice, double maxPrice) {
    cells = {};
    drawnMin = minPrice;
    drawnMax = maxPrice;
}

void WallCandidates::consider(int side, double price, double size) {
    int direction = LadderSnapshot::kBelow;
    if (price >= drawnMax) direction = LadderSnapshot::kAbove;
    else if (price >= drawnMin) return;
    auto& c = cells[size_t(side)][size_t(direction)];
    if (float(size) > c.size) {
        c.price = price;
        c.size = float(size);
    }
}

void WallCandidates::setCapped(bool bid, bool ask) {
    for (auto& c : cells[LadderSnapshot::kBid]) c.capped = bid;
    for (auto& c : cells[LadderSnapshot::kAsk]) c.capped = ask;
}

LadderTracker::LadderTracker()
    : tracker_({.maxRows = size_t(LadderSnapshot::kMaxRows)}), sizes_(size_t(LadderSnapshot::kMaxRows)) {}

void LadderTracker::reset() { tracker_.reset(); }

void LadderTracker::publish(LadderSnapshot& s, uint64_t series, int64_t nowMs, double multiple,
                            const WallCandidates& walls) {
    if (!s.valid) reset();
    s.radar = true;
    s.wallMultiple = multiple;
    if (s.valid && s.complete && observe(s, series, nowMs)) markRows(s, multiple);
    s.bidMedian = float(tracker_.median(Side::Bid));
    s.askMedian = float(tracker_.median(Side::Ask));
    markWalls(s, multiple, walls);
}

bool LadderTracker::observe(LadderSnapshot& s, uint64_t series, int64_t nowMs) {
    const int64_t stamp = std::max(nowMs, lastStamp_ + 1);
    lastStamp_ = stamp;
    const size_t rows = size_t(s.rowCount);
    for (size_t i = 0; i < rows; ++i) sizes_[i] = {s.rows[i].bid, s.rows[i].ask};
    const double bin = s.binPrice();
    const auto status = tracker_.update({series, bin, double(s.binLo) * bin,
                                         std::span<const sentinel::book::LevelSizes>(sizes_.data(), rows), stamp});
    if (status == UpdateStatus::Accepted) {
        ++accepted_;
        return true;
    }
    ++rejected_;
    if (warnedSeries_ != series) {
        warnedSeries_ = series;
        sLog_Warning("Wall radar: tracker rejected an update status=" << int(status) << " series=" << series
                     << " rows=" << rows << " stamp=" << stamp);
    }
    return false;
}

void LadderTracker::markRows(LadderSnapshot& s, double multiple) const {
    for (size_t i = 0; i < size_t(s.rowCount); ++i) {
        auto& row = s.rows[i];
        const auto bid = tracker_.level(i, Side::Bid), ask = tracker_.level(i, Side::Ask);
        row.bidSeenMs = saturatedMs(bid.seenForMs);
        row.askSeenMs = saturatedMs(ask.seenForMs);
        row.bidFlickers = saturatedCount(bid.flickers);
        row.askFlickers = saturatedCount(ask.flickers);
        row.flags = uint8_t((tracker_.isLarge(i, Side::Bid, multiple) ? LadderRow::kBidLarge : 0) |
                            (tracker_.isLarge(i, Side::Ask, multiple) ? LadderRow::kAskLarge : 0) |
                            (hot(bid) ? LadderRow::kBidHot : 0) | (hot(ask) ? LadderRow::kAskHot : 0));
    }
}

void LadderTracker::markWalls(LadderSnapshot& s, double multiple, const WallCandidates& walls) const {
    const double medians[2] = {tracker_.median(Side::Bid), tracker_.median(Side::Ask)};
    for (int side = 0; side < 2; ++side) {
        for (int direction = 0; direction < 2; ++direction) {
            auto& out = s.walls[size_t(side)][size_t(direction)];
            out = walls.cells[size_t(side)][size_t(direction)];
            out.found = medians[side] > 0 && double(out.size) >= multiple * medians[side];
        }
    }
}
} // namespace chart
