#include "TradeData.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <bit>

namespace {

void appendSnapshotSide(const std::vector<double>& levels, double minPrice, double tickSize,
                        std::vector<OrderBookLevel>& out) {
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] > 0.0) out.push_back({minPrice + double(i) * tickSize, levels[i]});
    }
}

size_t captureSide(bool isBid, const std::vector<uint64_t>& present,
                   const std::vector<double>& levels,
                   std::vector<std::pair<uint32_t, double>>& out,
                   size_t maxPerSide, size_t maxScanWords) {
    size_t scanned = 0;
    while (scanned < present.size() && out.size() < maxPerSide && scanned < maxScanWords) {
        const size_t word = isBid ? present.size() - 1 - scanned : scanned;
        ++scanned;
        uint64_t bits = present[word];
        while (bits && out.size() < maxPerSide) {
            const unsigned bit = isBid ? 63u - std::countl_zero(bits) : std::countr_zero(bits);
            const size_t index = word * 64 + bit;
            out.emplace_back(static_cast<uint32_t>(index), levels[index]);
            if (isBid) bits &= ~(uint64_t{1} << bit);
            else bits &= bits - 1;
        }
    }
    return scanned;
}

void accumulateSide(bool isBid, const std::vector<double>& levels,
                    size_t startIdx, size_t endIdx, double minPrice, double tickSize,
                    double spanMin, double spanMax, double rowTickSize,
                    std::vector<double>& rows, double* bestPrice) {
    for (size_t offset = 0; offset <= endIdx - startIdx; ++offset) {
        const size_t i = isBid ? endIdx - offset : startIdx + offset;
        const double qty = levels[i];
        if (qty <= 0.0) continue;
        const double price = minPrice + (static_cast<double>(i) * tickSize);
        if (price < spanMin || price > spanMax) continue;
        if (bestPrice && *bestPrice == 0.0) *bestPrice = price;
        const int row = static_cast<int>(std::floor((spanMax - price) / rowTickSize));
        if (row >= 0 && row < static_cast<int>(rows.size())) {
            rows[static_cast<size_t>(row)] += qty;
        }
    }
}

} // namespace

void LiveOrderBook::initialize(double min_price, double max_price, double tick_size) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_tick_size = tick_size;
    m_min_price = min_price;
    m_max_price = max_price;

    if (m_tick_size <= 0) return;

    // Buckets sit on the tick grid (a multiple of tick_size), so bucket prices are
    // clean ($x.x0 for a $0.10 tick) instead of inheriting the band edge's offset.
    m_min_price = std::floor(min_price / tick_size) * tick_size;
    m_max_price = std::ceil(max_price / tick_size) * tick_size;
    min_price = m_min_price;
    max_price = m_max_price;

    size_t size = static_cast<size_t>(std::llround((max_price - min_price) / tick_size)) + 1;

    m_bids.assign(size, 0.0);
    m_asks.assign(size, 0.0);
    m_bidPresent.assign((size + 63) / 64, 0);
    m_askPresent.assign((size + 63) / 64, 0);

    m_nonZeroBidCount = 0;
    m_nonZeroAskCount = 0;
    m_totalBidVolume = 0.0;
    m_totalAskVolume = 0.0;
    ++m_stateVersion;
}

void LiveOrderBook::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bids.clear();
    m_asks.clear();
    m_bidPresent.clear();
    m_askPresent.clear();
    m_min_price = m_max_price = m_tick_size = 0.0;
    m_nonZeroBidCount = m_nonZeroAskCount = 0;
    m_totalBidVolume = m_totalAskVolume = 0.0;
    ++m_stateVersion;
}

LiveOrderBook::LevelsSnapshot LiveOrderBook::snapshotLevels() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    LevelsSnapshot out;
    out.tickSize = m_tick_size;
    out.version = m_stateVersion;
    appendSnapshotSide(m_bids, m_min_price, m_tick_size, out.bids);
    appendSnapshotSide(m_asks, m_min_price, m_tick_size, out.asks);
    return out;
}

void LiveOrderBook::applyUpdates(std::span<const BookLevelUpdate> updates,
                                 std::chrono::system_clock::time_point exchange_timestamp,
                                 std::vector<BookDelta>* outDeltas) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (updates.empty()) {
        return;
    }

    if (outDeltas) {
        outDeltas->clear();
        outDeltas->reserve(updates.size());
    }

    m_lastUpdate = exchange_timestamp;

    bool changed = false;
    for (const auto& update : updates)
        changed = applyLevelLocked(update.isBid, update.price, update.quantity, outDeltas) || changed;
    if (changed) ++m_stateVersion;
}

size_t LiveOrderBook::getBidCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroBidCount;
}

size_t LiveOrderBook::getAskCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroAskCount;
}

double LiveOrderBook::getBidVolume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalBidVolume;
}

double LiveOrderBook::getAskVolume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalAskVolume;
}

bool LiveOrderBook::isEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroBidCount == 0 && m_nonZeroAskCount == 0;
}

