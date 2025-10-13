#pragma once
#include <string>
#include <string_view>

namespace ch {
    inline constexpr const char* kTrades         = "market_trades";
    inline constexpr const char* kL2Subscribe    = "level2";
    inline constexpr const char* kL2Data        = "l2_data";
    inline constexpr const char* kSubscriptions = "subscriptions";
}

namespace side_norm {
    inline std::string normalize(std::string_view s) {
        if (s == "offer") return "ask";
        return std::string{s};
    }
}
