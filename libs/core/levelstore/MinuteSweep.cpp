#include "MinuteSweep.hpp"
#include "ExactDecimal.hpp"
#include <algorithm>
#include <stdexcept>

namespace sentinel::levelstore {
namespace {
constexpr int64_t kMinute = 60'000;
int64_t floorDiv(int64_t v, int64_t d) { return v / d - (v % d < 0 ? 1 : 0); }
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(std::string("MinuteSweep: ") + what);
}
} // namespace

MinuteSweep::MinuteSweep(int64_t binUnits, bool nativeUnits) : binUnits_(binUnits), nativeUnits_(nativeUnits && binUnits == 1) {
    if (binUnits_ <= 0) throw std::invalid_argument("MinuteSweep: binUnits");
}

MinuteSweep::Key MinuteSweep::keyOf(bool isBid, int64_t units) const {
    return {isBid, binUnits_ == 1 ? units : floorDiv(units, binUnits_)};
}

void MinuteSweep::start(const BookImage &image) {
    priceDigits_ = image.priceDigits;
    sizeDigits_ = image.sizeDigits;
    previousBucket_ = INT64_MIN;
    levels_[0].clear();
    levels_[1].clear();
    bins_.clear();
    observed_ = image.observed;
    for (const auto &l : image.levels) {
        levels_[l.isBid][l.units] = l.size;
        Bin &b = bins_[keyOf(l.isBid, l.units)];
        ++b.count;
        b.sum = binUnits_ == 1 ? static_cast<Wide>(l.size) : b.sum + l.size;
    }
    started_ = true;
}

// First touch of a bin in a minute: the minute's integrals restart and its peak
// starts at the observed state at the minute start. Every live bin is touched at
// each observation change, clear and reset, so a later first touch still sees the
// observation the minute started with (bins with no level hold 0 throughout).
MinuteSweep::Bin &MinuteSweep::at(const Key &k) {
    Bin &b = bins_[k];
    if (b.minute != minute_) {
        b.minute = minute_;
        b.integral = 0;
        b.presence = 0;
        b.peak = observed_ ? b.sum : 0;
        b.lastT = minuteStart_;
        touched_.push_back(k);
    }
    return b;
}

void MinuteSweep::accrue(Bin &b, int64_t t) const {
    if (observed_ && t > b.lastT) {
        b.integral += b.sum * static_cast<Wide>(t - b.lastT);
        if (b.count > 0) b.presence += t - b.lastT;
    }
    b.lastT = std::max(b.lastT, t);
}

void MinuteSweep::accrueLive(int64_t t) {
    for (auto &[k, b] : bins_)
        if (b.count > 0) accrue(at(k), t);
}

void MinuteSweep::changeScale(uint32_t priceDigits, uint32_t sizeDigits) {
    if (priceDigits == priceDigits_ && sizeDigits == sizeDigits_) return;
    require(priceDigits >= priceDigits_ && sizeDigits >= sizeDigits_, "coarser period scale");
    const auto bin = decimalAtScale({binUnits_, priceDigits_}, priceDigits);
    if(!bin)throw std::overflow_error("bin scale overflow");
    std::unordered_map<int64_t, int64_t> next[2];
    for (bool bid : {false, true}) for (const auto& [p, q] : levels_[bid]) {
        const auto px = decimalAtScale({p, priceDigits_}, priceDigits);
        const auto qty = decimalAtScale({q, sizeDigits_}, sizeDigits);
        if(!px||!qty)throw std::overflow_error("book scale overflow");
        next[bid].emplace(*px, *qty);
    }
    binUnits_ = nativeUnits_ ? 1 : *bin;
    levels_[0].swap(next[0]); levels_[1].swap(next[1]);
    Wide factor = 1;
    for (auto d = sizeDigits_; d < sizeDigits; ++d) factor *= 10;
    std::unordered_map<Key, Bin, KeyHash> rebased;
    for (const auto& entry : bins_) {
        auto k = entry.first; auto b = entry.second;
        if (nativeUnits_) {
            const auto price = decimalAtScale({k.bin, priceDigits_}, priceDigits);
            if(!price)throw std::overflow_error("bin price overflow");
            k.bin = *price;
        }
        b.sum *= factor; b.integral *= factor; b.peak *= factor;
        rebased.emplace(k, std::move(b));
    }
    bins_.swap(rebased);
    for (auto& k : touched_) if (nativeUnits_) {
        const auto price = decimalAtScale({k.bin, priceDigits_}, priceDigits);
        if(!price)throw std::overflow_error("touched bin price overflow"); k.bin = *price;
    }
    priceDigits_ = priceDigits; sizeDigits_ = sizeDigits;
}

