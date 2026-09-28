#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <vector>
#include "../../core/marketdata/MarketDataCoreEngine.hpp"

class MarketDataCoreQt : public QObject {
    Q_OBJECT

public:
    explicit MarketDataCoreQt(Authenticator& auth, QObject* parent = nullptr);
    ~MarketDataCoreQt() override;

    void start();
    void stop();

    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

signals:
    void tradeReceived(const Trade& trade);
    void liveOrderBookLevelUpdates(const QString& productId,
                                   const std::vector<BookLevelUpdate>& updates,
                                   qint64 exchangeMs);
    void liveOrderBookInitialized(const QString& productId,
                                  const std::vector<OrderBookLevel>& bids,
                                  const std::vector<OrderBookLevel>& asks);
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);
    void latencyReceived(int milliseconds);

private:
    void wireCallbacks();

    MarketDataCoreEngine m_core;
};
