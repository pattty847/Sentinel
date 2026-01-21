#include "TradeData.h"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <span>

void LiveOrderBook::initialize(double min_price, double max_price, double tick_size) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_min_price = min_price;
    m_max_price = max_price;
    m_tick_size = tick_size;

    if (m_tick_size <= 0) return;

    size_t size = static_cast<size_t>((max_price - min_price) / tick_size) + 1;

    m_bids.assign(size, 0.0);
    m_asks.assign(size, 0.0);

    m_nonZeroBidCount = 0;
    m_nonZeroAskCount = 0;
    m_totalBidVolume = 0.0;
    m_totalAskVolume = 0.0;

    sLog_App(QString("O(1) LiveOrderBook initialized for %1 with size %2 (%3 -> %4 @ %5)")
              .arg(QString::fromStdString(m_productId)).arg(size)
              .arg(m_min_price).arg(m_max_price).arg(m_tick_size));
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
    size_t maxPerSide) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    bidBuffer.clear();
    askBuffer.clear();
    bidBuffer.reserve(std::min(maxPerSide, m_bids.size()));
    askBuffer.reserve(std::min(maxPerSide, m_asks.size()));

    for (size_t i = m_bids.size(); i-- > 0 && bidBuffer.size() < maxPerSide; ) {
        double qty = m_bids[i];
        if (qty > 0.0) {
            bidBuffer.emplace_back(static_cast<uint32_t>(i), qty);
        }
    }

    for (size_t i = 0; i < m_asks.size() && askBuffer.size() < maxPerSide; ++i) {
        double qty = m_asks[i];
        if (qty > 0.0) {
            askBuffer.emplace_back(static_cast<uint32_t>(i), qty);
        }
    }

    DenseBookSnapshotView view;
    view.minPrice = m_min_price;
    view.tickSize = m_tick_size;
    view.timestamp = m_lastUpdate;
    view.bidLevels = std::span<const std::pair<uint32_t, double>>(bidBuffer.data(), bidBuffer.size());
    view.askLevels = std::span<const std::pair<uint32_t, double>>(askBuffer.data(), askBuffer.size());
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
