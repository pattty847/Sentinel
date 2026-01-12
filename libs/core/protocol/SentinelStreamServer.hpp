#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <unordered_set>
#include <mutex>
#include <thread>
#include "../servermodel/ServerDataModel.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

class SentinelStreamServer : public QObject {
    Q_OBJECT
public:
    explicit SentinelStreamServer(ServerDataModel& model, int port, QObject* parent = nullptr);
    ~SentinelStreamServer();

    void start();
    void stop();

private:
    void doAccept();
    
    ServerDataModel& m_model;
    int m_port;
    
    net::io_context m_ioc;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

