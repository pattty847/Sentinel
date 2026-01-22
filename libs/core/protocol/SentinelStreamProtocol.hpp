#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// V0 JSON Protocol
namespace protocol {

enum class MessageType {
    Subscribe,
    Unsubscribe,
    Snapshot,
    SliceBatch,
    HeatmapSlice,
    TickDelta, // Maybe for later
    Error,
    Unknown
};

inline std::string toString(MessageType t) {
    switch (t) {
        case MessageType::Subscribe: return "subscribe";
        case MessageType::Unsubscribe: return "unsubscribe";
        case MessageType::Snapshot: return "snapshot";
        case MessageType::SliceBatch: return "slice_batch";
        case MessageType::HeatmapSlice: return "heatmap_slice";
        case MessageType::Error: return "error";
        default: return "unknown";
    }
}

inline MessageType fromString(const std::string& s) {
    if (s == "subscribe") return MessageType::Subscribe;
    if (s == "unsubscribe") return MessageType::Unsubscribe;
    if (s == "snapshot") return MessageType::Snapshot;
    if (s == "slice_batch") return MessageType::SliceBatch;
    if (s == "heatmap_slice") return MessageType::HeatmapSlice;
    if (s == "error") return MessageType::Error;
    return MessageType::Unknown;
}

} // namespace protocol

