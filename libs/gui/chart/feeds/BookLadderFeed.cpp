#include "chart/feeds/BookLadderFeed.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "marketdata/model/TradeData.h"
#include "SentinelLogging.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>

namespace chart {
namespace {
// The integer m with x == m * unit within 1e-6 units, else false.
bool wholeMultiple(double x, double unit, int64_t& m) {
    const double ratio = x / unit;
    m = std::llround(ratio);
    return std::abs(ratio - double(m)) < 1e-6;
}
int64_t ceilDiv(int64_t a, int64_t b) { return -floorDiv(-a, b); }
double priceAt(const LiveOrderBook::DenseBookSnapshotView& view, std::span<const std::pair<uint32_t, double>> side) {
    return side.empty() ? 0.0 : view.minPrice + side.front().first * view.tickSize;
}
// Bins wholly inside what the capture knows on both sides (server-level indices
// [base + bidKnownLo, base + min(askKnownEnd, slotCount))).
LadderBins knownBins(const LiveOrderBook::DenseBookSnapshotView& view, int64_t base, int64_t group) {
    const int64_t lo = base + int64_t(view.bidKnownLo);
    const int64_t end = base + int64_t(std::min(view.askKnownEnd, view.slotCount));
    return {ceilDiv(lo, group), floorDiv(end, group) - 1};
}
const char* incompleteReason(const LiveOrderBook::DenseBookSnapshotView& view, LadderBins window,
                             int64_t base, int64_t group) {
    const int64_t lo = window.lo * group - base, end = (window.hi + 1) * group - base;
    if (lo < 0 || end > int64_t(view.slotCount)) return "band";
    if (lo < int64_t(view.bidKnownLo)) return view.bidCapped ? "levels" : "scan";
    return view.askCapped ? "levels" : "scan";
}
// The median positive bin of one side's kWallReferenceBins display bins from its touch
// outward (the touch bin included), over the bins the capture knows wholly: bids down
// to known.lo, asks up to known.hi. Levels come best first.
double touchMedian(std::span<const std::pair<uint32_t, double>> levels, int64_t base, int64_t group, bool bid,
                   LadderBins known) {
    std::array<double, BookLadderFeed::kWallReferenceBins> bins{};
    if (levels.empty()) return 0;
    const int64_t touch = floorDiv(base + int64_t(levels.front().first), group);
    for (const auto& [index, qty] : levels) {
        const int64_t bin = floorDiv(base + int64_t(index), group);
        const int64_t offset = bid ? touch - bin : bin - touch;
        if (offset >= int64_t(bins.size()) || (bid ? bin < known.lo : bin > known.hi)) break;
        bins[size_t(offset)] += qty;
    }
    return sentinel::book::positiveMedian(bins);
}
// "BTC-USD" -> "BTC" (at most 7 characters).
void baseAsset(const std::string& product, char (&out)[8]) {
    const size_t n = std::min({product.find('-'), product.size(), sizeof(out) - 1});
    std::fill(std::begin(out), std::end(out), '\0');
    std::copy_n(product.begin(), n, out);
}
} // namespace

BookLadderFeed::BookLadderFeed(LadderMailbox& mailbox) : mailbox_(mailbox) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { publishRows(); });
    monotonic_.start();
}

void BookLadderFeed::publish(const LiveOrderBook& book) {
    // A resnapshot re-initializes the same replica: a new tracker series (no stale ages).
    const uint64_t epoch = book.epoch();
    if (&book != book_ || epoch != epoch_) {
        epoch_ = epoch;
        tracker_.reset();
        baseAsset(book.getProductId(), asset_);
    }
    book_ = &book;
    const auto view = book.captureDenseNonZero(top_.bids, top_.asks, 1, kMaxScanWords);
    bestBid_ = priceAt(view, view.bidLevels);
    bestAsk_ = priceAt(view, view.askLevels);
    if (ports_.top) ports_.top(bestBid_, bestAsk_);
    const double tick = book.getTickSize();
    if (!(tick > 0)) { // a cleared replica: withdraw, keep the receive time
        timer_.stop();
        lastTick_ = 0;
        if (active()) withdraw("withdrawn");
        return;
    }
    receivedAtMs_ = ports_.clock ? ports_.clock() : 0;
    if (lastTick_ > 0 && tick != lastTick_ && active()) withdraw("tick");
    lastTick_ = tick;
    if (active()) arm(kIntervalMs);
}

void BookLadderFeed::beginGeneration() {
    ++generation_;
    timer_.stop();
    book_ = nullptr;
    lastTick_ = 0;
    receivedAtMs_ = 0;
    bestBid_ = bestAsk_ = 0;
    withdraw("generation");
}

void BookLadderFeed::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    const bool was = active();
    enabled_ = enabled;
    activeChanged(was);
}

void BookLadderFeed::setExposed(bool exposed) {
    if (exposed_ == exposed) return;
    const bool was = active();
    exposed_ = exposed;
    activeChanged(was);
}

void BookLadderFeed::activeChanged(bool wasActive) {
    if (wasActive == active()) return;
    if (active()) {
        arm(0);
        return;
    }
    timer_.stop();
    withdraw("suspended"); // no rows survive a suspension
}

void BookLadderFeed::onFrameChanged() {
    if (active()) arm(kIntervalMs);
}

void BookLadderFeed::setRadar(bool on, double multiple) {
    multiple_ = multiple;
    if (radar_ == on) return;
    radar_ = on;
    tracker_.reset();
    if (active()) arm(0); // on: a fresh baseline at once; off: plain bars at once
}

