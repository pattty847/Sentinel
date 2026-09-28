#include "AgentApiSnapshots.hpp"
#include "../datasources/CandleSeriesBuffer.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include <cmath>

namespace AgentApi {
std::optional<CandleSnapshot> captureCandles(const CandleSeriesBuffer& buffer,
    const QString& symbol, qint64 startMs, qint64 endMs, qint64 timeframeMs,
    size_t limit, Metadata meta) {
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    bool hasMore = false;
    qint64 nextStartMs = 0;
    if (!buffer.getBoundedSlice(symbol, timeframeMs / 1000, startMs, endMs, limit, bars, hasMore, nextStartMs))
        return std::nullopt;
    CandleSnapshot out;
    out.meta = std::move(meta);
    out.meta.coverage = bars.empty() ? "unknown" : "partial";
    out.meta.truncated = hasMore;
    out.bars.reserve(bars.size());
    for (const auto& b : bars) {
        if (!std::isfinite(b.open) || !std::isfinite(b.high) || !std::isfinite(b.low) ||
            !std::isfinite(b.close) || !std::isfinite(b.volume)) continue;
        out.bars.push_back({b.timeStartMs, b.timeEndMs, b.open, b.high, b.low,
                            b.close, b.volume, b.isClosed, b.seq});
    }
    if (hasMore) out.nextStartMs = nextStartMs;
    return out;
}

BookSnapshot captureBook(const LiveOrderBook& book, size_t levels,
    std::optional<qint64> receivedAtMs, Metadata meta) {
    BookSnapshot out;
    out.meta = std::move(meta);
    out.receivedAtMs = receivedAtMs;
    std::vector<std::pair<uint32_t, double>> bids, asks;
    // At most 16,384 occupancy words (1,048,576 price slots) per side are examined.
    const auto view = book.captureDenseNonZero(bids, asks, levels, 16384);
    out.scanLimited = view.scanLimited;
    out.meta.truncated = view.scanLimited;
    if (view.tickSize > 0 && std::isfinite(view.minPrice) && std::isfinite(view.maxPrice)) {
        out.bandMin = view.minPrice;
        out.bandMax = view.maxPrice;
        for (const auto& [index, qty] : view.bidLevels) {
            const double price = view.minPrice + index * view.tickSize;
            if (std::isfinite(price) && std::isfinite(qty)) out.bids.push_back({price, qty});
        }
        for (const auto& [index, qty] : view.askLevels) {
            const double price = view.minPrice + index * view.tickSize;
            if (std::isfinite(price) && std::isfinite(qty)) out.asks.push_back({price, qty});
        }
    }
    if (!out.bids.empty()) out.bestBid = out.bids.front().price;
    if (!out.asks.empty()) out.bestAsk = out.asks.front().price;
    if (out.bestBid && out.bestAsk) out.spread = *out.bestAsk - *out.bestBid;
    out.meta.coverage = receivedAtMs ? "partial" : "unknown";
    return out;
}
} // namespace AgentApi
