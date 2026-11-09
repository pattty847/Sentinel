#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sentinel::flow {

enum class Side : uint8_t { Unknown, Buy, Sell };

struct Trade {
    std::string id;
    int64_t exchangeMs = 0;
    double price = 0;
    double size = 0;
    Side side = Side::Unknown;
};

struct Row {
    int16_t offset = 0; // From lowRow, inclusive.
    int64_t bidVol = 0, askVol = 0; // baseScale units.
    uint32_t bidCount = 0, askCount = 0;
    bool operator==(const Row&) const = default;
};

struct Notable {
    std::string tradeId;
    int64_t exchangeMs = 0;
    double price = 0;
    int64_t size = 0, notional = 0; // baseScale and quoteScale units.
    double minPrice = 0, maxPrice = 0;
    Side side = Side::Unknown;
    uint16_t sweepLevels = 1;
    bool operator==(const Notable&) const = default;
};

struct FlowBar {
    int64_t startMs = 0;
    int64_t timeframeMs = 0;
    int64_t lowRow = 0;
    double nativeTick = 0;
    int64_t priceScale = 0, baseScale = 0, quoteScale = 0;
    double open = 0, high = 0, low = 0, close = 0;
    int64_t baseVolume = 0, quoteVolume = 0;
    uint64_t count = 0;
    int64_t buyVol = 0, sellVol = 0, delta = 0, maxRowVol = 0;
    uint32_t revision = 0;
    std::vector<Row> rows;
    std::vector<Notable> notable; // At most eight, descending notional.
    bool operator==(const FlowBar&) const = default;
};

constexpr size_t NotableCapacity = 8;
} // namespace sentinel::flow
