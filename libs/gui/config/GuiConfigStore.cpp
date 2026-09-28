#include "GuiConfigStore.hpp"

#include <QtGlobal>
#include <QString>

GuiConfigStore& GuiConfigStore::instance() {
    static GuiConfigStore store;
    return store;
}

GuiConfigStore::GuiConfigStore()
    : QObject(nullptr) {
}

void GuiConfigStore::setClientConfig(const ClientConfig& config) {
    m_clientConfig = config;
    bool ok = false;
    const QByteArray gammaEnv = qgetenv("SENTINEL_HEATMAP_GAMMA");
    if (!gammaEnv.isEmpty()) {
        const double gamma = gammaEnv.toDouble(&ok);
        if (ok && gamma > 0.0) {
            m_clientConfig.heatmap.gamma = gamma;
        }
    }
    ok = false;
    const QByteArray contrastEnv = qgetenv("SENTINEL_HEATMAP_CONTRAST");
    if (!contrastEnv.isEmpty()) {
        const double contrast = contrastEnv.toDouble(&ok);
        if (ok && contrast > 0.0) {
            m_clientConfig.heatmap.contrast = contrast;
        }
    }
    ok = false;
    const QByteArray floorEnv = qgetenv("SENTINEL_HEATMAP_SHADER_FLOOR");
    if (!floorEnv.isEmpty()) {
        const double floorVal = floorEnv.toDouble(&ok);
        if (ok && floorVal >= 0.0 && floorVal <= 1.0) {
            m_clientConfig.heatmap.shaderFloor = floorVal;
        }
    }
    const int labelPx = qEnvironmentVariableIntValue("SENTINEL_HEATMAP_LABEL_PX");
    if (labelPx > 0) {
        m_clientConfig.heatmap.labelPx = labelPx;
    }
    const int cacheCols = qEnvironmentVariableIntValue("SENTINEL_HEATMAP_CLIENT_CACHE_COLUMNS");
    if (cacheCols > 0) {
        m_clientConfig.heatmap.clientCacheColumns = cacheCols;
    }
    emit clientConfigUpdated(m_clientConfig);
}

void GuiConfigStore::setServerConfig(const ServerConfig& config) {
    // Held in memory only: config/server_config.yaml belongs to the server,
    // and the client must never rewrite it.
    m_serverConfig = config;
    m_hasServerConfig = true;
    emit serverConfigUpdated(m_serverConfig);
}
