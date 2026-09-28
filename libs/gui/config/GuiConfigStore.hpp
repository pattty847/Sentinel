#pragma once

#include <QObject>
#include <QMetaType>
#include "../../core/config/ConfigTypes.hpp"

class GuiConfigStore : public QObject {
    Q_OBJECT
public:
    static GuiConfigStore& instance();

    void setClientConfig(const ClientConfig& config);
    void setServerConfig(const ServerConfig& config);

    const ClientConfig& clientConfig() const { return m_clientConfig; }
    const ServerConfig& serverConfig() const { return m_serverConfig; }
    bool hasServerConfig() const { return m_hasServerConfig; }

signals:
    void clientConfigUpdated(const ClientConfig& config);
    void serverConfigUpdated(const ServerConfig& config);

private:
    GuiConfigStore();

    ClientConfig m_clientConfig;
    ServerConfig m_serverConfig;
    bool m_hasServerConfig = false;
};
