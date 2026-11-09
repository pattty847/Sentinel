#include "FlowBarBuilder.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace sentinel::flow {
namespace {
std::optional<uint64_t> numericId(const std::string& id) {
    uint64_t n = 0;
    const auto result = std::from_chars(id.data(), id.data() + id.size(), n);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) return {};
    return n;
}
bool tradeLess(const Trade& a, const Trade& b) {
    if (a.exchangeMs != b.exchangeMs) return a.exchangeMs < b.exchangeMs;
    const auto ai = numericId(a.id), bi = numericId(b.id);
    if (ai.has_value() != bi.has_value()) return ai.has_value();
    if (ai && *ai != *bi) return *ai < *bi;
    return a.id < b.id;
}
bool notableLess(const Notable& a, const Notable& b) {
    if (a.notional != b.notional) return a.notional > b.notional;
    if (a.exchangeMs != b.exchangeMs) return a.exchangeMs < b.exchangeMs;
    return a.tradeId < b.tradeId;
}
void pushNotable(std::vector<Notable>& out, Notable n) {
    out.push_back(std::move(n));
    std::sort(out.begin(), out.end(), notableLess);
    if (out.size() > NotableCapacity) out.resize(NotableCapacity);
}
std::optional<int64_t> units(double value, int64_t scale) {
    if (!std::isfinite(value) || value <= 0) return {};
    const long double scaled = static_cast<long double>(value) * scale;
    if (scaled < 0.5L || scaled >= 0x1p63L) return {};
    return static_cast<int64_t>(std::llround(scaled));
}
bool canAdd(int64_t a, int64_t b) { return b >= 0 && a <= INT64_MAX - b; }
}
FlowBarBuilder::FlowBarBuilder(int64_t timeframeMs, double nativeTick,
                               int64_t priceScale, int64_t baseScale, int64_t quoteScale)
    : timeframeMs_(timeframeMs), nativeTick_(nativeTick), priceScale_(priceScale),
      baseScale_(baseScale), quoteScale_(quoteScale), rowTickUnits_(0) {
    if (timeframeMs <= 0 || !std::isfinite(nativeTick) || nativeTick <= 0 ||
        priceScale <= 0 || baseScale <= 0 || quoteScale <= 0 ||
        !std::isfinite(nativeTick * priceScale) || nativeTick * priceScale >= 0x1p63)
        throw std::invalid_argument("invalid flow grid");
    rowTickUnits_ = std::llround(nativeTick * priceScale);
    if (rowTickUnits_ <= 0 || std::abs(double(rowTickUnits_) - nativeTick * priceScale) > 1e-6)
        throw std::invalid_argument("unrepresentable flow row tick");
}
FlowBarBuilder::AddResult FlowBarBuilder::add(const Trade& t) {
    if (!t.id.empty() && idSet_.contains(t.id)) return AddResult::Duplicate;
    if (static_cast<uint8_t>(t.side) > 2) { ++rejectedCount_; return AddResult::Rejected; }
    const auto price = recording::priceUnits(t.price, double(priceScale_));
    const auto base = units(t.size, baseScale_);
    if (!price || !base) { ++rejectedCount_; return AddResult::Rejected; }
    const long double quoteValue = static_cast<long double>(*price) * *base * quoteScale_ /
                                   priceScale_ / baseScale_;
    if (!std::isfinite(quoteValue) || quoteValue < 0.5L || quoteValue >= 0x1p63L) {
        ++rejectedCount_; return AddResult::Rejected;
    }
    const auto quote = static_cast<int64_t>(std::llround(quoteValue));
    const auto row = recording::floorDiv(*price, rowTickUnits_);
    const auto start = recording::floorDiv(t.exchangeMs, timeframeMs_) * timeframeMs_;
    const auto existing = buckets_.find(start);
    const auto* prior = existing == buckets_.end() ? nullptr : &existing->second;
    const auto minRow = prior ? std::min(row, prior->minRow) : row;
    const auto maxRow = prior ? std::max(row, prior->maxRow) : row;
    if (maxRow - minRow > INT16_MAX ||
        (prior && (!canAdd(prior->baseTotal, *base) || !canAdd(prior->quoteTotal, quote) ||
                   (prior->finalPublished && prior->revision == UINT32_MAX)))) {
        ++rejectedCount_; return AddResult::Rejected;
    }
    auto& bucket = buckets_[start];
    AcceptedTrade accepted{t, *price, *base, quote, row};
    const auto place = std::upper_bound(bucket.trades.begin(), bucket.trades.end(), accepted,
        [](const AcceptedTrade& a, const AcceptedTrade& b) { return tradeLess(a.trade, b.trade); });
    bucket.trades.insert(place, std::move(accepted));
    bucket.minRow = minRow; bucket.maxRow = maxRow;
    bucket.baseTotal += *base; bucket.quoteTotal += quote;
    bucket.dirty = true;
    if (bucket.finalPublished) ++bucket.revision;
    if (!t.id.empty()) {
        idSet_.insert(t.id); recentIds_.push_back(t.id);
        if (recentIds_.size() > 4096) {
            idSet_.erase(recentIds_.front()); recentIds_.pop_front();
        }
    }
    return AddResult::Accepted;
}
FlowBar FlowBarBuilder::materialize(int64_t start, Bucket& bucket) {
    if (!bucket.dirty) return bucket.cached;
    FlowBar bar;
    bar.startMs = start; bar.timeframeMs = timeframeMs_; bar.nativeTick = nativeTick_;
    bar.priceScale = priceScale_; bar.baseScale = baseScale_; bar.quoteScale = quoteScale_;
    bar.lowRow = bucket.minRow; bar.revision = bucket.revision;
    std::map<int64_t, Row> rows;
    bool first = true;
    for (const auto& accepted : bucket.trades) {
        const auto& t = accepted.trade;
        if (first) { bar.open = bar.high = bar.low = bar.close = t.price; first = false; }
        else {
            bar.high = std::max(bar.high, t.price);
            bar.low = std::min(bar.low, t.price);
            bar.close = t.price;
        }
        bar.baseVolume += accepted.baseUnits;
        bar.quoteVolume += accepted.quoteUnits;
        ++bar.count;
        auto& row = rows[accepted.row];
        if (t.side == Side::Buy) {
            row.askVol += accepted.baseUnits; ++row.askCount; bar.buyVol += accepted.baseUnits;
        } else if (t.side == Side::Sell) {
            row.bidVol += accepted.baseUnits; ++row.bidCount; bar.sellVol += accepted.baseUnits;
        }
    }
    for (auto& [r, row] : rows) {
        row.offset = static_cast<int16_t>(r - bar.lowRow);
        bar.maxRowVol = std::max(bar.maxRowVol, row.bidVol + row.askVol);
        bar.rows.push_back(row);
    }
    bar.delta = bar.buyVol - bar.sellVol;
    for (size_t i = 0; i < bucket.trades.size();) {
        const auto& firstTrade = bucket.trades[i];
        Notable n{firstTrade.trade.id, firstTrade.trade.exchangeMs, firstTrade.trade.price,
                  firstTrade.baseUnits, firstTrade.quoteUnits, firstTrade.trade.price,
                  firstTrade.trade.price, firstTrade.trade.side, 1};
        std::set<double> prices{firstTrade.trade.price};
        size_t j = i + 1;
        auto previousId = numericId(firstTrade.trade.id);
        while (j < bucket.trades.size()) {
            const auto& next = bucket.trades[j];
            const auto nextId = numericId(next.trade.id);
            if (!previousId || !nextId || *previousId == UINT64_MAX ||
                *nextId != *previousId + 1 || next.trade.exchangeMs != n.exchangeMs ||
                next.trade.side != n.side) break;
            n.size += next.baseUnits; n.notional += next.quoteUnits;
            n.minPrice = std::min(n.minPrice, next.trade.price);
            n.maxPrice = std::max(n.maxPrice, next.trade.price);
            prices.insert(next.trade.price);
            previousId = nextId; ++j;
        }
        n.sweepLevels = static_cast<uint16_t>(std::min<size_t>(prices.size(), UINT16_MAX));
        pushNotable(bar.notable, std::move(n));
        i = j;
    }
    bucket.cached = bar; bucket.dirty = false;
    return bar;
}
std::optional<FlowBar> FlowBarBuilder::bar(int64_t start) {
    auto it = buckets_.find(start);
    if (it == buckets_.end()) return {};
    return materialize(start, it->second);
}
std::vector<FlowBar> FlowBarBuilder::bars() {
    std::vector<FlowBar> out;
    out.reserve(buckets_.size());
    for (auto& [start, bucket] : buckets_) out.push_back(materialize(start, bucket));
    return out;
}
void FlowBarBuilder::sealThrough(int64_t end) {
    for (auto& [start, bucket] : buckets_)
        if (start + timeframeMs_ <= end) bucket.finalPublished = true;
}
void FlowBarBuilder::dropBefore(int64_t start) {
    buckets_.erase(buckets_.begin(), buckets_.lower_bound(start));
}
void FlowBarBuilder::clear() {
    buckets_.clear(); recentIds_.clear(); idSet_.clear(); rejectedCount_ = 0;
}
} // namespace sentinel::flow
