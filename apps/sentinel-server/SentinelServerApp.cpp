#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QTimer>

SentinelServerApp::SentinelServerApp(QObject* parent) 
    : QObject(parent) 
{
}

SentinelServerApp::~SentinelServerApp() {
    if (m_marketDataCore) {
        m_marketDataCore->stop();
    }
}

bool SentinelServerApp::initialize() {
    try {
        sLog_App("Initializing Server Components...");

        // 1. Authenticator
        m_authenticator = std::make_unique<Authenticator>();
        
        // 2. Market Data Core
        m_marketDataCore = std::make_unique<MarketDataCoreEngine>(*m_authenticator);
        
        // 3. Server Data Model
        m_serverModel = std::make_unique<ServerDataModel>();

        // 4. Stream Server
        m_server = std::make_unique<SentinelStreamServer>(*m_serverModel, 8080);
        m_server->start();
        
        // Connect MarketDataCoreEngine -> ServerDataModel via queued invocations
        QPointer<ServerDataModel> modelPtr(m_serverModel.get());
        m_marketDataCore->onTrade([modelPtr](const Trade& trade) {
            if (!modelPtr) return;
            Trade tradeCopy = trade;
            QMetaObject::invokeMethod(modelPtr.data(), [modelPtr, tradeCopy]() mutable {
                if (!modelPtr) return;
                modelPtr->onTrade(tradeCopy);
            }, Qt::QueuedConnection);
        });
        m_marketDataCore->onLiveOrderBookLevelUpdates([modelPtr](const std::string& productId,
                                                                 const std::vector<BookLevelUpdate>& updates,
                                                                 int64_t exchangeMs) {
            if (!modelPtr) return;
            QString productIdQ = QString::fromStdString(productId);
            std::vector<BookLevelUpdate> updatesCopy = updates;
            QMetaObject::invokeMethod(modelPtr.data(), [modelPtr, productIdQ, updatesCopy = std::move(updatesCopy), exchangeMs]() mutable {
                if (!modelPtr) return;
                modelPtr->onLiveOrderBookLevelUpdates(productIdQ, updatesCopy, static_cast<qint64>(exchangeMs));
            }, Qt::QueuedConnection);
        });
        m_marketDataCore->onLiveOrderBookInitialized([modelPtr](const std::string& productId,
                                                                const std::vector<OrderBookLevel>& bids,
                                                                const std::vector<OrderBookLevel>& asks) {
            if (!modelPtr) return;
            QString productIdQ = QString::fromStdString(productId);
            std::vector<OrderBookLevel> bidsCopy = bids;
            std::vector<OrderBookLevel> asksCopy = asks;
            QMetaObject::invokeMethod(modelPtr.data(), [modelPtr, productIdQ, bidsCopy = std::move(bidsCopy), asksCopy = std::move(asksCopy)]() mutable {
                if (!modelPtr) return;
                modelPtr->onLiveOrderBookInitialized(productIdQ, bidsCopy, asksCopy);
            }, Qt::QueuedConnection);
        });
        
        // Wire up callbacks for logging
        m_marketDataCore->onConnectionStatus([](bool connected){
            sLog_App("MarketDataCore Connection: " << (connected ? "CONNECTED" : "DISCONNECTED"));
        });
        
        m_marketDataCore->onError([](const std::string& error){
            sLog_Error("MarketDataCore Error: " << error);
        });

        // Start connection
        m_marketDataCore->start();
        
        // Subscribe to BTC-USD by default for verification
        sLog_App("Subscribing to default symbol: BTC-USD");
        m_marketDataCore->subscribeToSymbols({"BTC-USD"});

        return true;
    } catch (const std::exception& e) {
        sLog_Error("Exception during initialization: " << e.what());
        return false;
    }
}
