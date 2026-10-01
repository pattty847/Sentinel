#pragma once
#include "RawCapture.hpp"
#include <QCryptographicHash>
#include <map>

namespace sentinel::capture {
inline constexpr const char* RoutingId = "product-ranges-v2";
inline constexpr uint64_t MaxRoutingFrames = 65536;
inline constexpr int64_t RoutingIntervalNs = 60LL * 1000000000;

// A proof group can span ordinary storage blocks; raw-data flush/fsync cadence
// remains independent. No payload history is retained while accumulating it.
class RoutingBatch {
public:
    bool empty() const { return count == 0; }
    bool due(const Record& next) const;
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
// Exact frozen identity line for hashing (golden vectors cover this contract).
std::string frameIdentityLine(const Record& record, const nlohmann::json& identity);
void validateRange(const nlohmann::json& receipt, const std::vector<std::string>& products);
}
