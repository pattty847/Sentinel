#pragma once
#include <QObject>
#include "roller/ShadowRoller.hpp"
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
    friend struct ShadowServerTestAccess;
public:
    explicit SentinelServerApp(const ServerConfig& config, QObject* parent = nullptr);
    ~SentinelServerApp();

    bool initialize();
    // True once recording is served: the primary recorder started, or
    // (recording.source: roller) the roller supervisor attached to the model.
    bool recording() const { return m_serverModel && m_serverModel->recordingAvailable(); }
    // --require-recording precheck: the reason the root that this config would
    // serve is unusable (disabled, unmounted, not a writable directory), or empty.
    static std::string requiredRecordingProblem(ServerConfig& config);

private:
    // False only when the journal live feed cannot start (no roller).
    bool startShadow(const std::vector<std::string>& symbols);
    void wireEngineFeed(const std::vector<std::string>& symbolList);
    void wireJournalFeed();
    sentinel::roller::ModelSink journalModelSink();
    ServerConfig m_serverConfig;
    // Declared first: outlives every component whose samplers it holds.
    sentinel::metrics::MetricsRegistry m_metrics;
    sentinel::metrics::Gauge* m_wsLatencyMs = nullptr;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<MarketDataFeeds> m_marketDataCore;
    std::unique_ptr<ServerDataModel> m_serverModel;
    std::unique_ptr<SentinelStreamServer> m_server;
    std::unique_ptr<sentinel::roller::ShadowRoller> m_shadowRoller;
    // Declared last: destroyed first, so no scrape renders a destroyed component.
    std::unique_ptr<sentinel::metrics::MetricsHttpServer> m_httpServer;
    std::unordered_set<std::string> m_defaultSymbols;
};
