#include "MinuteSweep.hpp"
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

MinuteSweep::MinuteSweep(int64_t binUnits) : binUnits_(binUnits) {
    if (binUnits_ <= 0) throw std::invalid_argument("MinuteSweep: binUnits");
}

MinuteSweep::Key MinuteSweep::keyOf(bool isBid, int64_t units) const {
    return {isBid, binUnits_ == 1 ? units : floorDiv(units, binUnits_)};
}

void MinuteSweep::start(const BookImage &image) {
    previousBucket_ = INT64_MIN;
    previousDamaged_ = damaged_ = false;
    levels_[0].clear();
    levels_[1].clear();
    bins_.clear();
    observed_ = image.observed;
    for (const auto &l : image.levels) {
        levels_[l.isBid][l.units] = l.size;
        Bin &b = bins_[keyOf(l.isBid, l.units)];
        ++b.count;
        b.sum = binUnits_ == 1 ? static_cast<long double>(l.size) : b.sum + l.size;
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
        b.peak = observed_ && !damaged_ ? b.sum : 0;
        b.lastT = minuteStart_;
        touched_.push_back(k);
    }
    return b;
}

void MinuteSweep::accrue(Bin &b, int64_t t) const {
    if (observed_ && !damaged_ && t > b.lastT) {
        b.integral += b.sum * static_cast<long double>(t - b.lastT);
        if (b.count > 0) b.presence += t - b.lastT;
    }
    b.lastT = std::max(b.lastT, t);
}

void MinuteSweep::accrueLive(int64_t t) {
    for (auto &[k, b] : bins_)
        if (b.count > 0) accrue(at(k), t);
}

void MinuteSweep::apply(const MinuteBlock &block, std::vector<BinRow> &rows, int64_t &coveredMs) {
    require(started_, "not started");
    require(block.bucketStartMs % kMinute == 0 && block.bucketStartMs > previousBucket_, "block order");
    const bool continues = previousBucket_ != INT64_MIN && block.bucketStartMs == previousBucket_ + kMinute &&
                           !previousDamaged_;
    require(!continues || block.damaged || block.observedAtStart == observed_, "observation does not continue");
    observed_ = block.observedAtStart;
    damaged_ = block.damaged;
    previousDamaged_ = block.damaged;
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
        if (transition || m.reset || m.clear != ClearCause::None) accrueLive(t);
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
            const double old = it == side.end() ? 0.0 : it->second;
            if (ch.size > 0) side[ch.units] = ch.size;
            else if (it != side.end()) side.erase(it);
            if (m.reset) continue; // rows are rebuilt below
            b.count = static_cast<uint32_t>(static_cast<int64_t>(b.count) + (ch.size > 0) - (old > 0));
            if (binUnits_ == 1) b.sum = ch.size;
            else {
                const long double delta = static_cast<long double>(ch.size) - old;
                b.sum = b.count == 0 ? 0.0L : std::max(0.0L, b.sum + delta);
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
                    b.sum = binUnits_ == 1 ? static_cast<long double>(size) : b.sum + size;
                }
        }
        if (observed_ && !m.observed && !damaged_) covered += t - since;
        if (!observed_ && m.observed) since = t;
        observed_ = m.observed;
        if (observed_ && !damaged_) {
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
    if (observed_ && !damaged_) covered += end - since;
    for (auto &[k, b] : bins_)
        if (b.count > 0) at(k);
    rows.reserve(touched_.size());
    for (const auto &k : touched_) {
        Bin &b = bins_[k];
        accrue(b, end);
        const long double last = b.count > 0 ? b.sum : 0;
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
