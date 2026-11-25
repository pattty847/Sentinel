#pragma once
// Client control plane for SCK1. No server/store dependencies. Snapshots replace
// an instrument atomically; epochs are identities, never ordered counters.
#include "ChunkFrame.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <optional>

namespace protocol::chunks {
struct Run {
    int64_t fromMs = 0, toMs = 0;
    bool operator==(const Run&) const = default;
};
struct Level {
    int64_t levelMs = 0, chunkSpanMs = 0;
    std::optional<int64_t> oldestMs, latestMs;
    std::vector<Run> sealed;
    bool contains(int64_t start) const;
    bool operator==(const Level&) const = default;
};
struct Channel {
    uint64_t epoch = 0;
    std::vector<Level> levels;
    bool operator==(const Channel&) const = default;
};
struct Availability {
    std::string instrument;
    int64_t liveFromMs = 0;
    std::map<std::string, Channel> channels;
    const Level* level(const Key& key) const;
    bool contains(const Key& key) const;
    uint64_t epoch(const Key& key) const;
    bool operator==(const Availability&) const = default;
};
// Throws invalid_argument for invalid outbound arguments or inbound JSON.
nlohmann::json buildRequest(uint64_t id, const Key& key, const std::vector<int64_t>& starts,
                            const std::vector<std::optional<uint64_t>>& hashes = {});
nlohmann::json buildSubscribe(const std::vector<std::string>& instruments);
Availability parseAvailability(const nlohmann::json& message);
// Rows are requested at step 1 but answered at their stored power-of-two step.
Key requestKey(Key key);
bool matches(const Key& request, const Key& reply);
} // namespace protocol::chunks
