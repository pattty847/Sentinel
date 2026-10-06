#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QStringList>
#include "metrics/ProcessMetrics.hpp"
#include "servermodel/RecordingDir.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <unistd.h>

namespace {
template <typename T, typename Fn>
void safeInvoke(QPointer<T> ptr, Fn&& fn) {
    QMetaObject::invokeMethod(ptr.data(), [ptr, fn = std::forward<Fn>(fn)]() mutable {
        if (!ptr) {
            return;
        }
        fn(*ptr);
    }, Qt::QueuedConnection);
}
} // namespace

SentinelServerApp::SentinelServerApp(const ServerConfig& config, QObject* parent) 
    : QObject(parent)
    , m_serverConfig(config)
{
}

SentinelServerApp::~SentinelServerApp() {
    if (m_server) {
        m_server->stop(); // admission callback uses m_marketDataCore
    }
    if (m_marketDataCore) {
        m_marketDataCore->stop();
    }
}

bool SentinelServerApp::initialize() {
    try {
        sLog_App("Initializing server components: streamPort=" << m_serverConfig.streamPort
                 << " mdcHost=" << m_serverConfig.mdc.host
                 << " defaultSymbols=" << m_serverConfig.defaultSymbols.size());

        // GET /ping and GET /metrics on 127.0.0.1 (ops/monitoring/README.md).
        sentinel::metrics::registerProcessMetrics(m_metrics);
        m_wsLatencyMs = &m_metrics.gauge("sentinel_mdc_ws_latency_ms",
                                         "Latest Coinbase WebSocket latency (server time minus exchange timestamp).");
        m_httpServer = std::make_unique<sentinel::metrics::MetricsHttpServer>(m_metrics);
        const QByteArray portEnv = qgetenv("SENTINEL_HEALTH_PORT");
        bool ok = false;
        const int port = portEnv.toInt(&ok);
        const quint16 healthPort = (ok && port > 0) ? static_cast<quint16>(port) : 8090;
        if (m_httpServer->listen(healthPort)) {
            sLog_App("Health and metrics endpoint listening on 127.0.0.1:" << healthPort << " (/ping, /metrics)");
        } else {
            sLog_Warning("Health endpoint failed to bind on 127.0.0.1:" << healthPort
                         << " error=" << m_httpServer->errorString());
        }

        // 1. Authenticator (optional: public channels work without key.json)
        m_authenticator = std::make_unique<Authenticator>();
        // Only send JWT when we have credentials and config enables it (user/futures channels need auth)
        m_serverConfig.mdc.useJwt = m_authenticator->hasCredentials() && m_serverConfig.mdc.useJwt;
        sLog_App("Authenticator: credentials=" << m_authenticator->hasCredentials()
                 << " useJwt=" << m_serverConfig.mdc.useJwt);

        // 2. Market Data Core
        try {
            if (m_serverConfig.mdc.maxConnections < 1) throw std::invalid_argument("max_connections must be at least 1");
            MarketDataFeeds::Options options;
            options.maxConnections = size_t(m_serverConfig.mdc.maxConnections);
            m_marketDataCore = std::make_unique<MarketDataFeeds>(*m_authenticator, m_serverConfig.mdc, options);
        } catch (const std::exception& e) {
            sLog_Error("MarketDataFeeds init failed: " << e.what());
            return false;
        }
        
        // 3. Server Data Model
        try {
        m_serverModel = std::make_unique<ServerDataModel>(m_serverConfig);
        } catch (const std::exception& e) {
            sLog_Error("ServerDataModel init failed: " << e.what());
            return false;
        }

        // 4. Stream Server
        try {
            quint16 streamPort = m_serverConfig.streamPort;
            m_server = std::make_unique<SentinelStreamServer>(*m_serverModel, *m_authenticator, m_serverConfig, streamPort);
            // Start only after callbacks and pinned feeds are installed below.
        } catch (const std::exception& e) {
            sLog_Error("SentinelStreamServer init failed: " << e.what());
            return false;
        }
        m_serverModel->registerMetrics(m_metrics);
        m_server->registerMetrics(m_metrics);
        m_metrics.gaugeFn("sentinel_stream_sessions", "Open client stream sessions.", {},
                          [this]() -> std::optional<double> { return double(m_server->sessionCount()); });
        
        // Connect MarketDataFeeds -> ServerDataModel via queued invocations
        QPointer<ServerDataModel> modelPtr(m_serverModel.get());
        // Lifecycle and data callbacks originate on the same I/O executor;
        // enqueue them to the model in that order, including rapid re-acquire.
        m_marketDataCore->onFeedLifecycle([modelPtr](const std::string& symbol, bool acquired) {
            const auto localMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            safeInvoke(modelPtr, [symbol, acquired, localMs](ServerDataModel& model) {
                if (acquired) model.acquireGuiFeed(symbol);
                else model.releaseGuiFeed(symbol, localMs);
            });
        });
        m_marketDataCore->onTrade([modelPtr](const Trade& trade) {
            Trade tradeCopy = trade;
            safeInvoke(modelPtr, [tradeCopy](ServerDataModel& model) mutable {
                model.onTrade(tradeCopy);
            });
        });
        m_marketDataCore->onLiveOrderBookLevelUpdates([modelPtr](const std::string& productId,
                                                                 const std::vector<BookLevelUpdate>& updates,
                                                                 int64_t exchangeMs) {
            QString productIdQ = QString::fromStdString(productId);
            std::vector<BookLevelUpdate> updatesCopy = updates;
            safeInvoke(modelPtr, [productIdQ, updatesCopy = std::move(updatesCopy), exchangeMs](ServerDataModel& model) mutable {
                model.onLiveOrderBookLevelUpdates(productIdQ, updatesCopy, static_cast<qint64>(exchangeMs));
            });
        });
        m_marketDataCore->onLiveOrderBookInitialized([modelPtr](const std::string& productId,
                                                                const std::vector<OrderBookLevel>& bids,
                                                                const std::vector<OrderBookLevel>& asks,
                                                                int64_t envelopeMs) {
            QString productIdQ = QString::fromStdString(productId);
            std::vector<OrderBookLevel> bidsCopy = bids;
            std::vector<OrderBookLevel> asksCopy = asks;
            safeInvoke(modelPtr, [productIdQ, bidsCopy = std::move(bidsCopy), asksCopy = std::move(asksCopy), envelopeMs](ServerDataModel& model) mutable {
                model.onLiveOrderBookInitialized(productIdQ, bidsCopy, asksCopy, static_cast<qint64>(envelopeMs));
            });
        });
        // Queued behind any updates already in flight, so ordering with the book stream holds.
        m_marketDataCore->onLiveOrderBookInvalidated([modelPtr](const std::string& productId,
                                                                const std::string& reason) {
            QString productIdQ = QString::fromStdString(productId);
            QString reasonQ = QString::fromStdString(reason);
            safeInvoke(modelPtr, [productIdQ, reasonQ](ServerDataModel& model) {
                model.onLiveOrderBookInvalidated(productIdQ, reasonQ);
            });
        });
        
        // Wire up callbacks for logging
        m_marketDataCore->onConnectionStatus([modelPtr](const std::string& product, bool connected){
            sLog_App("MarketDataCore Connection: product=" << product << " " << (connected ? "CONNECTED" : "DISCONNECTED"));
            safeInvoke(modelPtr, [product, connected](ServerDataModel& model) { model.onMarketDataConnectionChanged(product, connected); });
        });
        // The recorder dropped a book on its own: only a fresh snapshot resumes it.
        connect(m_serverModel.get(), &ServerDataModel::recordingResnapshotRequested, this,
                [this](const QString& symbol, const QString& reason) {
                    sLog_Warning("Recording resnapshot request: symbol=" << symbol << " reason=" << reason);
                    if (m_marketDataCore) m_marketDataCore->requestResnapshot(symbol.toStdString());
                });
        connect(m_serverModel.get(), &ServerDataModel::productMetadataRequested, this,
                [this](const QString& symbol, uint64_t lifetime) {
                    if (m_server) m_server->requestBookProductMetadata(symbol, lifetime);
                });
        
        m_marketDataCore->onError([](const std::string& product, const std::string& error){
            sLog_Error("MarketDataCore Error: product=" << product << " " << error);
        });

        m_marketDataCore->onLatency([this](int latencyMs) {
            // Coinbase WebSocket latency (server time - Coinbase timestamp) arrives
            // per book message: one always-on line per 10 s, every sample as a probe.
            sLog_DataN(10000, "Coinbase WebSocket latency: ms=" << latencyMs);
            sLog_Probe("ws.latency", "ms=" << latencyMs);
            m_wsLatencyMs->set(latencyMs);
            const auto now = std::chrono::steady_clock::now();
            if (m_server) {
                static int lastBroadcastMs = -1;
                static auto lastBroadcastTime = std::chrono::steady_clock::now();
                const auto sinceBroadcast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBroadcastTime).count();
                if (sinceBroadcast >= 1000 || std::abs(latencyMs - lastBroadcastMs) > 5) {
                    m_server->broadcastCoinbaseLatency(latencyMs);
                    lastBroadcastMs = latencyMs;
                    lastBroadcastTime = now;
                }
            }
        });

        m_server->setFeedAdmissionHandler([this](const std::string& symbol) {
            MarketDataFeeds::AddResult result;
            try {
                result = m_marketDataCore->add(symbol);
            } catch (const std::exception& e) {
                sLog_Error("Upstream feed admission threw: symbol=" << symbol << " error=" << e.what());
                return SentinelStreamServer::FeedAdmission::UpstreamUnavailable;
            }
            if (result == MarketDataFeeds::AddResult::CapacityExceeded ||
                result == MarketDataFeeds::AddResult::InvalidProduct) {
                sLog_Error("Upstream feed admission failed: symbol=" << symbol
                           << " result=" << (result == MarketDataFeeds::AddResult::CapacityExceeded
                               ? "CapacityExceeded" : "InvalidProduct"));
                return result == MarketDataFeeds::AddResult::CapacityExceeded
                    ? SentinelStreamServer::FeedAdmission::CapacityExceeded
                    : SentinelStreamServer::FeedAdmission::InvalidProduct;
            }
            return SentinelStreamServer::FeedAdmission::Accepted;
        });
        QObject::connect(m_server.get(), &SentinelStreamServer::clientSubscribed, this,
                         [this](const QString& symbol) {
                             if (!m_marketDataCore) {
                                 return;
                             }
                             sLog_Data("First client subscribed with upstream feed: symbol=" << symbol);
                         }, Qt::QueuedConnection);

        QObject::connect(m_server.get(), &SentinelStreamServer::clientUnsubscribed, this,
                         [this](const QString& symbol) {
                             if (!m_marketDataCore) {
                                 return;
                             }
                             const std::string native = symbol.toStdString();
                             if (m_defaultSymbols.find(native) != m_defaultSymbols.end()) {
                                 sLog_Data("Last client unsubscribed, keeping pinned default symbol upstream: symbol="
                                           << symbol);
                                 return;
                             }
                             if (!m_server->releaseIfNoSubscribers(native, [this, &native] {
                                     m_marketDataCore->remove(native);
                                 })) {
                                 sLog_Data("Queued upstream release skipped; subscribers returned: symbol=" << symbol);
                                 return;
                             }
                             sLog_Data("Last client unsubscribed, released upstream: symbol=" << symbol);
                         }, Qt::QueuedConnection);

        // Start connection
        m_marketDataCore->start();

        const auto normalizedSymbols = normalizedDefaultSymbols(m_serverConfig.defaultSymbols);
        std::vector<std::string> symbolList;
        symbolList.reserve(normalizedSymbols.size());
        for (const auto& sym : normalizedSymbols) {
            if (m_defaultSymbols.insert(sym).second) {
                symbolList.push_back(sym);
            }
        }

        for (const auto& symbol : symbolList) m_marketDataCore->add(symbol, true);
        sLog_Data("Server default subscribe: count=" << symbolList.size()
                  << " guiConnectionCap=" << m_serverConfig.mdc.maxConnections);
        // Before the stream server starts: a roller-served recording must be
        // attached before the first hello reports recording availability.
        startShadow(symbolList);
        m_server->start();
        return true;
    } catch (const std::exception& e) {
        sLog_Error("Exception during initialization: " << e.what());
        return false;
    }
}

