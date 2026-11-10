#pragma once
#include "SentinelStreamProtocol.hpp"
#include "marketdata/model/TradeData.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace protocol::tradewire {
struct Row {
    std::string id;
    int64_t exchangeMs = 0;
    double price = 0, size = 0;
    AggressorSide side = AggressorSide::Unknown;
    uint32_t count = 1;
};
struct Page {
    uint64_t req = 0;
    std::string symbol, status, reason;
    int64_t startMs = 0, endMs = 0, coveredFromMs = 0, coveredToMs = 0;
    std::vector<Row> rows;
};
inline void put(std::string& out, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out.push_back(char(value >> (8 * i)));
}
inline uint64_t get(std::span<const uint8_t> bytes, size_t& pos, unsigned width) {
    if (width > bytes.size() - pos) throw std::invalid_argument("truncated trade frame");
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[pos++]) << (8 * i);
    return value;
}
inline bool validRow(const Row& row, int64_t from, int64_t to) {
    return row.id.size() <= SentinelProtocol::kTradeHistoryMaxIdBytes &&
        row.exchangeMs >= from && row.exchangeMs < to &&
        std::isfinite(row.price) && row.price > 0 && std::isfinite(row.size) && row.size > 0 &&
        (row.side == AggressorSide::Buy || row.side == AggressorSide::Sell || row.side == AggressorSide::Unknown) &&
        row.count > 0;
}
inline std::string encode(const Page& page) {
    if (page.rows.size() > SentinelProtocol::kTradeHistoryMaxTrades ||
        page.startMs < 0 || page.endMs < page.startMs ||
        page.coveredFromMs < page.startMs || page.coveredToMs > page.endMs ||
        page.coveredToMs < page.coveredFromMs ||
        (page.status != "complete" && page.status != "partial" &&
         page.status != "building" && page.status != "refused"))
        throw std::invalid_argument("invalid trade page");
    nlohmann::json header = {{"type", "trade_history_chunk"}, {"schema_version", 1},
        {"req", page.req}, {"symbol", page.symbol}, {"start_ms", page.startMs},
        {"end_ms", page.endMs}, {"status", page.status},
        {"coverage", {{"from_ms", page.coveredFromMs}, {"to_ms", page.coveredToMs}}},
        {"count", page.rows.size()}};
    if (!page.reason.empty()) header["reason"] = page.reason;
    const auto json = header.dump();
    size_t total = 12 + json.size();
    for (const auto& row : page.rows) {
        if (!validRow(row, page.startMs, page.endMs)) throw std::invalid_argument("invalid trade row");
        total += 31 + row.id.size();
        if (total > SentinelProtocol::kTradeHistoryMaxFrameBytes)
            throw std::length_error("trade history frame exceeds 48 MiB cap");
    }
    std::string out = SentinelProtocol::kTradeHistoryMagic;
    out.reserve(total);
    put(out, json.size(), 4);
    out += json;
    put(out, page.rows.size(), 4);
    for (const auto& row : page.rows) {
        put(out, row.id.size(), 2);
        out += row.id;
        put(out, std::bit_cast<uint64_t>(row.exchangeMs), 8);
        put(out, std::bit_cast<uint64_t>(row.price), 8);
        put(out, std::bit_cast<uint64_t>(row.size), 8);
        put(out, static_cast<uint8_t>(row.side), 1);
        put(out, row.count, 4);
    }
    return out;
}
inline Page decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12 || bytes.size() > SentinelProtocol::kTradeHistoryMaxFrameBytes ||
        !std::equal(bytes.begin(), bytes.begin() + 4, SentinelProtocol::kTradeHistoryMagic))
        throw std::invalid_argument("invalid trade frame");
    size_t pos = 4;
    const auto headerSize = get(bytes, pos, 4);
    if (headerSize > bytes.size() - pos || headerSize > 4096)
        throw std::invalid_argument("invalid trade header length");
    const auto header = nlohmann::json::parse(bytes.begin() + pos, bytes.begin() + pos + headerSize);
    pos += headerSize;
    if (header.at("type") != "trade_history_chunk" || header.at("schema_version") != 1 ||
        !header.at("req").is_number_unsigned() || !header.at("symbol").is_string() ||
        !header.at("count").is_number_unsigned())
        throw std::invalid_argument("invalid trade header");
    Page page;
    page.req = header.at("req").get<uint64_t>();
    page.symbol = header.at("symbol").get<std::string>();
    page.status = header.at("status").get<std::string>();
    page.reason = header.value("reason", "");
    page.startMs = header.at("start_ms").get<int64_t>();
    page.endMs = header.at("end_ms").get<int64_t>();
    page.coveredFromMs = header.at("coverage").at("from_ms").get<int64_t>();
    page.coveredToMs = header.at("coverage").at("to_ms").get<int64_t>();
    const auto count = get(bytes, pos, 4);
    if (count != header.at("count").get<uint64_t>() || count > SentinelProtocol::kTradeHistoryMaxTrades ||
        page.startMs < 0 || page.endMs < page.startMs ||
        page.coveredFromMs < page.startMs || page.coveredToMs > page.endMs ||
        page.coveredToMs < page.coveredFromMs ||
        (page.status != "complete" && page.status != "partial" &&
         page.status != "building" && page.status != "refused") ||
        (page.status == "refused" && count))
        throw std::invalid_argument("invalid trade page metadata");
    if (count > (bytes.size() - pos) / 31)
        throw std::invalid_argument("invalid trade row count");
    page.rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto idSize = get(bytes, pos, 2);
        if (idSize > SentinelProtocol::kTradeHistoryMaxIdBytes || idSize > bytes.size() - pos)
            throw std::invalid_argument("invalid trade id length");
        Row row;
        row.id.assign(reinterpret_cast<const char*>(bytes.data() + pos), idSize);
        pos += idSize;
        row.exchangeMs = std::bit_cast<int64_t>(get(bytes, pos, 8));
        row.price = std::bit_cast<double>(get(bytes, pos, 8));
        row.size = std::bit_cast<double>(get(bytes, pos, 8));
        const auto side = get(bytes, pos, 1);
        if (side > static_cast<uint8_t>(AggressorSide::Unknown)) throw std::invalid_argument("invalid trade side");
        row.side = static_cast<AggressorSide>(side);
        row.count = get(bytes, pos, 4);
        if (!validRow(row, page.startMs, page.endMs) ||
            (!page.rows.empty() && row.exchangeMs < page.rows.back().exchangeMs))
            throw std::invalid_argument("invalid trade row");
        page.rows.push_back(std::move(row));
    }
    if (pos != bytes.size()) throw std::invalid_argument("trailing trade bytes");
    return page;
}
} // namespace protocol::tradewire
