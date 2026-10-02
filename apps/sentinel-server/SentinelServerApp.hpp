#pragma once
#include <QObject>
#include <memory>
#include <unordered_set>
#include "../../libs/core/marketdata/MarketDataFeeds.hpp"
#include "../../libs/core/marketdata/auth/Authenticator.hpp"
#include "../../libs/core/servermodel/ServerDataModel.hpp"
#include "../../libs/core/protocol/SentinelStreamServer.hpp"
#include "../../libs/core/config/ConfigTypes.hpp"
#include "../../libs/core/metrics/MetricsHttpServer.hpp"
#include "../../libs/core/metrics/MetricsRegistry.hpp"

class SentinelServerApp : public QObject {
    Q_OBJECT
public:
    explicit SentinelServerApp(const ServerConfig& config, QObject* parent = nullptr);
    ~SentinelServerApp();

    bool initialize();
    // True once the recorder started (recording.enabled and a usable directory).
    bool recording() const { return m_serverModel && m_serverModel->recordingDir().has_value(); }

private:
    ServerConfig m_serverConfig;
    // Declared first: outlives every component whose samplers it holds.
    sentinel::metrics::MetricsRegistry m_metrics;
    sentinel::metrics::Gauge* m_wsLatencyMs = nullptr;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<MarketDataFeeds> m_marketDataCore;
    std::unique_ptr<ServerDataModel> m_serverModel;
    std::unique_ptr<SentinelStreamServer> m_server;
    // Declared last: destroyed first, so no scrape renders a destroyed component.
    std::unique_ptr<sentinel::metrics::MetricsHttpServer> m_httpServer;
    std::unordered_set<std::string> m_defaultSymbols;
};
