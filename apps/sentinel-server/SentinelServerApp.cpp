#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include "metrics/ProcessMetrics.hpp"
#include "servermodel/RecordingDir.hpp"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
bool recordingDirectoryAccessible(const std::filesystem::path& dir) {
#ifdef _WIN32
    return ::_waccess(dir.c_str(), 6) == 0; // read/write; Windows has no execute access mode
#else
    return ::access(dir.c_str(), R_OK | W_OK | X_OK) == 0;
#endif
}

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
        m_server->stop();
    }
    if (m_serverModel) m_serverModel->setReseedHandler({});
}

bool SentinelServerApp::initialize() {
    try {
        if (const auto problem = recordingSourceProblem(m_serverConfig); !problem.empty()) {
            sLog_Error("Server not started: " << problem);
            return false;
        }
        sLog_App("Initializing server components: streamPort=" << m_serverConfig.streamPort);

        // GET /ping and GET /metrics on 127.0.0.1 (ops/monitoring/README.md).
        sentinel::metrics::registerProcessMetrics(m_metrics);
        // Main-thread load while serving the journal.
        m_eventLoopLag.registerMetrics(m_metrics);
        m_eventLoopLag.start();
        m_httpServer = std::make_unique<sentinel::metrics::MetricsHttpServer>(m_metrics);
        const QByteArray portEnv = qgetenv("SENTINEL_HEALTH_PORT");
        bool ok = false;
        const int port = portEnv.toInt(&ok);
        // Explicit zero lets isolated callers bind an OS-assigned port atomically.
        const quint16 healthPort = (ok && port >= 0 && port <= 65535) ? static_cast<quint16>(port) : 8090;
        if (m_httpServer->listen(healthPort)) {
            sLog_App("Health and metrics endpoint listening on 127.0.0.1:" << m_httpServer->port() << " (/ping, /metrics)");
        } else {
            sLog_Warning("Health endpoint failed to bind on 127.0.0.1:" << healthPort
                         << " error=" << m_httpServer->errorString());
        }

        // 1. Authenticator (optional: public channels work without key.json)
        m_authenticator = std::make_unique<Authenticator>();
        // 2. Server Data Model
        try {
            m_serverModel = std::make_unique<ServerDataModel>(m_serverConfig);
        } catch (const std::exception& e) {
            sLog_Error("ServerDataModel init failed: " << e.what());
            return false;
        }

        // 3. Stream Server
        try {
            quint16 streamPort = m_serverConfig.streamPort;
            m_server = std::make_unique<SentinelStreamServer>(*m_serverModel, *m_authenticator, m_serverConfig, streamPort);
            // Start only after admission and the journal model sink are installed below.
        } catch (const std::exception& e) {
            sLog_Error("SentinelStreamServer init failed: " << e.what());
            return false;
        }
        m_serverModel->registerMetrics(m_metrics);
        m_metrics.gaugeFn("sentinel_stream_sessions", "Open client stream sessions.", {},
                          [this]() -> std::optional<double> { return double(m_server->sessionCount()); });

        const auto symbolList = rollerProducts(m_serverConfig);
        wireJournalFeed();
        // Before the stream server starts: a roller-served recording must be
        // attached before the first hello reports recording availability.
        if (!startShadow(symbolList)) return false;
        m_server->start();
        return true;
    } catch (const std::exception& e) {
        sLog_Error("Exception during initialization: " << e.what());
        return false;
    }
}

void SentinelServerApp::wireJournalFeed() {
    const auto products = rollerProducts(m_serverConfig);
    sLog_App("Live feed: journal (capture fan-out); no Coinbase WebSocket: products=" << products.size());
    m_server->setFeedAdmissionHandler([products](const std::string& symbol) {
        if (std::find(products.begin(), products.end(), symbol) != products.end())
            return SentinelStreamServer::FeedAdmission::Accepted;
        sLog_Data("Feed admission refused, product not captured: symbol=" << symbol);
        return SentinelStreamServer::FeedAdmission::InvalidProduct;
    });
}

