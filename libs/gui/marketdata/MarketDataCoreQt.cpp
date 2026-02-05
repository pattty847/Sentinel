#include "MarketDataCoreQt.hpp"
#include <QMetaObject>
#include <QMetaType>

MarketDataCoreQt::MarketDataCoreQt(Authenticator& auth, QObject* parent)
    : QObject(parent)
    , m_core(auth, ServerMdcConfig{})
{
    qRegisterMetaType<BookLevelUpdate>("BookLevelUpdate");
    qRegisterMetaType<std::vector<BookLevelUpdate>>("BookLevelUpdateVector");

    wireCallbacks();
}

MarketDataCoreQt::~MarketDataCoreQt() {
    m_core.stop();
}

void MarketDataCoreQt::start() {
    m_core.start();
}

void MarketDataCoreQt::stop() {
    m_core.stop();
}

void MarketDataCoreQt::subscribeToSymbols(const std::vector<std::string>& symbols) {
    m_core.subscribeToSymbols(symbols);
}

void MarketDataCoreQt::unsubscribeFromSymbols(const std::vector<std::string>& symbols) {
    m_core.unsubscribeFromSymbols(symbols);
}

void MarketDataCoreQt::wireCallbacks() {
    QPointer<MarketDataCoreQt> self(this);

    m_core.onTrade([self](const Trade& trade) {
        if (!self) return;
        Trade tradeCopy = trade;
        QMetaObject::invokeMethod(self.data(), [self, tradeCopy]() mutable {
            if (!self) return;
            emit self->tradeReceived(tradeCopy);
        }, Qt::QueuedConnection);
    });

    m_core.onLiveOrderBookLevelUpdates([self](const std::string& productId,
                                              const std::vector<BookLevelUpdate>& updates,
                                              int64_t exchangeMs) {
        if (!self) return;
        QString productIdQ = QString::fromStdString(productId);
        std::vector<BookLevelUpdate> updatesCopy = updates;
        QMetaObject::invokeMethod(self.data(), [self, productIdQ, updatesCopy = std::move(updatesCopy), exchangeMs]() mutable {
            if (!self) return;
            emit self->liveOrderBookLevelUpdates(productIdQ, updatesCopy, static_cast<qint64>(exchangeMs));
        }, Qt::QueuedConnection);
    });

    m_core.onLiveOrderBookInitialized([self](const std::string& productId,
                                             const std::vector<OrderBookLevel>& bids,
                                             const std::vector<OrderBookLevel>& asks) {
        if (!self) return;
        QString productIdQ = QString::fromStdString(productId);
        std::vector<OrderBookLevel> bidsCopy = bids;
        std::vector<OrderBookLevel> asksCopy = asks;
        QMetaObject::invokeMethod(self.data(), [self, productIdQ, bidsCopy = std::move(bidsCopy), asksCopy = std::move(asksCopy)]() mutable {
            if (!self) return;
            emit self->liveOrderBookInitialized(productIdQ, bidsCopy, asksCopy);
        }, Qt::QueuedConnection);
    });

    m_core.onConnectionStatus([self](bool connected) {
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, connected]() {
            if (!self) return;
            emit self->connectionStatusChanged(connected);
        }, Qt::QueuedConnection);
    });

    m_core.onError([self](const std::string& error) {
        if (!self) return;
        QString errorQ = QString::fromStdString(error);
        QMetaObject::invokeMethod(self.data(), [self, errorQ]() {
            if (!self) return;
            emit self->errorOccurred(errorQ);
        }, Qt::QueuedConnection);
    });

    m_core.onLatency([self](int latencyMs) {
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, latencyMs]() {
            if (!self) return;
            emit self->latencyReceived(latencyMs);
        }, Qt::QueuedConnection);
    });
}
