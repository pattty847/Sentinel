#pragma once

#include <QObject>
#include <QTcpServer>

class SecBackendServer : public QObject {
    Q_OBJECT

public:
    explicit SecBackendServer(QObject* parent = nullptr);
    ~SecBackendServer();

    bool start(quint16 port);
    void stop();
    bool isRunning() const;

private:
    QTcpServer* m_server = nullptr;
};
