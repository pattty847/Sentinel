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

void WallCandidates::begin(double minPrice, double maxPrice, const sentinel::book::LargeReference& wallReference) {
    cells = {};
    reference = wallReference;
    drawnMin = minPrice;
    drawnMax = maxPrice;
}

void WallCandidates::consider(int side, double price, double size) {
    const double median = reference.medians[size_t(side)];
    if (!(median > 0) || !(size >= reference.multiple * median)) return;
    int direction = LadderSnapshot::kBelow;
    if (price >= drawnMax) direction = LadderSnapshot::kAbove;
    else if (price >= drawnMin) return;
    auto& c = cells[size_t(side)][size_t(direction)];
    const bool nearer = direction == LadderSnapshot::kAbove ? price < c.price : price > c.price;
    if (!c.found || nearer) c = {price, float(size), true, c.capped};
}

void WallCandidates::setCapped(bool bid, bool ask) {
    for (auto& c : cells[LadderSnapshot::kBid]) c.capped = bid;
    for (auto& c : cells[LadderSnapshot::kAsk]) c.capped = ask;
}

LadderTracker::LadderTracker()
    : tracker_({.maxRows = size_t(LadderSnapshot::kMaxRows)}), sizes_(size_t(LadderSnapshot::kMaxRows)) {}

void LadderTracker::reset() { tracker_.reset(); }

void LadderTracker::publish(LadderSnapshot& s, uint64_t series, int64_t nowMs, const WallCandidates& walls) {
    if (!s.valid) reset();
    s.radar = true;
    s.wallMultiple = walls.reference.multiple;
    if (s.valid && s.complete && observe(s, series, nowMs, walls.reference)) markRows(s);
    s.bidMedian = float(walls.reference.medians[size_t(Side::Bid)]);
    s.askMedian = float(walls.reference.medians[size_t(Side::Ask)]);
    s.walls = walls.cells;
}

bool LadderTracker::observe(LadderSnapshot& s, uint64_t series, int64_t nowMs,
                            const sentinel::book::LargeReference& reference) {
    const int64_t stamp = std::max(nowMs, lastStamp_ + 1);
    lastStamp_ = stamp;
    const size_t rows = size_t(s.rowCount);
    for (size_t i = 0; i < rows; ++i) sizes_[i] = {s.rows[i].bid, s.rows[i].ask};
    const double bin = s.binPrice();
    const auto status = tracker_.update({series, bin, double(s.binLo) * bin,
                                         std::span<const sentinel::book::LevelSizes>(sizes_.data(), rows), stamp},
                                        reference);
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

void LadderTracker::markRows(LadderSnapshot& s) const {
    for (size_t i = 0; i < size_t(s.rowCount); ++i) {
        auto& row = s.rows[i];
        const auto bid = tracker_.level(i, Side::Bid), ask = tracker_.level(i, Side::Ask);
        row.bidWallMs = saturatedMs(bid.largeForMs);
        row.askWallMs = saturatedMs(ask.largeForMs);
        row.bidFlickers = saturatedCount(bid.flickers);
        row.askFlickers = saturatedCount(ask.flickers);
        row.flags = uint8_t((bid.large ? LadderRow::kBidLarge : 0) | (ask.large ? LadderRow::kAskLarge : 0) |
                            (hot(bid) ? LadderRow::kBidHot : 0) | (hot(ask) ? LadderRow::kAskHot : 0));
    }
}
} // namespace chart