// Earliest deadline: a shorter request moves a pending publication forward.
void BookLadderFeed::arm(int ms) {
    if (!timer_.isActive() || timer_.remainingTime() > ms) timer_.start(ms);
}

int64_t BookLadderFeed::monotonicMs() const { return ports_.monotonic ? ports_.monotonic() : monotonic_.elapsed(); }

LadderSnapshot& BookLadderFeed::begin(const MappingFrameContext& frame) {
    auto& s = mailbox_.back();
    s.generation = generation_;
    s.serial = ++serial_;
    s.valid = s.complete = false;
    s.serverTick = 0;
    s.group = s.binLo = 0;
    s.rowCount = 0;
    const auto& cam = frame.raster;
    s.evalTick = cam.valid && cam.tick > 0 ? cam.tick : 0;
    const auto window = s.evalTick > 0 ? ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, cam.tick) : LadderBins{};
    s.evalLo = window.lo;
    s.evalHi = window.hi;
    s.bestBid = bestBid_;
    s.bestAsk = bestAsk_;
    s.receivedAtMs = receivedAtMs_;
    s.radar = false;
    s.wallMultiple = 0;
    s.bidMedian = s.askMedian = 0;
    s.walls = {};
    std::copy(std::begin(asset_), std::end(asset_), s.asset);
    return s;
}

void BookLadderFeed::post() {
    mailbox_.post();
    if (ports_.update) ports_.update();
}

void BookLadderFeed::withdraw(const char* reason) {
    begin(ports_.frame ? ports_.frame() : MappingFrameContext{});
    reason_ = reason;
    tracker_.reset(); // every withdrawal ends the tracked series
    sLog_Probe("ladder.withdraw", "gen=" << generation_ << " serial=" << serial_ << " reason=" << reason);
    post();
}

void BookLadderFeed::publishRows() {
    if (!active()) return;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    auto& s = begin(frame);
    walls_.begin(frame.raster.drawnMinPrice, frame.raster.drawnMaxPrice, {{}, multiple_});
    reason_ = fill(s, frame.raster);
    if (radar_) tracker_.publish(s, series(), monotonicMs(), walls_);
    ++rowPublications_;
    sLog_Probe("ladder.publish", "gen=" << generation_ << " serial=" << serial_ << " valid=" << s.valid
               << " complete=" << s.complete << " reason=" << reason_ << " rows=" << s.rowCount
               << " bidLevels=" << rows_.bids.size() << " askLevels=" << rows_.asks.size()
               << " radar=" << radar_ << " us=" << double(elapsed.nsecsElapsed()) / 1000.0);
    post();
    if (radar_) arm(radarIntervalMs_);
}

const char* BookLadderFeed::fill(LadderSnapshot& s, const chart_raster::RasterCamera& cam) {
    rows_.bids.clear();
    rows_.asks.clear();
    if (!(s.evalTick > 0)) return "raster";
    if (!book_ || !(book_->getTickSize() > 0)) return "book";
    const double serverTick = book_->getTickSize();
    int64_t base = 0, group = 0;
    if (!wholeMultiple(book_->getMinPrice(), serverTick, base) || !wholeMultiple(cam.tick, serverTick, group) ||
        group < 1)
        return "grid";
    const auto window = ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, double(group) * serverTick);
    const int64_t bins = window.hi - window.lo + 1;
    if (bins > LadderSnapshot::kMaxRows) return "wide";
    const auto view = book_->captureDenseNonZero(rows_.bids, rows_.asks, scanLevels_, scanWords_);
    // Rows: the window plus a margin of a quarter window each side (a drag between
    // publications keeps its bars), limited to what the capture knows.
    const int64_t margin = std::min<int64_t>(std::max<int64_t>(1, bins / 4), (LadderSnapshot::kMaxRows - bins) / 2);
    const auto known = knownBins(view, base, group);
    const bool complete = window.lo >= known.lo && window.hi <= known.hi;
    if (radar_ && complete)
        walls_.reference.medians = {touchMedian(view.bidLevels, base, group, true, known),
                                    touchMedian(view.askLevels, base, group, false, known)};
    else if (radar_)
        walls_.reference.medians = tracker_.medians(); // not an observation: the last accepted
    s.binLo = std::min(window.lo, std::max(window.lo - margin, known.lo));
    const int64_t binHi = std::max(window.hi, std::min(window.hi + margin, known.hi));
    s.rowCount = int(binHi - s.binLo + 1);
    s.serverTick = serverTick;
    s.group = group;
    std::fill_n(s.rows.begin(), s.rowCount, LadderRow{});
    const auto add = [&](std::span<const std::pair<uint32_t, double>> levels, float LadderRow::*side, int sideIndex) {
        for (const auto& [index, qty] : levels) {
            walls_.consider(sideIndex, view.minPrice + double(index) * view.tickSize, qty);
            const int64_t row = floorDiv(base + int64_t(index), group) - s.binLo;
            if (row >= 0 && row < s.rowCount) s.rows[size_t(row)].*side += float(qty);
        }
    };
    add(view.bidLevels, &LadderRow::bid, LadderSnapshot::kBid);
    add(view.askLevels, &LadderRow::ask, LadderSnapshot::kAsk);
    walls_.setCapped(view.bidKnownLo > 0, view.askKnownEnd < view.slotCount);
    s.valid = true;
    s.complete = complete;
    return s.complete ? "ok" : incompleteReason(view, window, base, group);
}
} // namespace chart
