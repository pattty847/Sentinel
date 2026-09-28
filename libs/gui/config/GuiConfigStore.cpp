#include "GuiConfigStore.hpp"
#include "SentinelLogging.hpp"

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
        } else {
            sLog_Warning("Invalid SENTINEL_HEATMAP_GAMMA ignored: value=" << gammaEnv);
        }
    }
    ok = false;
    const QByteArray contrastEnv = qgetenv("SENTINEL_HEATMAP_CONTRAST");
    if (!contrastEnv.isEmpty()) {
        const double contrast = contrastEnv.toDouble(&ok);
        if (ok && contrast > 0.0) {
            m_clientConfig.heatmap.contrast = contrast;
        } else {
            sLog_Warning("Invalid SENTINEL_HEATMAP_CONTRAST ignored: value=" << contrastEnv);
        }
    }
    ok = false;
    const QByteArray floorEnv = qgetenv("SENTINEL_HEATMAP_SHADER_FLOOR");
    if (!floorEnv.isEmpty()) {
        const double floorVal = floorEnv.toDouble(&ok);
        if (ok && floorVal >= 0.0 && floorVal <= 1.0) {
            m_clientConfig.heatmap.shaderFloor = floorVal;
        } else {
            sLog_Warning("Invalid SENTINEL_HEATMAP_SHADER_FLOOR ignored: value=" << floorEnv);
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
    sLog_Data("Server config received: grid=" << config.heatmap.gridWidth << "x"
              << config.heatmap.gridHeight
              << " activeTfMs=" << config.heatmap.activeTimeframeMs
              << " timeframes=" << config.heatmap.timeframesMs.size()
              << " defaultSymbol="
              << (config.defaultSymbols.empty() ? std::string() : config.defaultSymbols.front()));
    emit serverConfigUpdated(m_serverConfig);
}
