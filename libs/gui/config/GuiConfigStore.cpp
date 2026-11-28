#include "GuiConfigStore.hpp"
#include "SentinelLogging.hpp"

#include <QtGlobal>
#include <QString>

// Server-advertised timeframes and native grids are authoritative; client
// display preferences must not silently override the server data contract.
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
    emit clientConfigUpdated(m_clientConfig);
}

// Received server configuration is memory-only. The GUI must never write it
// back to config/server_config.yaml or change server persistence settings.
void GuiConfigStore::setServerConfig(const ServerConfig& config) {
    m_serverConfig = config;
    m_hasServerConfig = true;
    sLog_Data("Server config received: retentionColumns=" << config.tradeOverlays.retentionColumns
              << " defaultTfMs=" << config.chart.defaultTimeframeMs
              << " timeframes=" << config.chart.timeframesMs.size()
              << " defaultSymbol="
              << (config.defaultSymbols.empty() ? std::string() : config.defaultSymbols.front()));
    emit serverConfigUpdated(m_serverConfig);
}
