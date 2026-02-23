#include "GuiConfigStore.hpp"

#include <QDir>
#include <QFile>
#include <QTextStream>
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

void GuiConfigStore::setServerConfig(const ServerConfig& config, bool persist) {
    m_serverConfig = config;
    m_hasServerConfig = true;
    if (persist) {
        persistServerConfig(config);
    }
    emit serverConfigUpdated(m_serverConfig);
}

void GuiConfigStore::persistServerConfig(const ServerConfig& config) const {
    const QString dirPath = QDir::current().filePath("config");
    QDir().mkpath(dirPath);
    const QString filePath = QDir(dirPath).filePath("server_config.yaml");

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    out << "schema_version: 1\n";
    out << "timeframes_ms: [";
    for (size_t i = 0; i < config.heatmap.timeframesMs.size(); ++i) {
        if (i > 0) out << ", ";
        out << config.heatmap.timeframesMs[i];
    }
    out << "]\n";

    out << "heatmap:\n";
    out << "  grid_width: " << config.heatmap.gridWidth << "\n";
    out << "  grid_height: " << config.heatmap.gridHeight << "\n";
    out << "  tick_size: " << config.heatmap.tickSize << "\n";
    out << "  recenter_delta: " << config.heatmap.recenterDelta << "\n";
    if (config.heatmap.activeTimeframeMs > 0) {
        out << "  active_timeframe_ms: " << config.heatmap.activeTimeframeMs << "\n";
    }

    out << "orderbook:\n";
    out << "  tick_size: " << config.orderbook.tickSize << "\n";
    out << "  band_pct: " << config.orderbook.bandPct << "\n";

    out << "candles:\n";
    out << "  update_bps_fast: " << config.candles.bpsFast << "\n";
    out << "  update_bps_slow: " << config.candles.bpsSlow << "\n";
    out << "  update_tick_mult_fast: " << config.candles.tickMultFast << "\n";
    out << "  update_tick_mult_slow: " << config.candles.tickMultSlow << "\n";
    out << "  update_silence_ms_fast: " << config.candles.silenceMsFast << "\n";
    out << "  update_silence_ms_slow: " << config.candles.silenceMsSlow << "\n";
    out << "  update_volume_fast: " << config.candles.volumeFast << "\n";
    out << "  update_volume_slow: " << config.candles.volumeSlow << "\n";
    out << "  update_tick_size: " << config.candles.tickSize << "\n";

    out << "default_symbols: [";
    for (size_t i = 0; i < config.defaultSymbols.size(); ++i) {
        if (i > 0) out << ", ";
        out << QString::fromStdString(config.defaultSymbols[i]);
    }
    out << "]\n";
}
