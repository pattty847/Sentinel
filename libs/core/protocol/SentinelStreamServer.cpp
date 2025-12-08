#include "SentinelStreamServer.hpp"
#include "SentinelLogging.hpp"

SentinelStreamServer::SentinelStreamServer(ServerDataModel& model, int port, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_port(port)
{
}

SentinelStreamServer::~SentinelStreamServer() {
    stop();
}

void SentinelStreamServer::start() {
    if (m_running) return;
    
    try {
        m_running = true;
        
        // Setup acceptor
        tcp::endpoint endpoint(tcp::v4(), m_port);
        m_acceptor = std::make_unique<tcp::acceptor>(m_ioc);
        m_acceptor->open(endpoint.protocol());
        m_acceptor->set_option(net::socket_base::reuse_address(true));
        m_acceptor->bind(endpoint);
        m_acceptor->listen();
        
        sLog_App("SentinelStreamServer: Listening on port " << m_port);
        
        doAccept();
        
        m_thread = std::thread([this] {
            while (m_running) {
                try {
                    m_ioc.run();
                } catch (const std::exception& e) {
                    sLog_Error("SentinelStreamServer I/O error: " << e.what());
                    m_ioc.restart();
                }
            }
        });
        
    } catch (const std::exception& e) {
        sLog_Error("SentinelStreamServer start failed: " << e.what());
        m_running = false;
    }
}

void SentinelStreamServer::stop() {
    m_running = false;
    if (m_acceptor) {
        m_acceptor->close();
    }
    m_ioc.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SentinelStreamServer::doAccept() {
    // Placeholder for async accept loop
    // m_acceptor->async_accept(...)
}

