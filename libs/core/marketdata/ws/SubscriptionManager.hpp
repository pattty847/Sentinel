#pragma once
#include <string>
#include <string_view>
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
        return buildSubscribeMsgs(m_desired, jwt);
    }
    // Frame scope is the requested delta, never the remaining desired set.
    std::vector<std::string> buildSubscribeMsgs(const std::vector<std::string>& products,
                                               const std::string& jwt, bool level2Only = false) const {
        return buildMsgs("subscribe", products, jwt, !level2Only, level2Only);
    }
    std::vector<std::string> buildUnsubscribeMsgs(const std::vector<std::string>& products,
                                                 const std::string& jwt, bool level2Only = false) const {
        return buildMsgs("unsubscribe", products, jwt, m_desired.empty() && !level2Only, level2Only);
    }

private:
    std::vector<std::string> m_desired;

    std::vector<std::string> buildMsgs(const std::string& type, const std::vector<std::string>& products,
                                       const std::string& jwt, bool heartbeats, bool level2Only) const {
        std::vector<std::string> out;
        if (products.empty()) return out;
        for (const char* channel : {ch::kL2Subscribe, ch::kTrades, ch::kHeartbeats}) {
            if (std::string_view(channel) == ch::kHeartbeats && !heartbeats) continue;
            if (std::string_view(channel) == ch::kTrades && level2Only) continue;
            nlohmann::json msg;
            msg["type"] = type;
            msg["channel"] = channel;
            // Coinbase heartbeats are connection-scoped; product_ids can suppress heartbeats.
            if (std::string_view(channel) != ch::kHeartbeats) {
                msg["product_ids"] = products;
            }
            if (!jwt.empty()) {
                msg["jwt"] = jwt;
            }
            out.emplace_back(msg.dump());
        }
        return out;
    }
};
