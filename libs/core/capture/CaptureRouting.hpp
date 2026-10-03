#pragma once
#include "RawCapture.hpp"
#include <QCryptographicHash>
#include <map>

namespace sentinel::capture {
inline constexpr const char* RoutingId = "product-ranges-v2";
inline constexpr uint64_t MaxRoutingFrames = 65536;
// The v2 writer closed a range at most this long after its first frame
// (historic contract of the 2026-09-30..10-02 archive; the writer is removed).
inline constexpr int64_t RoutingIntervalNs = 60LL * 1000000000;

// RAWL2 v2 is read-only: the verifier rebuilds a range receipt from recovered
// raw frames with this and compares it to the stored one. A proof group can span
// ordinary storage blocks. No payload history is retained while accumulating it.
class RoutingBatch {
public:
    bool empty() const { return count == 0; }
    void add(const Record& record, const nlohmann::json& identity);
    nlohmann::json receipt(const std::string& symbol) const;
    void clear();
    Stamp first{}, last{};
    uint64_t connection = 0, count = 0, bytes = 0, firstSeq = 0, lastSeq = 0, gaps = 0;
private:
    struct Counts { uint64_t frames = 0, bytes = 0; };
    std::map<std::string, Counts> own, destinations;
    QCryptographicHash digest{QCryptographicHash::Sha256};
};
// Optional verifier extraction uses the routing SAX state machine, retaining only
// this product's trade scalars from one bounded raw frame (never an L2 DOM).
struct CapturedTrade { std::string id, size, side, time; };
struct CapturedTradeEvent { std::string type; std::vector<CapturedTrade> trades; };
std::vector<CapturedTradeEvent> parseTradeEvents(std::string_view payload,
    const std::vector<std::string>& products, const std::string& symbol, nlohmann::json* envelope = nullptr);
// Best-effort header peek: stops at channel; never hashes or sorts destinations.
std::optional<std::string> peekFrameChannel(std::string_view payload);
// Exact frozen identity line for hashing (golden vectors cover this contract).
std::string frameIdentityLine(const Record& record, const nlohmann::json& identity);
void validateRange(const nlohmann::json& receipt, const std::vector<std::string>& products);
}
