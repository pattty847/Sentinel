#pragma once
// Pure validation predicates for protocol message gating (INV-020, FM-014/015).
// No logging or side effects — callers wrap with their own diagnostics.
#include <nlohmann/json.hpp>
#include <string>
#include "SentinelStreamProtocol.hpp"

namespace protocol::validation {

inline int extractSchemaVersion(const nlohmann::json& msg) {
    const auto it = msg.find("schema_version");
    if (it == msg.end() || !it->is_number_integer()) return -1;
    return it->get<int>();
}

inline bool isSchemaCompatible(const nlohmann::json& msg, int supportedVersion) {
    return extractSchemaVersion(msg) == supportedVersion;
}

inline bool isGridHeightValid(int gridHeight) {
    return gridHeight > 0 && gridHeight <= SentinelProtocol::kMaxGridHeight;
}

inline bool isPayloadSizeValid(size_t bytes) {
    return bytes <= static_cast<size_t>(SentinelProtocol::kMaxPayloadBytes);
}

inline size_t estimateBase64DecodedBytes(const std::string& encoded) {
    if (encoded.empty()) return 0;
    const size_t len = encoded.size();
    const size_t blocks = (len + 3) / 4;
    size_t padding = 0;
    if (encoded[len - 1] == '=') {
        ++padding;
        if (len > 1 && encoded[len - 2] == '=') ++padding;
    }
    const size_t estimated = blocks * 3;
    return (estimated >= padding) ? (estimated - padding) : 0;
}

} // namespace protocol::validation