void SentinelServerApp::startShadow(const std::vector<std::string>& symbols) {
    // Shadow: an independent consumer; no primary callbacks or recorder queue.
    // Serving (recording.source: roller): the model has no primary recorder and
    // serves roller_shadow.dir; the roller publishes into its LiveService.
    try {
        const bool serving = m_serverModel->servesRoller();
        auto& shadow = m_serverConfig.rollerShadow;
        // Roots of the primary recorder only: never the served roller root.
        shadow.protectedRoots = {m_serverConfig.recording.dir};
        if (!m_serverConfig.recording.fallbackDir.empty())
            shadow.protectedRoots.push_back(m_serverConfig.recording.fallbackDir);
        if (serving) {
            shadow.publisher = m_serverModel->rollerPublisher();
            shadow.livePublishMs = m_serverConfig.recording.livePublishMs;
        }
        const auto products = shadow.products.empty() ? symbols : rollerProducts(m_serverConfig);
        // Informational cross-connection comparison root: the primary recorder's.
        const std::filesystem::path primaryRoot =
            serving ? std::filesystem::path(m_serverConfig.recording.dir)
                    : m_serverModel->recordingDir().value_or(m_serverConfig.recording.dir);
        m_shadowRoller = std::make_unique<sentinel::roller::ShadowRoller>(shadow, products, primaryRoot, m_metrics);
        if (serving && m_shadowRoller->active()) {
            auto* roller = m_shadowRoller.get();
            m_serverModel->attachRoller(
                [roller](const std::string& product, const std::string& layer) {
                    return roller->watermarks(product, layer);
                },
                [roller](const std::string& product) { return roller->running(product); });
        }
    } catch (const std::exception& e) {
        m_metrics.counter("sentinel_roller_shadow_start_failures_total",
                          "Shadow supervisor construction failures.").inc();
        sLog_Error("Shadow roller supervisor failed: " << e.what());
    }
}

std::string SentinelServerApp::requiredRecordingProblem(ServerConfig& config) {
    auto& rc = config.recording;
    rc.fallbackDir.clear(); // never fall back to the system disk
    if (!rc.enabled) return "recording.enabled is false";
    const bool roller = rc.source == "roller";
    if (!roller && rc.source != "primary") return "unknown recording.source=" + rc.source;
    if (roller && !config.rollerShadow.enabled) return "recording.source=roller requires roller_shadow.enabled";
    // The root this process will serve: the primary recorder's or the roller's.
    const std::string dir = roller ? config.rollerShadow.outputRoot : rc.dir;
    std::error_code ec;
    const bool usable = !dir.empty() && !recording::resolveRecordingDir(dir, {}).dir.empty() &&
                        std::filesystem::is_directory(dir, ec) && !ec &&
                        ::access(dir.c_str(), R_OK | W_OK | X_OK) == 0;
    if (!usable)
        return dir + " is not mounted or accessible (errno=" + std::to_string(errno) + ")";
    return {};
}
