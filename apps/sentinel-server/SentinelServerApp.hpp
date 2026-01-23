#pragma once
#include <QObject>
#include <QTcpServer>
#include <memory>
#include "../../libs/core/marketdata/MarketDataCoreEngine.hpp"
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
    std::unique_ptr<MarketDataCoreEngine> m_marketDataCore;
    std::unique_ptr<ServerDataModel> m_serverModel;
    std::unique_ptr<SentinelStreamServer> m_server;
    QTcpServer* m_healthServer = nullptr;
};

