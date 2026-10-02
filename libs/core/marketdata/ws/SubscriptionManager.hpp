#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "../dispatch/Channels.hpp"

class SubscriptionManager {
public:
    static std::vector<std::string> buildSubscribeMsgs(const std::string& product, const std::string& jwt = {}) {
        std::vector<std::string> out;
        for (const char* channel : {ch::kL2Subscribe, ch::kTrades, ch::kHeartbeats}) {
            nlohmann::json msg{{"type", "subscribe"}, {"channel", channel}};
            if (std::string_view(channel) != ch::kHeartbeats) msg["product_ids"] = {product};
            if (!jwt.empty()) msg["jwt"] = jwt;
            out.push_back(msg.dump());
        }
        return out;
    }
};
