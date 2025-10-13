#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../dispatch/Channels.hpp"

class SubscriptionManager {
public:
    void setDesiredProducts(std::vector<std::string> products) {
        m_desired = std::move(products);
    }
    const std::vector<std::string>& desired() const { return m_desired; }

    std::vector<std::string> buildSubscribeMsgs(const std::string& jwt) const {
        return buildMsgs("subscribe", jwt);
    }
    std::vector<std::string> buildUnsubscribeMsgs(const std::string& jwt) const {
        return buildMsgs("unsubscribe", jwt);
    }

private:
    std::vector<std::string> m_desired;

    std::vector<std::string> buildMsgs(const std::string& type, const std::string& jwt) const {
        std::vector<std::string> out;
        if (m_desired.empty()) return out;
        // level2 (subscribe) + market_trades
        for (const char* channel : {ch::kL2Subscribe, ch::kTrades}) {
            nlohmann::json msg;
            msg["type"] = type;
            msg["product_ids"] = m_desired;
            msg["channel"] = channel;
            msg["jwt"] = jwt;
            out.emplace_back(msg.dump());
        }
        return out;
    }
};
