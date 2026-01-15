#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
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
        
        // 2. Data Cache
        m_dataCache = std::make_unique<DataCache>();

        // 3. Market Data Core
        m_marketDataCore = std::make_unique<MarketDataCore>(*m_authenticator, *m_dataCache);
        
        // 4. Server Data Model
        m_serverModel = std::make_unique<ServerDataModel>();

        // 5. Stream Server
        m_server = std::make_unique<SentinelStreamServer>(*m_serverModel, 8080);
        m_server->start();
        
        // Connect MarketDataCore -> ServerDataModel
        connect(m_marketDataCore.get(), &MarketDataCore::tradeReceived, 
                m_serverModel.get(), &ServerDataModel::onTrade);
        connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookLevelUpdates,
                m_serverModel.get(), &ServerDataModel::onLiveOrderBookLevelUpdates);
        connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookInitialized,
                m_serverModel.get(), &ServerDataModel::onLiveOrderBookInitialized);
        
        // Wire up signals for logging
        connect(m_marketDataCore.get(), &MarketDataCore::connectionStatusChanged, [](bool connected){
            sLog_App("MarketDataCore Connection: " << (connected ? "CONNECTED" : "DISCONNECTED"));
        });
        
        connect(m_marketDataCore.get(), &MarketDataCore::errorOccurred, [](const QString& error){
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
