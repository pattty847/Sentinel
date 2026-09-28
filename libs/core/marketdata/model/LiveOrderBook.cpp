#include "TradeData.h"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>
#include <span>
#include <bit>

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

    size_t size = static_cast<size_t>((max_price - min_price) / tick_size) + 1;

    m_bids.assign(size, 0.0);
    m_asks.assign(size, 0.0);
    m_bidPresent.assign((size + 63) / 64, 0);
    m_askPresent.assign((size + 63) / 64, 0);

    m_nonZeroBidCount = 0;
    m_nonZeroAskCount = 0;
    m_totalBidVolume = 0.0;
    m_totalAskVolume = 0.0;
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

    for (const auto& update : updates) {
        applyLevelLocked(update.isBid, update.price, update.quantity, outDeltas);
    }
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

void LiveOrderBook::applyLevelLocked(bool isBid,
                                     double price,
                                     double quantity,
                                     std::vector<BookDelta>* outDeltas) {
    if (price < m_min_price || price > m_max_price || m_tick_size <= 0.0) {
        return;
    }

    size_t index = price_to_index(price);
    auto& levels = isBid ? m_bids : m_asks;
    if (index >= levels.size()) {
        return;
    }

    double& slot = levels[index];
    const double previous = slot;
    const double newValue = quantity > 0.0 ? quantity : 0.0;

    if (previous == newValue) {
        return;
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

    size_t bidScanned = 0;
    if (m_nonZeroBidCount) {
        for (size_t word = m_bidPresent.size(); word-- > 0 && bidBuffer.size() < maxPerSide && bidScanned < maxScanWordsPerSide;) {
            ++bidScanned;
            uint64_t bits = m_bidPresent[word];
            while (bits && bidBuffer.size() < maxPerSide) {
                const unsigned bit = 63u - std::countl_zero(bits);
                const size_t index = word * 64 + bit;
                bidBuffer.emplace_back(static_cast<uint32_t>(index), m_bids[index]);
                bits &= ~(uint64_t{1} << bit);
            }
        }
    }
    size_t askScanned = 0;
    if (m_nonZeroAskCount) {
        for (size_t word = 0; word < m_askPresent.size() && askBuffer.size() < maxPerSide && askScanned < maxScanWordsPerSide; ++word) {
            ++askScanned;
            uint64_t bits = m_askPresent[word];
            while (bits && askBuffer.size() < maxPerSide) {
                const unsigned bit = std::countr_zero(bits);
                const size_t index = word * 64 + bit;
                askBuffer.emplace_back(static_cast<uint32_t>(index), m_asks[index]);
                bits &= bits - 1;
            }
        }
    }

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
    const double spanMax = maxPrice;
    const double spanMin = minPrice;

    for (size_t i = clampedEnd + 1; i-- > startIdx; ) {
        const double qty = m_bids[i];
        if (qty <= 0.0) {
            continue;
        }
        const double price = m_min_price + (static_cast<double>(i) * m_tick_size);
        if (price < spanMin || price > spanMax) {
            continue;
        }
        if (bestBid && *bestBid == 0.0) {
            *bestBid = price;
        }
        const int row = static_cast<int>(std::floor((spanMax - price) / rowTickSize));
        if (row >= 0 && row < static_cast<int>(rowValues.size())) {
            rowValues[static_cast<size_t>(row)] += qty;
        }
    }

    for (size_t i = startIdx; i <= clampedEnd; ++i) {
        const double qty = m_asks[i];
        if (qty <= 0.0) {
            continue;
        }
        const double price = m_min_price + (static_cast<double>(i) * m_tick_size);
        if (price < spanMin || price > spanMax) {
            continue;
        }
        if (bestAsk && *bestAsk == 0.0) {
            *bestAsk = price;
        }
        const int row = static_cast<int>(std::floor((spanMax - price) / rowTickSize));
        if (row >= 0 && row < static_cast<int>(rowValues.size())) {
            rowValues[static_cast<size_t>(row)] += qty;
        }
    }
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
    const double spanMax = maxPrice;
    const double spanMin = minPrice;

    for (size_t i = clampedEnd + 1; i-- > startIdx; ) {
        const double qty = m_bids[i];
        if (qty <= 0.0) {
            continue;
        }
        const double price = m_min_price + (static_cast<double>(i) * m_tick_size);
        if (price < spanMin || price > spanMax) {
            continue;
        }
        if (bestBid && *bestBid == 0.0) {
            *bestBid = price;
        }
        const int row = static_cast<int>(std::floor((spanMax - price) / rowTickSize));
        if (row >= 0 && row < static_cast<int>(bidRows.size())) {
            bidRows[static_cast<size_t>(row)] += qty;
        }
    }

    for (size_t i = startIdx; i <= clampedEnd; ++i) {
        const double qty = m_asks[i];
        if (qty <= 0.0) {
            continue;
        }
        const double price = m_min_price + (static_cast<double>(i) * m_tick_size);
        if (price < spanMin || price > spanMax) {
            continue;
        }
        if (bestAsk && *bestAsk == 0.0) {
            *bestAsk = price;
        }
        const int row = static_cast<int>(std::floor((spanMax - price) / rowTickSize));
        if (row >= 0 && row < static_cast<int>(askRows.size())) {
            askRows[static_cast<size_t>(row)] += qty;
        }
    }
}
