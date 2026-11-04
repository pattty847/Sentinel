#pragma once
#include "levelstore/LevelLog.hpp"
#include <nlohmann/json.hpp>
#include <chrono>

namespace sentinel::roller {
// Stored with the exact recorder payload; JSON arrays preserve bytes in both JSON
// checkpoints and CBOR anchors. LevelRecorder validates the pair's length and CRC.
inline nlohmann::json levelBinding(const levelstore::DayBinding& b) {
    return {{"symbol",b.symbol},{"floor",b.floorMs},{"ceiling",b.ceilingMs},{"digits",b.priceDigits}};
}
inline levelstore::DayBinding levelBinding(const nlohmann::json& j) {
    return {j.at("symbol"),j.at("floor"),j.at("ceiling"),j.at("digits")};
}
inline nlohmann::json levelFence(levelstore::LevelRecorder& level, levelstore::LevelLog& log,
        std::optional<int64_t> clock, const std::vector<uint8_t>& recorder, const std::function<void(const char*,double)>& timing = {}) {
    auto state = level.exportState(clock,recorder);
    const auto start = std::chrono::steady_clock::now();
    log.sync();
    if (timing) timing("sync",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    const auto tail = log.durableTail();
    nlohmann::json j = {{"version",1},{"binding",levelBinding(level.binding())},{"state",state},
        {"clock",clock ? nlohmann::json(*clock) : nlohmann::json()},{"tail",nullptr}};
    if (tail) j["tail"] = {{"binding",levelBinding(tail->binding)},{"minute",tail->minute},{"image",tail->isImage}};
    return j;
}
inline void importLevelFence(levelstore::LevelRecorder& level, const nlohmann::json& j,
        std::optional<int64_t> clock, const std::vector<uint8_t>& recorder) {
    if (j.is_null()) throw std::runtime_error("C resume requires paired C state; HMC2-only fence");
    if (j.at("version") != 1 || levelBinding(j.at("binding")) != level.binding() ||
        j.at("clock") != (clock ? nlohmann::json(*clock) : nlohmann::json()))
        throw std::runtime_error("C fence binding/clock/version mismatch");
    const auto result = level.importState(j.at("state").get<std::vector<uint8_t>>(),clock,recorder);
    if (!result.ok) throw std::runtime_error("C paired state rejected: " + result.reason);
}
inline void cutLevelFence(levelstore::LevelLog& log, const nlohmann::json& j) {
    std::optional<levelstore::LevelLogTail> tail;
    if (!j.is_null() && !j.at("tail").is_null()) {
        const auto& t = j.at("tail");
        tail = levelstore::LevelLogTail{levelBinding(t.at("binding")),t.at("minute"),t.at("image")};
        if (tail->minute < 0 || tail->minute > INT64_MAX-60'000 || tail->minute % 60'000 ||
            tail->minute < tail->binding.floorMs || tail->minute >= tail->binding.ceilingMs)
            throw std::runtime_error("C fence tail range mismatch");
    }
    if (!tail) log.truncateAfter(0);
    else if (tail->isImage) log.truncateAfter(tail->minute,levelstore::CutImageAtBoundary::Keep);
    else log.truncateAfter(tail->minute + 60'000);
    if (log.durableTail() != tail) throw std::runtime_error("C durable tail does not match paired fence");
}
}
