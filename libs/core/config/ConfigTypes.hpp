#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <QMetaType>

struct ServerHeatmapConfig {
    int gridWidth = 5120;
    int gridHeight = 2048;
    double tickSize = 0.0;
    double recenterDelta = 0.01;
    // All 7 timeframes that the toolbar exposes: 1s, 1m, 5m, 15m, 1h, 4h, 1D.
    // The server pre-builds a TWAP heatmap ring buffer for each of these so
    // the client can switch between any of them without waiting for data.
    std::vector<int64_t> timeframesMs{1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    int64_t activeTimeframeMs = 0;
    double bandFast = 0.15;
    double bandMedium = 0.25;
    double bandSlow = 0.35;
    std::string intensityMode{"log"};
    std::string intensityMaxMode{"running"};
    double intensityMaxDecay = 0.995;
    double intensityLogScale = 1000.0;
    double intensityPower = 0.4;
    double intensityFloor = 0.001;
    bool debugSliceLog = false;
};

struct ServerOrderBookConfig {
    double tickSize = 0.10;
    double bandPct = 0.30;
};

struct ServerCandleGateConfig {
    double bpsFast = 0.00005;
    double bpsSlow = 0.0002;
    int tickMultFast = 1;
    int tickMultSlow = 2;
    int64_t silenceMsFast = 200;
    int64_t silenceMsSlow = 1000;
    double volumeFast = 0.0;
    double volumeSlow = 0.0;
    double tickSize = 0.0;
};

struct ServerMdcConfig {
    std::string host = "advanced-trade-ws.coinbase.com";
    std::string port = "443";
    std::string target = "/v1";
    bool useJwt = false;
    std::string sslCaBundle;
};

struct ServerTradingConfig {
    std::string mode = "paper";
    double slippageBps = 2.0;
};

struct ServerTlsConfig {
    // PEM cert chain and private key for the internal WSS stream server.
    // Generate with: scripts/certs/gen-certs.ps1 (Windows) or gen-certs.sh (Linux/Mac).
    std::string certFile = "certs/sentinel-server.crt";
    std::string keyFile  = "certs/sentinel-server.key";
};

struct ServerConfig {
    ServerHeatmapConfig heatmap;
    ServerOrderBookConfig orderbook;
    ServerCandleGateConfig candles;
    ServerMdcConfig mdc;
    ServerTradingConfig trading;
    ServerTlsConfig tls;
    uint16_t streamPort = 8080;
    std::vector<std::string> defaultSymbols{"BTC-USD"};
};

struct ClientHeatmapConfig {
    double gamma = 0.85;
    double contrast = 1.6;
    double shaderFloor = 0.0;
    int labelPx = 14;
    int clientCacheColumns = 0;
};

struct ClientGuiConfig {
    int apiPort = 17100;
    std::string screenshotDir = "./screenshots";
    std::string msdfFontPath;
    int axisLabelPx = 0;
    double defaultOrderQty = 1.0;
};

struct ClientServerConfig {
    std::string host = "127.0.0.1";
    std::string port = "8080";
    // Path to the server's self-signed cert to use as a trusted CA.
    // Set to empty string to disable cert verification (insecure, for dev only).
    std::string caFile = "certs/sentinel-server.crt";
};

struct ClientConfig {
    ClientHeatmapConfig heatmap;
    ClientGuiConfig gui;
    ClientServerConfig server;
};

Q_DECLARE_METATYPE(ServerConfig)
Q_DECLARE_METATYPE(ClientConfig)
