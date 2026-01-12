#pragma once
#include <QObject>
#include <memory>
#include "../../libs/core/marketdata/MarketDataCore.hpp"
#include "../../libs/core/marketdata/cache/DataCache.hpp"
#include "../../libs/core/marketdata/auth/Authenticator.hpp"
#include "../../libs/core/servermodel/ServerDataModel.hpp"
#include "../../libs/core/protocol/SentinelStreamServer.hpp"

class SentinelServerApp : public QObject {
    Q_OBJECT
public:
    explicit SentinelServerApp(QObject* parent = nullptr);
    ~SentinelServerApp();

    bool initialize();

private:
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DataCache> m_dataCache;
    std::unique_ptr<MarketDataCore> m_marketDataCore;
    std::unique_ptr<ServerDataModel> m_serverModel;
    std::unique_ptr<SentinelStreamServer> m_server;
};

