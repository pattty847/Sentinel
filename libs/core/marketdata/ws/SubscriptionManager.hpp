#pragma once
#include <string>
#include <vector>

class SubscriptionManager {
public:
    void setDesiredProducts(std::vector<std::string> products) {
        m_desired = std::move(products);
    }
    const std::vector<std::string>& desired() const { return m_desired; }

    std::vector<std::string> buildSubscribeMsgs(const std::string& jwt) const;
    std::vector<std::string> buildUnsubscribeMsgs(const std::string& jwt) const;

private:
    std::vector<std::string> m_desired;
};


