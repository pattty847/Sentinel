#pragma once
#include "AgentApiTypes.hpp"

class CandleSeriesBuffer;
class LiveOrderBook;

namespace AgentApi {
std::optional<CandleSnapshot> captureCandles(const CandleSeriesBuffer& buffer,
    const QString& symbol, qint64 startMs, qint64 endMs, qint64 timeframeMs,
    size_t limit, Metadata meta);
BookSnapshot captureBook(const LiveOrderBook& book, size_t levels,
    std::optional<qint64> receivedAtMs, Metadata meta);
} // namespace AgentApi
