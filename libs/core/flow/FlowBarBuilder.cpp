#include "FlowBarBuilder.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <array>
#include <bit>
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
    const double scaled = value * double(scale);
    if (!std::isfinite(scaled) || scaled < 0.5 || scaled >= 0x1p63) return {};
    return static_cast<int64_t>(std::llround(scaled));
}
bool canAdd(int64_t a, int64_t b) { return b >= 0 && a <= INT64_MAX - b; }

// Six base-2^32 limbs hold a product of three positive int64 quantities.
// Long division keeps quote rounding identical on Apple, Linux and MSVC.
struct Wide {
    std::array<uint32_t, 6> limb{};
    explicit Wide(uint64_t value = 0) { limb[0] = uint32_t(value); limb[1] = uint32_t(value >> 32); }
    bool multiply(uint64_t factor) {
        const auto input = limb;
        limb.fill(0);
        for (size_t part = 0; part < 2; ++part) {
            const uint32_t f = part ? uint32_t(factor >> 32) : uint32_t(factor);
            uint64_t carry = 0;
            for (size_t i = 0; i + part < limb.size(); ++i) {
                const uint64_t sum = uint64_t(input[i]) * f + limb[i + part] + carry;
                limb[i + part] = uint32_t(sum);
                carry = sum >> 32;
            }
            if (carry) return false;
        }
        return true;
    }
    bool atLeast(const Wide& other) const {
        for (size_t i = limb.size(); i-- > 0;)
            if (limb[i] != other.limb[i]) return limb[i] > other.limb[i];
        return true;
    }
    void subtract(const Wide& other) {
        uint64_t borrow = 0;
        for (size_t i = 0; i < limb.size(); ++i) {
            const uint64_t amount = uint64_t(other.limb[i]) + borrow;
            borrow = limb[i] < amount;
            limb[i] = uint32_t((uint64_t(1) << 32) + limb[i] - amount);
        }
    }
    bool shiftOne() {
        uint32_t carry = 0;
        for (auto& word : limb) {
            const uint32_t next = word >> 31;
            word = (word << 1) | carry;
            carry = next;
        }
        return !carry;
    }
    bool bit(unsigned i) const { return (limb[i / 32] >> (i % 32)) & 1u; }
    unsigned bits() const {
        for (unsigned i = limb.size(); i-- > 0;)
            if (limb[i]) return i * 32 + 32 - std::countl_zero(limb[i]);
        return 0;
    }
};

std::optional<int64_t> quoteUnits(int64_t priceUnits, int64_t baseUnits,
                                  int64_t priceScale, int64_t baseScale, int64_t quoteScale) {
    Wide numerator{uint64_t(priceUnits)}, denominator{uint64_t(priceScale)};
    if (!numerator.multiply(uint64_t(baseUnits)) || !numerator.multiply(uint64_t(quoteScale)) ||
        !denominator.multiply(uint64_t(baseScale))) return {};
    Wide remainder;
    uint64_t quotient = 0;
    for (unsigned i = numerator.bits(); i-- > 0;) {
        remainder.shiftOne();
        remainder.limb[0] |= numerator.bit(i);
        if (remainder.atLeast(denominator)) {
            remainder.subtract(denominator);
            if (i >= 63) return {};
            quotient |= uint64_t(1) << i;
        }
    }
    // Nearest integer, exact half upward, for positive quote amounts.
    remainder.shiftOne();
    const bool roundUp = remainder.atLeast(denominator);
    if (quotient > uint64_t(INT64_MAX) - roundUp || quotient + roundUp == 0) return {};
    return int64_t(quotient + roundUp);
}
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
    const auto start = recording::floorDiv(t.exchangeMs, timeframeMs_) * timeframeMs_;
    if (start < droppedBefore_) {
        ++rejectedCount_; ++droppedRejectCount_; return AddResult::Rejected;
    }
    if (!t.id.empty() && idSet_.contains(t.id)) return AddResult::Duplicate;
    if (static_cast<uint8_t>(t.side) > 2) { ++rejectedCount_; return AddResult::Rejected; }
    const auto price = recording::priceUnits(t.price, double(priceScale_));
    const auto base = units(t.size, baseScale_);
    if (!price || !base) { ++rejectedCount_; return AddResult::Rejected; }
    const auto quote = quoteUnits(*price, *base, priceScale_, baseScale_, quoteScale_);
    if (!quote) { ++rejectedCount_; return AddResult::Rejected; }
    const auto row = recording::floorDiv(*price, rowTickUnits_);
    const bool sealed = start < sealedThrough_;
    const auto existing = buckets_.find(start);
    const auto* prior = existing == buckets_.end() ? nullptr : &existing->second;
    const auto minRow = prior ? std::min(row, prior->minRow) : row;
    const auto maxRow = prior ? std::max(row, prior->maxRow) : row;
    if (maxRow - minRow > INT16_MAX ||
        (prior && (!canAdd(prior->baseTotal, *base) || !canAdd(prior->quoteTotal, *quote) ||
                   (sealed && prior->revision == UINT32_MAX)))) {
        ++rejectedCount_; return AddResult::Rejected;
    }
    auto& bucket = buckets_[start];
    AcceptedTrade accepted{t, *price, *base, *quote, row};
    const auto place = std::upper_bound(bucket.trades.begin(), bucket.trades.end(), accepted,
        [](const AcceptedTrade& a, const AcceptedTrade& b) { return tradeLess(a.trade, b.trade); });
    bucket.trades.insert(place, std::move(accepted));
    bucket.minRow = minRow; bucket.maxRow = maxRow;
    bucket.baseTotal += *base; bucket.quoteTotal += *quote;
    bucket.dirty = true;
    if (sealed) ++bucket.revision;
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
    bar.lowRow = bucket.minRow;
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
    if (end % timeframeMs_ != 0) throw std::invalid_argument("unaligned flow seal");
    sealedThrough_ = std::max(sealedThrough_, end);
}
void FlowBarBuilder::dropBefore(int64_t start) {
    droppedBefore_ = std::max(droppedBefore_, std::min(start, sealedThrough_));
    buckets_.erase(buckets_.begin(), buckets_.lower_bound(droppedBefore_));
}
std::optional<uint32_t> FlowBarBuilder::revisionAt(int64_t start) const {
    const auto it = buckets_.find(start);
    if (it == buckets_.end()) return {};
    return it->second.revision;
}
void FlowBarBuilder::clear() {
    buckets_.clear(); recentIds_.clear(); idSet_.clear();
    rejectedCount_ = droppedRejectCount_ = 0;
    sealedThrough_ = droppedBefore_ = INT64_MIN;
}
} // namespace sentinel::flow
