#include "FlowBarBuilder.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace sentinel::flow {
namespace {
int64_t floorBucket(int64_t ms, int64_t width) {
    const auto q = ms / width;
    return (ms % width < 0 ? q - 1 : q) * width;
}
int64_t priceRow(double price, double tick) {
    const double scaled = price / tick;
    if (!std::isfinite(scaled) || scaled < static_cast<double>(INT64_MIN) ||
        scaled > static_cast<double>(INT64_MAX)) throw std::out_of_range("flow price row");
    return static_cast<int64_t>(std::llround(scaled));
}
std::optional<uint64_t> numericId(const std::string& id) {
    uint64_t n = 0;
    const auto result = std::from_chars(id.data(), id.data() + id.size(), n);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) return {};
    return n;
}
bool tradeLess(const Trade& a, const Trade& b) {
    if (a.exchangeMs != b.exchangeMs) return a.exchangeMs < b.exchangeMs;
    const auto ai = numericId(a.id), bi = numericId(b.id);
    if (ai && bi && *ai != *bi) return *ai < *bi;
    return a.id < b.id;
}
bool notableLess(const Notable& a, const Notable& b) {
    const auto an = a.notional, bn = b.notional;
    if (an != bn) return an > bn;
    if (a.exchangeMs != b.exchangeMs) return a.exchangeMs < b.exchangeMs;
    return a.tradeId < b.tradeId;
}
void pushNotable(std::vector<Notable>& out, Notable n) {
    out.push_back(std::move(n));
    std::sort(out.begin(), out.end(), notableLess);
    if (out.size() > NotableCapacity) out.resize(NotableCapacity);
}
}
FlowBarBuilder::FlowBarBuilder(int64_t timeframeMs, double nativeTick)
    : timeframeMs_(timeframeMs), nativeTick_(nativeTick) {
    if (timeframeMs <= 0 || !std::isfinite(nativeTick) || nativeTick <= 0)
        throw std::invalid_argument("invalid flow grid");
}
bool FlowBarBuilder::add(const Trade& t) {
    if (!std::isfinite(t.price) || !std::isfinite(t.size) || t.price <= 0 || t.size <= 0)
        throw std::invalid_argument("invalid flow trade");
    if (!t.id.empty() && idSet_.contains(t.id)) return false;
    const auto row = priceRow(t.price, nativeTick_);
    const auto start = floorBucket(t.exchangeMs, timeframeMs_);
    auto& bucket = buckets_[start];
    if (bucket.trades.empty()) bucket.minRow = bucket.maxRow = row;
    const auto minRow = std::min(row, bucket.minRow);
    const auto maxRow = std::max(row, bucket.maxRow);
    if (maxRow - minRow > INT16_MAX) throw std::out_of_range("flow row offset exceeds int16");
    if (published_.contains(start) && revisions_[start] == UINT32_MAX)
        throw std::overflow_error("flow revision overflow");
    bucket.trades.push_back(t);
    bucket.minRow = minRow; bucket.maxRow = maxRow;
    if (published_.contains(start)) {
        ++revisions_[start];
    }
    if (!t.id.empty()) {
        idSet_.insert(t.id);
        recentIds_.push_back(t.id);
        if (recentIds_.size() > 4096) {
            idSet_.erase(recentIds_.front());
            recentIds_.pop_front();
        }
    }
    return true;
}
std::vector<FlowBar> FlowBarBuilder::bars() const {
    std::vector<FlowBar> out;
    out.reserve(buckets_.size());
    for (const auto& [start, source] : buckets_) {
        auto trades = source.trades;
        std::sort(trades.begin(), trades.end(), tradeLess);
        FlowBar bar;
        bar.startMs = start; bar.timeframeMs = timeframeMs_; bar.nativeTick = nativeTick_;
        if (const auto it = revisions_.find(start); it != revisions_.end()) bar.revision = it->second;
        std::map<int64_t, Row> rows;
        bool first = true;
        for (const auto& t : trades) {
            const auto r = priceRow(t.price, nativeTick_);
            if (first) {
                bar.open = bar.high = bar.low = bar.close = t.price;
                bar.lowRow = r; first = false;
            } else {
                bar.high = std::max(bar.high, t.price);
                bar.low = std::min(bar.low, t.price);
                bar.close = t.price;
                bar.lowRow = std::min(bar.lowRow, r);
            }
            bar.baseVolume += t.size;
            bar.quoteVolume += t.price * t.size;
            ++bar.count;
            auto& row = rows[r];
            if (t.side == Side::Buy) {
                row.askVol += t.size; ++row.askCount; bar.buyVol += t.size;
            } else if (t.side == Side::Sell) {
                row.bidVol += t.size; ++row.bidCount; bar.sellVol += t.size;
            }
        }
        for (auto& [r, row] : rows) {
            const auto offset = r - bar.lowRow;
            if (offset > INT16_MAX) throw std::out_of_range("flow row offset exceeds int16");
            row.offset = static_cast<int16_t>(offset);
            bar.maxRowVol = std::max(bar.maxRowVol, row.bidVol + row.askVol);
            bar.rows.push_back(row);
        }
        bar.delta = bar.buyVol - bar.sellVol;
        for (size_t i = 0; i < trades.size();) {
            const auto& firstTrade = trades[i];
            Notable n{firstTrade.id, firstTrade.exchangeMs, firstTrade.price,
                      firstTrade.size, firstTrade.price * firstTrade.size,
                      firstTrade.price, firstTrade.price, firstTrade.side, 1};
            std::set<double> prices{firstTrade.price};
            size_t j = i + 1;
            auto previousId = numericId(firstTrade.id);
            while (j < trades.size()) {
                const auto nextId = numericId(trades[j].id);
                if (!previousId || !nextId || *previousId == UINT64_MAX ||
                    *nextId != *previousId + 1 || trades[j].exchangeMs != n.exchangeMs ||
                    trades[j].side != n.side) break;
                n.size += trades[j].size;
                n.notional += trades[j].price * trades[j].size;
                n.minPrice = std::min(n.minPrice, trades[j].price);
                n.maxPrice = std::max(n.maxPrice, trades[j].price);
                prices.insert(trades[j].price);
                previousId = nextId;
                ++j;
            }
            n.sweepLevels = static_cast<uint16_t>(std::min<size_t>(prices.size(), UINT16_MAX));
            pushNotable(bar.notable, std::move(n));
            i = j;
        }
        out.push_back(std::move(bar));
        published_.insert(start);
    }
    return out;
}
void FlowBarBuilder::clear() {
    buckets_.clear(); recentIds_.clear(); idSet_.clear(); published_.clear(); revisions_.clear();
}
} // namespace sentinel::flow
