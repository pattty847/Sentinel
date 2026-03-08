#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QStringList>
#include <algorithm>
#include <cctype>

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
    if (m_marketDataCore) {
        m_marketDataCore->stop();
    }
}

bool SentinelServerApp::initialize() {
    try {
        sLog_App("Initializing Server Components...");

        // Health endpoint (HTTP over TCP)
        m_healthServer = new QTcpServer(this);
        const QByteArray portEnv = qgetenv("SENTINEL_HEALTH_PORT");
        bool ok = false;
        const int port = portEnv.toInt(&ok);
        const quint16 healthPort = (ok && port > 0) ? static_cast<quint16>(port) : 8090;
        if (m_healthServer->listen(QHostAddress::LocalHost, healthPort)) {
            sLog_App("Health endpoint listening on 127.0.0.1:" << healthPort);
            connect(m_healthServer, &QTcpServer::newConnection, this, [this]() {
                while (m_healthServer->hasPendingConnections()) {
                    QTcpSocket* socket = m_healthServer->nextPendingConnection();
                    connect(socket, &QTcpSocket::readyRead, this, [socket]() {
                        const QByteArray request = socket->readAll();
                        const QByteArray firstLine = request.left(request.indexOf('\n')).trimmed();
                        const bool isPing = firstLine.startsWith("GET /ping");
                        const QByteArray body = isPing ? QByteArray("OK") : QByteArray("Not Found");
                        const QByteArray status = isPing ? QByteArray("200 OK") : QByteArray("404 Not Found");
                        QByteArray response;
                        response += "HTTP/1.1 " + status + "\r\n";
                        response += "Content-Type: text/plain\r\n";
                        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                        response += "Connection: close\r\n\r\n";
                        response += body;
                        socket->write(response);
                        socket->flush();
                        socket->disconnectFromHost();
                    });
                    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
                }
            });
        } else {
            sLog_Warning("Health endpoint failed to bind on 127.0.0.1:" << healthPort);
        }

        // 1. Authenticator (optional: public channels work without key.json)
        m_authenticator = std::make_unique<Authenticator>();
        // Only send JWT when we have credentials and config enables it (user/futures channels need auth)
        m_serverConfig.mdc.useJwt = m_authenticator->hasCredentials() && m_serverConfig.mdc.useJwt;

        // 2. Market Data Core
        try {
            m_marketDataCore = std::make_unique<MarketDataCoreEngine>(*m_authenticator, m_serverConfig.mdc);
        } catch (const std::exception& e) {
            sLog_Error("MarketDataCoreEngine init failed: " << e.what());
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
            m_server->start();
        } catch (const std::exception& e) {
            sLog_Error("SentinelStreamServer init failed: " << e.what());
            return false;
        }
        
        // Connect MarketDataCoreEngine -> ServerDataModel via queued invocations
        QPointer<ServerDataModel> modelPtr(m_serverModel.get());
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
                                                                const std::vector<OrderBookLevel>& asks) {
            QString productIdQ = QString::fromStdString(productId);
            std::vector<OrderBookLevel> bidsCopy = bids;
            std::vector<OrderBookLevel> asksCopy = asks;
            safeInvoke(modelPtr, [productIdQ, bidsCopy = std::move(bidsCopy), asksCopy = std::move(asksCopy)](ServerDataModel& model) mutable {
                model.onLiveOrderBookInitialized(productIdQ, bidsCopy, asksCopy);
            });
        });
        
        // Wire up callbacks for logging
        m_marketDataCore->onConnectionStatus([](bool connected){
            sLog_App("MarketDataCore Connection: " << (connected ? "CONNECTED" : "DISCONNECTED"));
        });
        
        m_marketDataCore->onError([](const std::string& error){
            sLog_Error("MarketDataCore Error: " << error);
        });

        m_marketDataCore->onLatency([this](int latencyMs) {
            // Log Coinbase WebSocket latency (server time - Coinbase timestamp)
            // This runs frequently, so throttle logging
            static int lastLoggedLatency = -1;
            static auto lastLogTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();

            // Log if latency changed significantly or every 10 seconds
            if (std::abs(latencyMs - lastLoggedLatency) > 5 || elapsed >= 10) {
                sLog_Data("Coinbase WebSocket Latency: " << latencyMs << " ms");
                lastLoggedLatency = latencyMs;
                lastLogTime = now;
            }
            if (m_server)
                m_server->broadcastCoinbaseLatency(latencyMs);
        });

        QObject::connect(m_server.get(), &SentinelStreamServer::clientSubscribed, this,
                         [this](const QString& symbol) {
                             if (!m_marketDataCore) {
                                 return;
                             }
                             sLog_App("Client subscribe: " << symbol);
                             m_marketDataCore->subscribeToSymbols({symbol.toStdString()});
                         }, Qt::QueuedConnection);

        QObject::connect(m_server.get(), &SentinelStreamServer::clientUnsubscribed, this,
                         [this](const QString& symbol) {
                             if (!m_marketDataCore) {
                                 return;
                             }
                             sLog_App("Client unsubscribe: " << symbol);
                             const std::string native = symbol.toStdString();
                             if (m_defaultSymbols.find(native) != m_defaultSymbols.end()) {
                                 sLog_App("Skipping unsubscribe for pinned default symbol: " << symbol);
                                 return;
                             }
                             m_marketDataCore->unsubscribeFromSymbols({native});
                         }, Qt::QueuedConnection);

        // Start connection
        m_marketDataCore->start();

        auto parseDefaultSymbols = [](const std::vector<std::string>& input) {
            std::vector<std::string> out;
            out.reserve(input.size());
            std::unordered_set<std::string> seen;
            for (const auto& sym : input) {
                if (sym.empty()) {
                    continue;
                }
                std::string normalized = sym;
                std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (seen.insert(normalized).second) {
                    out.push_back(normalized);
                }
            }
            return out;
        };

        const auto normalizedSymbols = parseDefaultSymbols(m_serverConfig.defaultSymbols);
        std::vector<std::string> symbolList;
        symbolList.reserve(normalizedSymbols.size());
        for (const auto& sym : normalizedSymbols) {
            if (m_defaultSymbols.insert(sym).second) {
                symbolList.push_back(sym);
            }
        }

        if (!symbolList.empty()) {
            QTimer::singleShot(0, this, [this, symbolList]() mutable {
                if (!m_marketDataCore) {
                    return;
                }
                sLog_App("Server default subscribe: " << QString::fromStdString(symbolList.front())
                             << (symbolList.size() > 1 ? " (+more)" : ""));
                m_marketDataCore->subscribeToSymbols(symbolList);
            });
        }

        return true;
    } catch (const std::exception& e) {
        sLog_Error("Exception during initialization: " << e.what());
        return false;
    }
}