// Roller worker threads: each call is one queued hand-off to the model's thread.
sentinel::roller::ModelSink SentinelServerApp::journalModelSink() {
    QPointer<ServerDataModel> modelPtr(m_serverModel.get());
    // Shared, read-only after construction: no per-record QString conversion.
    auto names = std::make_shared<std::map<std::string, QString>>();
    for (const auto& product : rollerProducts(m_serverConfig))
        names->emplace(product, QString::fromStdString(product));
    const auto name = [names](const std::string& product) {
        const auto it = names->find(product);
        return it != names->end() ? it->second : QString::fromStdString(product);
    };
    sentinel::roller::ModelSink sink;
    sink.snapshot = [modelPtr, name](const std::string& product, std::vector<OrderBookLevel> bids,
                                     std::vector<OrderBookLevel> asks, int64_t envelopeMs) {
        safeInvoke(modelPtr, [id = name(product), bids = std::move(bids), asks = std::move(asks),
                              envelopeMs](ServerDataModel& model) {
            model.onLiveOrderBookInitialized(id, bids, asks, static_cast<qint64>(envelopeMs));
        });
    };
    sink.updates = [modelPtr, name](const std::string& product, std::vector<BookLevelUpdate> updates,
                                    int64_t exchangeMs) {
        safeInvoke(modelPtr, [id = name(product), updates = std::move(updates), exchangeMs](ServerDataModel& model) {
            model.onLiveOrderBookLevelUpdates(id, updates, static_cast<qint64>(exchangeMs));
        });
    };
    sink.invalidate = [modelPtr, name](const std::string& product, const std::string& reason) {
        safeInvoke(modelPtr, [id = name(product), reason = QString::fromStdString(reason)](ServerDataModel& model) {
            model.onLiveOrderBookInvalidated(id, reason);
        });
    };
    sink.trade = [modelPtr](const Trade& trade) {
        safeInvoke(modelPtr, [trade](ServerDataModel& model) { model.onTrade(trade); });
    };
    sink.connection = [modelPtr](const std::string& product, bool connected) {
        safeInvoke(modelPtr, [product, connected](ServerDataModel& model) {
            sLog_App("Journal live feed: product=" << product << " " << (connected ? "CONNECTED" : "DISCONNECTED"));
            model.onMarketDataConnectionChanged(product, connected);
        });
    };
    sink.watermark = [modelPtr](const std::string& product, int64_t journalMs) {
        safeInvoke(modelPtr, [product, journalMs](ServerDataModel& model) { model.onFeedWatermark(product, journalMs); });
    };
    sink.metadata = [modelPtr](const std::string& product, const nlohmann::json& metadata) {
        safeInvoke(modelPtr, [product, metadata](ServerDataModel& model) { model.onFeedMetadata(product, metadata); });
    };
    sink.candleRestore = [modelPtr](const std::string& product,
                                    std::optional<sentinel::roller::CandleRestore> restore) {
        // Shared so the queued functor stays copyable without copying the bars.
        auto held = std::make_shared<std::optional<sentinel::roller::CandleRestore>>(std::move(restore));
        safeInvoke(modelPtr, [product, held](ServerDataModel& model) {
            model.onCandleHistoryRestored(product, std::move(*held));
        });
    };
    return sink;
}

bool SentinelServerApp::startShadow(const std::vector<std::string>& symbols) {
    // The roller serves history and publishes into the model's LiveService.
    try {
        const bool serving = m_serverModel->servesRoller();
        auto& shadow = m_serverConfig.rollerShadow;
        // Protect configured archival roots from accidental output aliasing.
        shadow.protectedRoots = {m_serverConfig.recording.dir};
        if (!m_serverConfig.recording.fallbackDir.empty())
            shadow.protectedRoots.push_back(m_serverConfig.recording.fallbackDir);
        if (serving) {
            shadow.publisher = m_serverModel->rollerPublisher();
            shadow.retractLive = m_serverModel->rollerRetract();
            shadow.ensureLiveFinal = m_serverModel->rollerEnsureFinal();
            shadow.livePublishMs = m_serverConfig.recording.livePublishMs;
        }
        shadow.model = journalModelSink();
        const auto products = shadow.products.empty() ? symbols : rollerProducts(m_serverConfig);
        // Restart rebuild of the RAM-served trade timeframes (100 ms, 1 s).
        shadow.candleTimeframesMs.clear();
        for (const int64_t tf : {int64_t{100}, int64_t{1000}})
            if (std::find(m_serverConfig.chart.timeframesMs.begin(), m_serverConfig.chart.timeframesMs.end(), tf) !=
                m_serverConfig.chart.timeframesMs.end())
                shadow.candleTimeframesMs.push_back(tf);
        shadow.candleHistoryBars = m_serverConfig.chart.historyBars;
        // Same condition as the roller's rebuild: every product it rebuilds
        // (or refuses) posts exactly once, which ends its warming.
        if (shadow.model.candleRestore && shadow.candleRestoreForTest && !shadow.candleTimeframesMs.empty())
            m_serverModel->setCandleHistoryWarming(products);
        m_shadowRoller = std::make_unique<sentinel::roller::ShadowRoller>(shadow, products, m_metrics);
        if (serving && m_shadowRoller->active()) {
            auto* roller = m_shadowRoller.get();
            m_serverModel->attachRoller(
                [roller](const std::string& product, const std::string& layer) {
                    return roller->watermarks(product, layer);
                },
                [roller](const std::string& product) { return roller->running(product); });
        }
        {
            if (!m_shadowRoller->active()) throw std::runtime_error("the journal live feed needs the roller");
            auto* roller = m_shadowRoller.get();
            m_serverModel->setReseedHandler([roller](const std::string& product) { roller->requestReseed(product); });
        }
    } catch (const std::exception& e) {
        m_metrics.counter("sentinel_roller_shadow_start_failures_total",
                          "Shadow supervisor construction failures.").inc();
        sLog_Error("Shadow roller supervisor failed: " << e.what());
        return false;
    }
    return true;
}

std::string SentinelServerApp::requiredRecordingProblem(ServerConfig& config) {
    auto& rc = config.recording;
    rc.fallbackDir.clear(); // never fall back to the system disk
    if (!rc.enabled) return "recording.enabled is false";
    if (const auto problem = recordingSourceProblem(config); !problem.empty()) return problem;
    const std::string dir = config.rollerShadow.outputRoot;
    std::error_code ec;
    const bool usable = !dir.empty() && !recording::resolveRecordingDir(dir, {}).dir.empty() &&
                        std::filesystem::is_directory(dir, ec) && !ec &&
                        recordingDirectoryAccessible(dir);
    if (!usable)
        return dir + " is not mounted or accessible (errno=" + std::to_string(errno) + ")";
    return {};
}