bool LiveOrderBook::applyLevelLocked(bool isBid,
                                     double price,
                                     double quantity,
                                     std::vector<BookDelta>* outDeltas) {
    if (price < m_min_price || price > m_max_price || m_tick_size <= 0.0) {
        return false;
    }

    size_t index = price_to_index(price);
    auto& levels = isBid ? m_bids : m_asks;
    if (index >= levels.size()) {
        return false;
    }

    double& slot = levels[index];
    const double previous = slot;
    const double newValue = quantity > 0.0 ? quantity : 0.0;

    if (previous == newValue) {
        return false;
    }

    auto& totalVolume = isBid ? m_totalBidVolume : m_totalAskVolume;
    auto& nonZeroLevels = isBid ? m_nonZeroBidCount : m_nonZeroAskCount;

    const bool wasNonZero = previous > 0.0;
    const bool isNonZero = newValue > 0.0;

    if (wasNonZero) {
        totalVolume -= previous;
    }
    if (isNonZero) {
        totalVolume += newValue;
    }

    if (wasNonZero != isNonZero) {
        auto& present = isBid ? m_bidPresent : m_askPresent;
        const uint64_t bit = uint64_t{1} << (index % 64);
        if (isNonZero) present[index / 64] |= bit;
        else present[index / 64] &= ~bit;
        if (isNonZero) {
            ++nonZeroLevels;
        } else if (nonZeroLevels > 0) {
            --nonZeroLevels;
        }
    }

    slot = newValue;

    if (totalVolume < 0.0) {
        totalVolume = 0.0;
    }

    if (outDeltas) {
        outDeltas->push_back({static_cast<uint32_t>(index), static_cast<float>(newValue), isBid});
    }
    return true;
}

LiveOrderBook::DenseBookSnapshotView LiveOrderBook::captureDenseNonZero(
    std::vector<std::pair<uint32_t, double>>& bidBuffer,
    std::vector<std::pair<uint32_t, double>>& askBuffer,
    size_t maxPerSide, size_t maxScanWordsPerSide) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    bidBuffer.clear();
    askBuffer.clear();
    bidBuffer.reserve(std::min(maxPerSide, m_bids.size()));
    askBuffer.reserve(std::min(maxPerSide, m_asks.size()));

    const size_t bidScanned = m_nonZeroBidCount
        ? captureSide(true, m_bidPresent, m_bids, bidBuffer, maxPerSide, maxScanWordsPerSide) : 0;
    const size_t askScanned = m_nonZeroAskCount
        ? captureSide(false, m_askPresent, m_asks, askBuffer, maxPerSide, maxScanWordsPerSide) : 0;

    DenseBookSnapshotView view;
    view.minPrice = m_min_price;
    view.maxPrice = m_max_price;
    view.tickSize = m_tick_size;
    view.timestamp = m_lastUpdate;
    view.bidLevels = std::span<const std::pair<uint32_t, double>>(bidBuffer.data(), bidBuffer.size());
    view.askLevels = std::span<const std::pair<uint32_t, double>>(askBuffer.data(), askBuffer.size());
    view.scanLimited = (bidBuffer.size() < maxPerSide && m_nonZeroBidCount > bidBuffer.size() && bidScanned >= maxScanWordsPerSide) ||
                       (askBuffer.size() < maxPerSide && m_nonZeroAskCount > askBuffer.size() && askScanned >= maxScanWordsPerSide);
    return view;
}

void LiveOrderBook::accumulateRange(double minPrice,
                                    double maxPrice,
                                    double rowTickSize,
                                    std::vector<double>& rowValues,
                                    double* bestBid,
                                    double* bestAsk) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (rowTickSize <= 0.0 || maxPrice <= minPrice || rowValues.empty()) {
        return;
    }

    std::fill(rowValues.begin(), rowValues.end(), 0.0);
    if (bestBid) *bestBid = 0.0;
    if (bestAsk) *bestAsk = 0.0;

    const size_t startIdx = price_to_index(std::max(minPrice, m_min_price));
    const size_t endIdx = price_to_index(std::min(maxPrice, m_max_price));

    if (startIdx >= m_bids.size() || startIdx >= m_asks.size()) {
        return;
    }

    const size_t clampedEnd = std::min(endIdx, m_bids.size() - 1);
    if (clampedEnd < startIdx) return;
    accumulateSide(true, m_bids, startIdx, clampedEnd, m_min_price, m_tick_size,
                   minPrice, maxPrice, rowTickSize, rowValues, bestBid);
    accumulateSide(false, m_asks, startIdx, clampedEnd, m_min_price, m_tick_size,
                   minPrice, maxPrice, rowTickSize, rowValues, bestAsk);
}

void LiveOrderBook::accumulateRangeSplit(double minPrice,
                                         double maxPrice,
                                         double rowTickSize,
                                         std::vector<double>& bidRows,
                                         std::vector<double>& askRows,
                                         double* bestBid,
                                         double* bestAsk) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (rowTickSize <= 0.0 || maxPrice <= minPrice || bidRows.empty() || askRows.empty()) {
        return;
    }

    std::fill(bidRows.begin(), bidRows.end(), 0.0);
    std::fill(askRows.begin(), askRows.end(), 0.0);
    if (bestBid) *bestBid = 0.0;
    if (bestAsk) *bestAsk = 0.0;

    const size_t startIdx = price_to_index(std::max(minPrice, m_min_price));
    const size_t endIdx = price_to_index(std::min(maxPrice, m_max_price));

    if (startIdx >= m_bids.size() || startIdx >= m_asks.size()) {
        return;
    }

    const size_t clampedEnd = std::min(endIdx, m_bids.size() - 1);
    if (clampedEnd < startIdx) return;
    accumulateSide(true, m_bids, startIdx, clampedEnd, m_min_price, m_tick_size,
                   minPrice, maxPrice, rowTickSize, bidRows, bestBid);
    accumulateSide(false, m_asks, startIdx, clampedEnd, m_min_price, m_tick_size,
                   minPrice, maxPrice, rowTickSize, askRows, bestAsk);
}