void MinuteSweep::apply(const MinuteBlock &block, std::vector<BinRow> &rows, int64_t &coveredMs) {
    require(started_, "not started");
    require(block.bucketStartMs % kMinute == 0 && block.bucketStartMs > previousBucket_, "block order");
    const bool continues = previousBucket_ != INT64_MIN && block.bucketStartMs == previousBucket_ + kMinute;
    require(!continues || block.observedAtStart == observed_, "observation does not continue");
    changeScale(block.priceDigits, block.sizeDigits);
    observed_ = block.observedAtStart;
    previousBucket_ = block.bucketStartMs;
    rows.clear();
    touched_.clear();
    ++minute_;
    minuteStart_ = block.bucketStartMs;
    const int64_t end = block.bucketStartMs + kMinute;
    int64_t covered = 0, since = block.bucketStartMs, previous = block.bucketStartMs;
    size_t c = 0;
    std::vector<Key> messageBins;
    for (uint32_t i = 0; i < block.messages.size(); ++i) {
        const auto &m = block.messages[i];
        require(m.timeMs >= previous && m.timeMs < end, "message time");
        previous = m.timeMs;
        const int64_t t = m.timeMs;
        const bool transition = m.observed != observed_;
        ++message_;
        messageBins.clear();
        const bool scaleChange = m.priceDigits != priceDigits_ || m.sizeDigits != sizeDigits_;
        if (transition || m.reset || m.clear != ClearCause::None || scaleChange) accrueLive(t);
        if (m.reset) { levels_[0].clear(); levels_[1].clear(); }
        changeScale(m.priceDigits, m.sizeDigits);
        if (m.clear != ClearCause::None) {
            for (auto &[k, b] : bins_)
                if (b.count > 0) {
                    at(k);
                    b.count = 0;
                    b.sum = 0;
                }
            levels_[0].clear();
            levels_[1].clear();
        }
        for (; c < block.changes.size() && block.changes[c].message == i; ++c) {
            const auto &ch = block.changes[c];
            const Key k = keyOf(ch.isBid, ch.units);
            Bin &b = at(k);
            if (b.message != message_) {
                accrue(b, t);
                b.message = message_;
                messageBins.push_back(k);
            }
            auto &side = levels_[ch.isBid];
            const auto it = side.find(ch.units);
            const int64_t old = it == side.end() ? 0 : it->second;
            if (ch.size > 0) side[ch.units] = ch.size;
            else if (it != side.end()) side.erase(it);
            if (m.reset) continue; // rows are rebuilt below
            b.count = static_cast<uint32_t>(static_cast<int64_t>(b.count) + (ch.size > 0) - (old > 0));
            if (binUnits_ == 1) b.sum = ch.size;
            else {
                const Wide delta = static_cast<Wide>(ch.size) - old;
                b.sum = b.count == 0 ? Wide(0) : Wide(b.sum + delta);
                require(b.sum >= 0, "negative aggregate");
            }
        }
        require(c == block.changes.size() || block.changes[c].message > i, "change order");
        if (m.reset) {
            // A snapshot rebuilds every row from nothing, as the recorder does.
            for (auto &[k, b] : bins_) {
                b.count = 0;
                b.sum = 0;
            }
            for (bool isBid : {true, false})
                for (const auto &[units, size] : levels_[isBid]) {
                    Bin &b = at(keyOf(isBid, units));
                    ++b.count;
                    b.sum = binUnits_ == 1 ? static_cast<Wide>(size) : b.sum + size;
                }
        }
        if (observed_ && !m.observed) covered += t - since;
        if (!observed_ && m.observed) since = t;
        observed_ = m.observed;
        if (observed_) {
            // The state after the whole message: every live bin when observation
            // starts or a snapshot replaced the book, else the bins it changed.
            if (transition || m.reset) {
                for (auto &[k, b] : bins_)
                    if (b.count > 0) at(k).peak = std::max(b.peak, b.sum);
            } else {
                for (const auto &k : messageBins) {
                    Bin &b = bins_[k];
                    b.peak = std::max(b.peak, b.sum);
                }
            }
        }
    }
    require(c == block.changes.size(), "dangling change");
    if (observed_) covered += end - since;
    for (auto &[k, b] : bins_)
        if (b.count > 0) at(k);
    rows.reserve(touched_.size());
    for (const auto &k : touched_) {
        Bin &b = bins_[k];
        accrue(b, end);
        const Wide last = b.count > 0 ? b.sum : 0;
        if (b.integral > 0 || b.presence > 0 || b.peak > 0 || last > 0)
            rows.push_back({k.isBid, k.bin, b.integral, b.presence, b.peak, last});
    }
    std::erase_if(bins_, [](const auto &e) { return e.second.count == 0; });
    std::sort(rows.begin(), rows.end(), [](const BinRow &a, const BinRow &b) {
        return a.isBid != b.isBid ? a.isBid : a.bin < b.bin;
    });
    coveredMs = covered;
}

} // namespace sentinel::levelstore
