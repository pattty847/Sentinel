#include "SentinelServerApp.hpp"
#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QStringList>

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

        m_marketDataCore->onLatency([](int latencyMs) {
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

        // Default subscribe to symbols so server builds history before clients connect.
        const QByteArray defaultEnv = qgetenv("SENTINEL_SERVER_DEFAULT_SYMBOLS");
        QString symbolsSpec = defaultEnv.isEmpty() ? QStringLiteral("BTC-USD") : QString::fromUtf8(defaultEnv);
        symbolsSpec.replace(',', ' ');
        symbolsSpec = symbolsSpec.simplified();
        const QStringList symbols = symbolsSpec.isEmpty()
            ? QStringList()
            : symbolsSpec.split(' ', Qt::SkipEmptyParts);

        std::vector<std::string> symbolList;
        symbolList.reserve(static_cast<size_t>(symbols.size()));
        for (const auto& sym : symbols) {
            const QString normalized = sym.trimmed().toUpper();
            if (normalized.isEmpty()) {
                continue;
            }
            const std::string native = normalized.toStdString();
            if (m_defaultSymbols.insert(native).second) {
                symbolList.push_back(native);
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
