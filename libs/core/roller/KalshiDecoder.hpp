#pragma once
// Offline Kalshi payload state. NO bids stay in NO pricing internally; all
// arithmetic is integer (1/10000 dollars, 1/100 contracts). Only callback output
// converts to binary64. Sequence is subscription-wide, including foreign books
// and control replies. Invalid books cannot apply deltas until a fresh snapshot.
#include "marketdata/model/TradeData.h"
#include "servermodel/BookRecorder.hpp"
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

namespace sentinel::roller {
class KalshiDecoder {
public:
    struct Result {
        enum class Kind { None, Snapshot, Update, Trade } kind = Kind::None;
        int64_t envelopeMs = 0;
        std::vector<recording::Level> levels;
        std::optional<::Trade> trade;
    };
    Result apply(const nlohmann::json&, const std::string& product, int64_t localMs);
    void invalidate() { valid_ = false; yes_.clear(); no_.clear(); }
    void reset() { *this = {}; }
    bool valid() const { return valid_; }
    nlohmann::json exportState() const;
    static KalshiDecoder fromState(const nlohmann::json&);
private:
    using Levels = std::map<int64_t, int64_t>;
    std::optional<int64_t> sid_, sequence_;
    bool valid_ = false, retired_ = false;
    Levels yes_, no_;
    void advance(const nlohmann::json&);
};
} // namespace sentinel::roller
