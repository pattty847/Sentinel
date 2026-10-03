#include "mainwindow/SymbolSubscriptionManager.hpp"
#include <gtest/gtest.h>

using Action = SymbolSubscriptionManager::Action;

TEST(SymbolSubscriptionManager, TenSwitchesReleaseOnlyAfterAcknowledgment) {
    SymbolSubscriptionManager leases;
    QString active;
    QSet<QString> server;
    for (int i = 0; i < 10; ++i) {
        const QString next = QString("SYM%1-USD").arg(i);
        auto actions = leases.request("main", next);
        ASSERT_EQ(actions.size(), 1);
        EXPECT_EQ(actions[0].kind, Action::Subscribe);
        server.insert(actions[0].symbol);
        EXPECT_LE(server.size(), 2); // overlap exists until admission completes
        if (i) EXPECT_TRUE(server.contains(active));
        actions = leases.acknowledged(next.toLower());
        ASSERT_EQ(actions.size(), i ? 2 : 1);
        EXPECT_EQ(actions[0].kind, Action::Activate);
        active = actions[0].symbol;
        if (i) {
            EXPECT_EQ(actions[1].kind, Action::Unsubscribe);
            server.remove(actions[1].symbol);
        }
        EXPECT_EQ(server, QSet<QString>{active});
    }
}

TEST(SymbolSubscriptionManager, RefusalKeepsOldChartAndLease) {
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    leases.acknowledged("BTC-USD");
    const auto request = leases.request("main", "ETH-USD");
    ASSERT_EQ(request.size(), 1);
    EXPECT_EQ(request[0].kind, Action::Subscribe);
    const auto rejected = leases.refused("eth-usd");
    ASSERT_EQ(rejected.size(), 1);
    EXPECT_EQ(rejected[0].kind, Action::Refused);
    EXPECT_EQ(leases.held("main"), "BTC-USD");
    EXPECT_EQ(leases.heldSymbols(), QSet<QString>{"BTC-USD"});
}

TEST(SymbolSubscriptionManager, RefusedFirstSelectionIsNotRetriedOnReconnect) {
    SymbolSubscriptionManager leases;
    leases.request("main", "NEW-USD");
    const auto refused = leases.refused("new-usd");
    ASSERT_EQ(refused.size(), 1);
    EXPECT_EQ(refused[0].kind, Action::Refused);
    EXPECT_TRUE(leases.heldSymbols().isEmpty());
    EXPECT_TRUE(leases.reconnect().isEmpty());
}

TEST(SymbolSubscriptionManager, AnotherDockRetainsTheSymbol) {
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    leases.acknowledged("BTC-USD");
    auto actions = leases.request("chart2", "btc-usd");
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions[0].kind, Action::Activate); // shared server subscription
    leases.request("main", "ETH-USD");
    actions = leases.acknowledged("ETH-USD");
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions[0].kind, Action::Activate); // BTC is still held
    EXPECT_EQ(leases.heldSymbols(), (QSet<QString>{"BTC-USD", "ETH-USD"}));
    actions = leases.release("chart2");
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions[0].kind, Action::Unsubscribe);
    EXPECT_EQ(actions[0].symbol, "BTC-USD");
}

TEST(SymbolSubscriptionManager, ReconnectOnlyRequestsHeldSet) {
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    leases.acknowledged("BTC-USD");
    leases.request("chart2", "ETH-USD");
    leases.acknowledged("ETH-USD");
    leases.request("main", "REFUSED-USD"); // unacknowledged switch
    const auto actions = leases.reconnect();
    QSet<QString> resent;
    for (const auto& action : actions) {
        EXPECT_EQ(action.kind, Action::Subscribe);
        resent.insert(action.symbol);
    }
    EXPECT_EQ(resent, (QSet<QString>{"BTC-USD", "ETH-USD"}));
    EXPECT_EQ(leases.held("main"), "BTC-USD");
    EXPECT_TRUE(leases.acknowledged("REFUSED-USD").isEmpty());
}

TEST(SymbolSubscriptionManager, RefusedReconnectCanRetryTheHeldSymbol) {
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    leases.acknowledged("BTC-USD");
    const auto reconnect = leases.reconnect();
    ASSERT_EQ(reconnect.size(), 1);
    leases.refused("btc-usd");
    EXPECT_EQ(leases.held("main"), "BTC-USD");
    const auto retry = leases.request("main", "BTC-USD");
    ASSERT_EQ(retry.size(), 1);
    EXPECT_EQ(retry[0].kind, Action::Subscribe);
    EXPECT_EQ(retry[0].symbol, "BTC-USD");
}

TEST(SymbolSubscriptionManager, SupersededAckReleasesUnusedSymbol) {
    SymbolSubscriptionManager leases;
    leases.request("main", "BTC-USD");
    leases.request("main", "ETH-USD");
    const auto stale = leases.acknowledged("btc-usd");
    ASSERT_EQ(stale.size(), 1);
    EXPECT_EQ(stale[0].kind, Action::Unsubscribe);
    EXPECT_EQ(stale[0].symbol, "BTC-USD");
    EXPECT_EQ(leases.acknowledged("ETH-USD")[0].kind, Action::Activate);
}
