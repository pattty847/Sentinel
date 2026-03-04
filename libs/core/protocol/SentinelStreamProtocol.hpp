#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// V0 JSON Protocol
namespace protocol {

namespace SentinelProtocol {
constexpr int kServerConfigSchemaVersion = 1;
constexpr int kHeatmapSchemaVersion = 1;
constexpr int kCandleSchemaVersion = 1;
constexpr int kFootprintSchemaVersion = 1;
constexpr int kTpoSchemaVersion = 1;
constexpr int kVolumeProfileSchemaVersion = 1;
constexpr int kMaxGridHeight = 65536;
constexpr int kMaxPayloadBytes = 262144; // 256 KiB
}

enum class MessageType {
    Subscribe,
    Unsubscribe,
    ServerConfig,
    Snapshot,
    SliceBatch,
    HeatmapSlice,
    HeatmapHistoryRequest,
    HeatmapHistoryChunk,
    CandleHistoryRequest,
    CandleHistoryChunk,
    CandleBarUpdate,
    CandleBarClosed,
    FootprintConfig,
    FootprintSlice,
    FootprintHistoryRequest,
    FootprintHistoryChunk,
    TpoSlice,
    TpoHistoryRequest,
    TpoHistoryChunk,
    VolumeProfileSlice,
    TickDelta, // Maybe for later
    ScreenerRequest,
    ScreenerUpdate,
    TradeCommand,
    OrderUpdate,
    PositionUpdate,
    Error,
    Unknown
};

inline std::string toString(MessageType t) {
    switch (t) {
        case MessageType::Subscribe: return "subscribe";
        case MessageType::Unsubscribe: return "unsubscribe";
        case MessageType::ServerConfig: return "server_config";
        case MessageType::Snapshot: return "snapshot";
        case MessageType::SliceBatch: return "slice_batch";
        case MessageType::HeatmapSlice: return "heatmap_slice";
        case MessageType::HeatmapHistoryRequest: return "heatmap_history_request";
        case MessageType::HeatmapHistoryChunk: return "heatmap_history_chunk";
        case MessageType::CandleHistoryRequest: return "candle_history_request";
        case MessageType::CandleHistoryChunk: return "candle_history_chunk";
        case MessageType::CandleBarUpdate: return "candle_bar_update";
        case MessageType::CandleBarClosed: return "candle_bar_closed";
        case MessageType::FootprintConfig: return "footprint_config";
        case MessageType::FootprintSlice: return "footprint_slice";
        case MessageType::FootprintHistoryRequest: return "footprint_history_request";
        case MessageType::FootprintHistoryChunk: return "footprint_history_chunk";
        case MessageType::TpoSlice: return "tpo_slice";
        case MessageType::TpoHistoryRequest: return "tpo_history_request";
        case MessageType::TpoHistoryChunk: return "tpo_history_chunk";
        case MessageType::VolumeProfileSlice: return "volume_profile_slice";
        case MessageType::ScreenerRequest: return "screener_request";
        case MessageType::ScreenerUpdate:  return "screener_update";
        case MessageType::TradeCommand: return "trade_command";
        case MessageType::OrderUpdate: return "order_update";
        case MessageType::PositionUpdate: return "position_update";
        case MessageType::Error: return "error";
        default: return "unknown";
    }
}

inline MessageType fromString(const std::string& s) {
    if (s == "subscribe") return MessageType::Subscribe;
    if (s == "unsubscribe") return MessageType::Unsubscribe;
    if (s == "server_config") return MessageType::ServerConfig;
    if (s == "snapshot") return MessageType::Snapshot;
    if (s == "slice_batch") return MessageType::SliceBatch;
    if (s == "heatmap_slice") return MessageType::HeatmapSlice;
    if (s == "heatmap_history_request") return MessageType::HeatmapHistoryRequest;
    if (s == "heatmap_history_chunk") return MessageType::HeatmapHistoryChunk;
    if (s == "candle_history_request") return MessageType::CandleHistoryRequest;
    if (s == "candle_history_chunk") return MessageType::CandleHistoryChunk;
    if (s == "candle_bar_update") return MessageType::CandleBarUpdate;
    if (s == "candle_bar_closed") return MessageType::CandleBarClosed;
    if (s == "footprint_config") return MessageType::FootprintConfig;
    if (s == "footprint_slice") return MessageType::FootprintSlice;
    if (s == "footprint_history_request") return MessageType::FootprintHistoryRequest;
    if (s == "footprint_history_chunk") return MessageType::FootprintHistoryChunk;
    if (s == "tpo_slice") return MessageType::TpoSlice;
    if (s == "tpo_history_request") return MessageType::TpoHistoryRequest;
    if (s == "tpo_history_chunk") return MessageType::TpoHistoryChunk;
    if (s == "volume_profile_slice") return MessageType::VolumeProfileSlice;
    if (s == "screener_request") return MessageType::ScreenerRequest;
    if (s == "screener_update")  return MessageType::ScreenerUpdate;
    if (s == "trade_command") return MessageType::TradeCommand;
    if (s == "order_update") return MessageType::OrderUpdate;
    if (s == "position_update") return MessageType::PositionUpdate;
    if (s == "error") return MessageType::Error;
    return MessageType::Unknown;
}

} // namespace protocol

