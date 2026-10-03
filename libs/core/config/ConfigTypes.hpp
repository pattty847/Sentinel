#pragma once
#include "roller/ShadowConfig.hpp"
#include <utility>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <QMetaType>

struct ServerHeatmapConfig {
    int gridWidth = 5120;
    int gridHeight = 2048;
    double tickSize = 0.0;
    double recenterDelta = 0.01;
    // Configured candidates; the streamer currently builds only the active timeframe.
    std::vector<int64_t> timeframesMs{1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    // Empty means the server did not advertise availability (older protocol peer).
    std::vector<int64_t> servedTimeframesMs;
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

    // F1 phase 1.3: derived heatmap column persistence (slot-addressed .hmcol files).
    // Default OFF until validated in real-world soak testing. When enabled, every
    // finalized bucket on the active timeframe is appended to disk via
    // HeatmapColumnStore. See docs/private/plans/F1_HEATMAP_PERSISTENCE.md.
    bool persistenceEnabled = false;
    std::string persistenceDir{"data/heatmap"};
    int persistenceFsyncEveryNRecords = 1;
    int persistenceFsyncEveryMs = 1000;
    // F1 phase 5: per-(symbol, tf) day-file retention. 0 = keep forever; N>0
    // means delete any day file older than today UTC - N days at server start.
    int persistenceRetentionDays = 0;
};

struct ServerTradeOverlayConfig {
    int gridWidth = 512;
    int gridHeight = 2048;
    double tickSize = 5.0;
    int64_t footprintTimeframeMs = 60000;
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
    // Bound one WS connect attempt (resolve + TCP + TLS + WS handshake) and the
    // WS close handshake; a timeout counts as a failed attempt and backs off.
    int connectTimeoutMs = 20000;
    int closeTimeoutMs = 3000;
    int maxConnections = 8; // GUI-only products; pinned products are exempt (minimum 1).
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

// Recording v2 (docs/research/2026-09-recording-v2.md). Ticks are in quote
// currency; one set of values for now (BTC-USD). Per-asset ticks come later.
struct ServerRecordingConfig {
    bool enabled = false;
    // Wire capability; true only when the server's recorder started.
    bool available = false;
    std::vector<std::string> layers;
    std::vector<int64_t> timeframesMs;
    std::string dir = "data/recording";
    std::string fallbackDir;          // used when dir's volume is not mounted; empty = do not record
    double priceScale = 100.0;        // price units per 1.0 quote (0.01 increment)
    double sizeFloor = 1e-6;
    double codesPerOctave = 819.0;
    double nearTick = 1.0;
    double nearPct = 0.05;
    double deepTick = 5.0;
    double deepLowFrac = 0.25;
    double deepHighMult = 4.0;
    int64_t latenessMs = 2000;
    // Live open-minute publication interval (recording.live_publish_ms); the
    // loader clamps it to [kLivePublishMinMs, kLivePublishMaxMs].
    static constexpr int64_t kLivePublishMinMs = 250, kLivePublishMaxMs = 5000;
    int64_t livePublishMs = 500;
};

struct ServerConfig {
    // Client-side wire presence: absent capabilities must not appear as defaults.
    std::set<std::string> advertisedFields;
    bool wasAdvertised(const std::string& key) const { return advertisedFields.contains(key); }
    ServerHeatmapConfig heatmap;
    ServerOrderBookConfig orderbook;
    ServerTradeOverlayConfig tradeOverlays;
    ServerCandleGateConfig candles;
    ServerMdcConfig mdc;
    ServerTradingConfig trading;
    ServerTlsConfig tls;
    ServerRecordingConfig recording;
    sentinel::roller::ShadowConfig rollerShadow;
    uint16_t streamPort = 8080;
    std::vector<std::string> defaultSymbols{"BTC-USD"};
};

// Pinned symbols exactly as the server subscribes and records them:
// upper-case, non-empty, first occurrence kept.
inline std::vector<std::string> normalizedDefaultSymbols(const std::vector<std::string>& input) {
    std::vector<std::string> out;
    for (std::string symbol : input) {
        if (symbol.empty()) continue;
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (std::find(out.begin(), out.end(), symbol) == out.end()) out.push_back(std::move(symbol));
    }
    return out;
}

struct ClientHeatmapConfig {
    std::string renderer = "gpu";
    std::string tickMode = "auto";
    int64_t manualTick = 100;
    double minRowPx = 2, hysteresis = 0.25;
    int crossfadeMs = 150;
    bool showBandEdges = false;
    std::string palettePreset = "Electric";
    std::vector<std::pair<double, std::string>> bidGradient{{0, "#000000"}, {1, "#00ffff"}};
    std::vector<std::pair<double, std::string>> askGradient{{0, "#000000"}, {1, "#ffc800"}};
    double opacity = 1;
    uint64_t gpuCapBytes = 320ull << 20, uploadBudgetBytes = 8ull << 20;
    uint64_t decodedChunkBytes = 512ull << 20, spanSourceBytes = 256ull << 20, cpuCeilingBytes = 1024ull << 20;
    int prefetchTiles = 1, liveMinIntervalMs = 500;
    bool showTelemetry = false;
    std::string source = "legacy"; // recording requires advertised recording.available
    double gamma = 0.85;
    double contrast = 1.6;
    double shaderFloor = 0.0;
    int labelPx = 14;
    int clientCacheColumns = 1024;
    /// Time zoom on connect: screen pixels per heatmap column (2–64).
    int initialColumnPx = 8;
    /// Price zoom on connect: % of full range to show (1–100). 0 = use full range.
    int initialPricePct = 5;
    /// Display tick: rows merge (1-2-5 steps) until a row is about column width * cellAspect
    /// tall (cells stay roughly square), and never shorter than targetRowPx.
    int targetRowPx = 2;
    double cellAspect = 0.75;
    /// Recording mode colour range, in base units (BTC): sizes at or below min are dark,
    /// at or above max are brightest (log scale in between).
    double sensitivityMin = 0.05;
    double sensitivityMax = 50.0;
};

struct ClientGuiConfig {
    int apiPort = 17100;
    std::string screenshotDir = "./screenshots";
    std::string msdfFontPath;
    int axisLabelPx = 0;
    double defaultOrderQty = 1.0;
    // Process-only (--no-screener): skip the screener_server.py child on port 17200. Not read
    // from YAML. A/B and test processes use it so they never spawn or kill a port-17200 holder.
    bool startScreener = true;
};

/// TPO (market profile) overlay. Parsed values are validated by the GUI.
struct ClientTpoConfig {
    std::string layout = "collapsed";  // collapsed | split
    std::string theme = "rainbow";     // rainbow | calm | sage
    std::string session = "h24";       // ny | london | asia | australia | h24 | w1 | m1
    int periodMinutes = 30;            // letter bracket; must divide a UTC day or the session
    int sessions = 5;                  // retained sessions (current + previous), 1..8
    int rowPx = 14;                    // rows merge (1-2-5 ticks) until at least this tall
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
    ClientTpoConfig tpo;
    ClientServerConfig server;
};

Q_DECLARE_METATYPE(ServerConfig)
Q_DECLARE_METATYPE(ClientConfig)
