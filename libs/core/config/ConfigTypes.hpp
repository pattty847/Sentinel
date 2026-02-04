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
    std::vector<int64_t> timeframesMs{1000, 60000, 3600000, 86400000};
    int64_t activeTimeframeMs = 0;
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

struct ServerConfig {
    ServerHeatmapConfig heatmap;
    ServerOrderBookConfig orderbook;
    ServerCandleGateConfig candles;
    std::vector<std::string> defaultSymbols{"BTC-USD"};
};

struct ClientHeatmapConfig {
    double gamma = 1.05;
    double contrast = 1.15;
    double shaderFloor = 0.01;
    int labelPx = 14;
    int clientCacheColumns = 0;
};

struct ClientGuiConfig {
    int apiPort = 17100;
    std::string screenshotDir = "./screenshots";
    std::string msdfFontPath;
};

struct ClientServerConfig {
    std::string host = "127.0.0.1";
    std::string port = "8080";
};

struct ClientConfig {
    ClientHeatmapConfig heatmap;
    ClientGuiConfig gui;
    ClientServerConfig server;
};

Q_DECLARE_METATYPE(ServerConfig)
Q_DECLARE_METATYPE(ClientConfig)
